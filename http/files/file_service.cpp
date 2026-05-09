#include "../core/connection.h"
#include "file_helpers.h"
#include "file_store.h"
#include "multipart_parser.h"
#include "../../infra/storage/storage.h"
#include "../../repo/mysql/file_repository.h"
#include "../../service/files/file_service.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <sys/stat.h>

using namespace std;

namespace
{
const int kDefaultListLimit = 20;
const int kMaxListLimit = 100;
bool ends_with_ignore_case(const string &value, const string &suffix)
{
    if (suffix.size() > value.size())
    {
        return false;
    }

    const size_t offset = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        if (tolower(static_cast<unsigned char>(value[offset + i])) !=
            tolower(static_cast<unsigned char>(suffix[i])))
        {
            return false;
        }
    }
    return true;
}

bool starts_with_html_document(const string &path)
{
    ifstream input(path.c_str(), ios::in | ios::binary);
    if (!input.is_open())
    {
        return false;
    }

    char buffer[256];
    input.read(buffer, sizeof(buffer));
    const streamsize count = input.gcount();
    if (count <= 0)
    {
        return false;
    }

    string sample(buffer, static_cast<size_t>(count));
    size_t pos = 0;
    while (pos < sample.size())
    {
        unsigned char ch = static_cast<unsigned char>(sample[pos]);
        if (ch == 0xEF && pos + 2 < sample.size() &&
            static_cast<unsigned char>(sample[pos + 1]) == 0xBB &&
            static_cast<unsigned char>(sample[pos + 2]) == 0xBF)
        {
            pos += 3;
            continue;
        }
        if (!isspace(ch))
        {
            break;
        }
        ++pos;
    }

    const string prefix = sample.substr(pos);
    return prefix.compare(0, 15, "<!DOCTYPE html") == 0 ||
           prefix.compare(0, 5, "<html") == 0 ||
           prefix.compare(0, 14, "<!doctype html") == 0;
}

void normalize_download_metadata(const string &path, string &content_type, string &download_name)
{
    if (ends_with_ignore_case(download_name, ".html") || ends_with_ignore_case(download_name, ".htm"))
    {
        if (content_type.empty() || content_type == "text/plain" || content_type == "application/octet-stream")
        {
            content_type = "text/html; charset=utf-8";
        }
        return;
    }

    if ((content_type == "text/plain" || content_type == "application/octet-stream") &&
        starts_with_html_document(path))
    {
        content_type = "text/html; charset=utf-8";
        download_name += ".html";
    }
}

string trim_ascii_copy(const string &value)
{
    size_t start = 0;
    while (start < value.size() && isspace(static_cast<unsigned char>(value[start])))
    {
        ++start;
    }
    size_t end = value.size();
    while (end > start && isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }
    return value.substr(start, end - start);
}

bool parse_non_negative_long(const string &value, long &number)
{
    const string trimmed = trim_ascii_copy(value);
    if (trimmed.empty())
    {
        return false;
    }

    char *endptr = nullptr;
    number = strtol(trimmed.c_str(), &endptr, 10);
    return endptr != trimmed.c_str() && endptr != nullptr && *endptr == '\0' && number >= 0;
}

bool is_valid_share_token(const string &token)
{
    if (token.empty() || token.size() > 64)
    {
        return false;
    }
    for (size_t i = 0; i < token.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(token[i]);
        if (!isalnum(ch) && ch != '-' && ch != '_')
        {
            return false;
        }
    }
    return true;
}

bool parse_share_path(const char *path, bool download, string &token)
{
    const string suffix = path == nullptr ? string() : string(path);
    const string download_suffix = "/download";
    if (download)
    {
        if (suffix.size() <= download_suffix.size() ||
            suffix.compare(suffix.size() - download_suffix.size(), download_suffix.size(), download_suffix) != 0)
        {
            return false;
        }
        token = suffix.substr(0, suffix.size() - download_suffix.size());
    }
    else
    {
        if (suffix.find('/') != string::npos)
        {
            return false;
        }
        token = suffix;
    }
    return is_valid_share_token(token);
}

}

bool HttpConnection::begin_streamed_body_capture()
{
    if (m_stream_body_file != nullptr)
    {
        return true;
    }

    const string storage_root = infra_storage::storage_root(doc_root);
    const string temp_root = infra_storage::temp_root(doc_root);
    if (!infra_storage::ensure_directory(storage_root) ||
        !infra_storage::ensure_directory(temp_root))
    {
        return false;
    }

    const string token = make_session_token("multipart-body");
    if (token.empty())
    {
        return false;
    }

    m_stream_body_tmp_path = temp_root + "/body-" + token + ".tmp";
    m_stream_body_file = fopen(m_stream_body_tmp_path.c_str(), "wb");
    if (m_stream_body_file == nullptr)
    {
        m_stream_body_tmp_path.clear();
        return false;
    }

    m_stream_body_bytes_received = 0;
    m_request_body.clear();
    return true;
}

bool HttpConnection::append_streamed_body_chunk(const char *data, size_t len)
{
    if (m_stream_body_file == nullptr)
    {
        return false;
    }

    const long body_limit = m_chunked
                                ? static_cast<long>(m_upload_max_bytes + m_upload_request_overhead_bytes)
                                : m_content_length;
    if (m_stream_body_bytes_received + static_cast<long>(len) > body_limit)
    {
        m_body_parse_error_status = m_chunked ? 413 : 400;
        m_body_parse_error_title = m_chunked ? "Payload Too Large" : "Bad Request";
        m_body_parse_error_message = m_chunked
                                         ? string("upload exceeds limit of ") + to_string(m_upload_max_bytes) + " bytes"
                                         : "request body overflow";
        return false;
    }

    if (len == 0)
    {
        return true;
    }

    const size_t written = fwrite(data, 1, len, m_stream_body_file);
    if (written != len)
    {
        m_body_parse_error_status = 500;
        m_body_parse_error_title = "Internal Error";
        m_body_parse_error_message = "failed to spool multipart body";
        return false;
    }

    m_stream_body_bytes_received += static_cast<long>(len);
    return true;
}

void HttpConnection::reset_streamed_body_buffer()
{
    reset_ring_buffer(m_read_ring, m_read_ring_buf.data(), (int)m_read_ring_buf.size());
    m_read_idx = 0;
    m_checked_idx = 0;
    m_body_start_idx = 0;
    m_start_line = 0;
    m_check_state = CHECK_STATE_CONTENT;
    if (!m_read_buf.empty())
    {
        m_read_buf[0] = '\0';
    }
}

bool HttpConnection::parse_multipart_form_data_from_file()
{
    if (m_stream_body_file != nullptr)
    {
        fclose(m_stream_body_file);
        m_stream_body_file = nullptr;
    }

    http_multipart::ParseResult result;
    http_multipart::ParseError error;
    if (!http_multipart::parse_spooled_multipart(m_stream_body_tmp_path, m_content_type, doc_root,
                                                 m_upload_max_bytes, result, error))
    {
        m_body_parse_error_status = error.status;
        m_body_parse_error_title = error.title;
        m_body_parse_error_message = error.message;
        return false;
    }

    m_form_data.insert(result.fields.begin(), result.fields.end());
    m_upload_tmp_path = result.file.temp_path;
    m_upload_tmp_filename = result.file.filename;
    m_upload_tmp_content_type = result.file.content_type;
    m_upload_tmp_sha256 = result.file.sha256;
    m_upload_tmp_size = result.file.size;

    infra_storage::remove_file(m_stream_body_tmp_path);
    m_stream_body_tmp_path.clear();
    return true;
}

bool HttpConnection::parse_managed_upload_payload(service_files::UploadPayload &payload, int &status, const char *&title, string &message)
{
    payload.filename = http_file_helpers::sanitize_filename(request_value("filename", "name"));
    payload.content_type = request_value("content_type", "file_content_type");
    const string folder_value = request_value("folder_id");
    if (!folder_value.empty())
    {
        char *endptr = nullptr;
        const long folder_id = strtol(folder_value.c_str(), &endptr, 10);
        if (endptr == folder_value.c_str() || (endptr != nullptr && *endptr != '\0') || folder_id < 0)
        {
            status = 400;
            title = "Bad Request";
            message = "invalid folder_id";
            return false;
        }
        payload.folder_id = folder_id;
    }

    if (!m_upload_tmp_path.empty())
    {
        if (payload.filename.empty())
        {
            payload.filename = http_file_helpers::sanitize_filename(m_upload_tmp_filename);
        }
        if (payload.filename.empty())
        {
            payload.filename = "file.bin";
        }
        if (payload.content_type.empty())
        {
            payload.content_type = m_upload_tmp_content_type.empty() ? "application/octet-stream" : m_upload_tmp_content_type;
        }

        payload.temp_path = m_upload_tmp_path;
        payload.sha256 = m_upload_tmp_sha256;
        payload.size = m_upload_tmp_size;
        payload.use_temp_file = true;
        return true;
    }

    if (!m_legacy_compat_enabled)
    {
        status = 400;
        title = "Bad Request";
        message = "multipart/form-data file upload required";
        return false;
    }

    payload.content = request_value("content", "file");
    if (payload.content.empty())
    {
        const string content_base64 = request_value("content_base64", "file_base64");
        if (!content_base64.empty() && !decode_base64(content_base64, payload.content))
        {
            status = 400;
            title = "Bad Request";
            message = "invalid base64 content";
            return false;
        }
    }

    if (payload.content.empty())
    {
        status = 400;
        title = "Bad Request";
        message = "content is empty";
        return false;
    }

    if (payload.filename.empty())
    {
        payload.filename = "file.txt";
    }
    if (payload.content_type.empty())
    {
        payload.content_type = "text/plain";
    }
    if (payload.content.size() > m_upload_max_bytes)
    {
        status = 413;
        title = "Payload Too Large";
        message = string("upload exceeds limit of ") + to_string(m_upload_max_bytes) + " bytes";
        return false;
    }

    payload.size = payload.content.size();
    return true;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_upload_preflight()
{
    HTTP_CODE auth_code = require_user_session("file upload requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    string size_value = request_value("size", "file_size");
    if (size_value.empty())
    {
        size_value = request_value("bytes");
    }
    if (size_value.empty())
    {
        size_value = query_value("size");
    }

    long requested_bytes = 0;
    if (!parse_non_negative_long(size_value, requested_bytes))
    {
        return respond_json_error(400, "Bad Request", "invalid file size");
    }

    const string folder_value = request_value("folder_id").empty() ? query_value("folder_id") : request_value("folder_id");
    if (!folder_value.empty())
    {
        long folder_id = 0;
        if (!parse_non_negative_long(folder_value, folder_id))
        {
            return respond_json_error(400, "Bad Request", "invalid folder_id");
        }

        bool exists = false;
        if (!repo_mysql::folder_exists(mysql, m_current_user, folder_id, exists))
        {
            return INTERNAL_ERROR;
        }
        if (!exists)
        {
            return respond_json_error(404, "Not Found", "folder not found");
        }
    }

    service_files::UploadQuota quota;
    quota.max_single_file_bytes = static_cast<long>(m_upload_max_bytes);
    quota.max_total_bytes = static_cast<long>(m_user_storage_quota_bytes);

    service_files::UploadQuotaStatus quota_status;
    service_files::ServiceError error;
    if (!service_files::inspect_upload_quota(mysql, m_current_user, requested_bytes, quota, quota_status, error))
    {
        if (!error.message.empty())
        {
            return respond_json_error(error.status, error.title, error.message);
        }
        return INTERNAL_ERROR;
    }

    set_memory_response(200, "OK", service_files::build_upload_preflight_json(quota_status), "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_upload()
{
    HTTP_CODE auth_code = require_user_session("file upload requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    service_files::UploadPayload payload;
    int error_status = 200;
    const char *error_title = "OK";
    string error_message;
    if (!parse_managed_upload_payload(payload, error_status, error_title, error_message))
    {
        return respond_json_error(error_status, error_title, error_message);
    }

    const bool is_public = service_files::parse_public_flag(request_value("is_public"));
    const string stored_name_prefix = make_session_token(payload.filename);
    if (stored_name_prefix.empty())
    {
        return INTERNAL_ERROR;
    }

    service_files::UploadResult result;
    service_files::ServiceError error;
    service_files::UploadQuota quota;
    quota.max_single_file_bytes = static_cast<long>(m_upload_max_bytes);
    quota.max_total_bytes = static_cast<long>(m_user_storage_quota_bytes);
    if (!service_files::create_uploaded_file(mysql, doc_root, m_current_user, stored_name_prefix,
                                             payload, is_public, quota, result, &error))
    {
        if (!error.message.empty())
        {
            return respond_json_error(error.status, error.title, error.message);
        }
        return INTERNAL_ERROR;
    }
    if (payload.use_temp_file && payload.temp_path == m_upload_tmp_path)
    {
        m_upload_tmp_path.clear();
    }

    write_operation_log(m_current_user, "upload", "file", result.file_id, result.filename);
    set_memory_response(200, "OK", service_files::build_upload_success_json(result), "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_drive_item_list()
{
    HTTP_CODE auth_code = require_user_session("drive list requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    const long folder_id = query_long_value("folder_id", 0, 0, 2147483647L);
    string body;
    service_files::ServiceError error;
    if (!service_files::list_drive_items(mysql, m_current_user, folder_id, body, error))
    {
        if (!error.message.empty())
        {
            return respond_json_error(error.status, error.title, error.message);
        }
        return INTERNAL_ERROR;
    }

    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_drive_folder_create()
{
    HTTP_CODE auth_code = require_user_session("folder create requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    const string parent_value = request_value("parent_id", "folder_id");
    long parent_id = 0;
    if (!parent_value.empty())
    {
        char *endptr = nullptr;
        parent_id = strtol(parent_value.c_str(), &endptr, 10);
        if (endptr == parent_value.c_str() || (endptr != nullptr && *endptr != '\0') || parent_id < 0)
        {
            return respond_json_error(400, "Bad Request", "invalid parent_id");
        }
    }

    repo_mysql::FolderListItem folder;
    service_files::ServiceError error;
    if (!service_files::create_folder(mysql, m_current_user, parent_id, request_value("name"), folder, error))
    {
        if (!error.message.empty())
        {
            return respond_json_error(error.status, error.title, error.message);
        }
        return INTERNAL_ERROR;
    }

    write_operation_log(m_current_user, "create_folder", "folder", folder.id, folder.name);
    set_memory_response(200, "OK", service_files::build_folder_created_json(folder), "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_drive_folder_delete(const char *path)
{
    HTTP_CODE auth_code = require_user_session("folder delete requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long folder_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return BAD_REQUEST;
    }

    repo_mysql::FolderListItem folder;
    service_files::ServiceError error;
    if (!service_files::delete_empty_folder(mysql, m_current_user, folder_id, folder, error))
    {
        if (!error.message.empty())
        {
            return respond_json_error(error.status, error.title, error.message);
        }
        return INTERNAL_ERROR;
    }

    write_operation_log(m_current_user, "delete_folder", "folder", folder_id, folder.name);
    set_memory_response(200, "OK", "{\"code\":0,\"message\":\"folder deleted\"}", "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_drive_file_upload()
{
    return handle_file_upload();
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_list()
{
    HTTP_CODE auth_code = require_user_session("file list requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    const long limit = query_long_value("limit", kDefaultListLimit, 1, kMaxListLimit);
    const long cursor = query_long_value("cursor", 0, 0, 2147483647L);
    const bool include_deleted = query_truthy_value("include_deleted") || query_truthy_value("trash");

    string body;
    if (!service_files::list_private_files(mysql, m_current_user, include_deleted, cursor, limit, body))
    {
        return INTERNAL_ERROR;
    }

    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_public_file_list()
{
    const long limit = query_long_value("limit", kDefaultListLimit, 1, kMaxListLimit);
    const long cursor = query_long_value("cursor", 0, 0, 2147483647L);

    string body;
    if (!service_files::list_public_files(mysql, cursor, limit, body))
    {
        return INTERNAL_ERROR;
    }

    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_public_file_detail(const char *path)
{
    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    long actual_size = 0;
    service_files::ServiceError error;
    if (!service_files::load_public_file_detail(mysql, doc_root, file_id, record, actual_size, error))
    {
        return respond_json_error(error.status, error.title, error.message);
    }

    set_memory_response(200, "OK", service_files::build_public_file_detail_json(record, actual_size),
                        "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_download(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file download requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/download") != 0)
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::load_owned_file_record(mysql, m_current_user, file_id, record, error))
    {
        return respond_json_error(error.status, error.title, error.message);
    }

    if (!open_managed_file(infra_storage::file_path(doc_root, record.stored_name),
                           record.content_type, record.original_name))
    {
        return respond_json_error(404, "Not Found", "file content not found");
    }

    write_operation_log(m_current_user, "download", "file", file_id, record.original_name);
    return FILE_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_public_file_download(const char *path)
{
    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/download") != 0)
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::load_public_file_record(mysql, file_id, record, error))
    {
        return respond_json_error(error.status, error.title, error.message);
    }

    if (!open_managed_file(infra_storage::file_path(doc_root, record.stored_name),
                           record.content_type, record.original_name))
    {
        return respond_json_error(404, "Not Found", "file content not found");
    }

    write_operation_log(m_current_user.empty() ? "guest" : m_current_user,
                        "public_download", "file", file_id, record.original_name);
    return FILE_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_share_create(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file share requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/share") != 0)
    {
        return BAD_REQUEST;
    }

    service_files::ShareOptions options;
    options.access_code = request_value("access_code", "code");

    const string expires_value = request_value("expires_in_seconds", "expires_in");
    if (!expires_value.empty() && !parse_non_negative_long(expires_value, options.expires_in_seconds))
    {
        return respond_json_error(400, "Bad Request", "invalid expires_in_seconds");
    }
    const string max_downloads_value = request_value("max_downloads", "download_limit");
    if (!max_downloads_value.empty() && !parse_non_negative_long(max_downloads_value, options.max_downloads))
    {
        return respond_json_error(400, "Bad Request", "invalid max_downloads");
    }

    service_files::ShareResult share;
    service_files::ServiceError error;
    if (!service_files::create_share_link(mysql, m_current_user, file_id, options, share, error))
    {
        if (error.message.empty())
        {
            return INTERNAL_ERROR;
        }
        return respond_json_error(error.status, error.title, error.message);
    }

    write_operation_log(m_current_user, "create_share", "file", file_id, share.filename);
    set_memory_response(200, "OK", service_files::build_share_json(share), "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_share_detail(const char *path)
{
    string token;
    if (!parse_share_path(path, false, token))
    {
        return BAD_REQUEST;
    }

    string access_code = request_value("access_code", "code");
    if (access_code.empty())
    {
        access_code = query_value("access_code");
    }
    if (access_code.empty())
    {
        access_code = query_value("code");
    }

    service_files::ShareResult share;
    service_files::ServiceError error;
    if (!service_files::load_share_detail(mysql, token, access_code, share, error))
    {
        return respond_json_error(error.status, error.title, error.message);
    }

    set_memory_response(200, "OK", service_files::build_share_json(share), "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_share_download(const char *path)
{
    string token;
    if (!parse_share_path(path, true, token))
    {
        return BAD_REQUEST;
    }

    string access_code = request_value("access_code", "code");
    if (access_code.empty())
    {
        access_code = query_value("access_code");
    }
    if (access_code.empty())
    {
        access_code = query_value("code");
    }

    ManagedFileRecord record;
    service_files::ShareResult share;
    service_files::ServiceError error;
    if (!service_files::load_share_download(mysql, token, access_code, record, share, error))
    {
        return respond_json_error(error.status, error.title, error.message);
    }

    if (!open_managed_file(infra_storage::file_path(doc_root, record.stored_name),
                           record.content_type, record.original_name))
    {
        return respond_json_error(404, "Not Found", "file content not found");
    }

    write_operation_log("guest", "share_download", "file", record.file_id, record.original_name);
    return FILE_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_delete(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file delete requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::soft_delete_file(mysql, m_current_user, file_id, record, error))
    {
        if (error.message.empty())
        {
            return INTERNAL_ERROR;
        }
        return respond_json_error(error.status, error.title, error.message);
    }

    write_operation_log(m_current_user, "delete", "file", file_id, record.original_name);
    set_memory_response(200, "OK",
                        "{\"code\":0,\"message\":\"file moved to recycle bin\"}",
                        "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_visibility_update(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file visibility update requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/visibility") != 0)
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    const bool is_public = service_files::parse_public_flag(request_value("is_public"));
    if (!service_files::update_file_visibility(mysql, m_current_user, file_id, is_public, record, error))
    {
        if (error.message.empty())
        {
            return INTERNAL_ERROR;
        }
        return respond_json_error(error.status, error.title, error.message);
    }

    write_operation_log(m_current_user, is_public ? "publish" : "unpublish", "file", file_id, record.original_name);
    set_memory_response(200, "OK",
                        is_public ? "{\"code\":0,\"message\":\"file is now public\"}"
                                  : "{\"code\":0,\"message\":\"file is now private\"}",
                        "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_restore(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file restore requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/restore") != 0)
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    string restored_name;
    if (!service_files::restore_file(mysql, doc_root, m_current_user, file_id, record, restored_name, error))
    {
        if (error.message.empty())
        {
            return INTERNAL_ERROR;
        }
        return respond_json_error(error.status, error.title, error.message);
    }

    write_operation_log(m_current_user, "restore", "file", file_id, restored_name);
    set_memory_response(200, "OK", service_files::build_restore_success_json(file_id, restored_name),
                        "application/json");
    return MEMORY_REQUEST;
}

bool HttpConnection::open_managed_file(const string &path, const string &content_type, const string &download_name)
{
    strncpy(m_real_file, path.c_str(), sizeof(m_real_file) - 1);
    m_real_file[sizeof(m_real_file) - 1] = '\0';

    if (stat(m_real_file, &m_file_stat) < 0 || S_ISDIR(m_file_stat.st_mode))
    {
        return false;
    }

    m_filefd = open(m_real_file, O_RDONLY);
    if (m_filefd < 0)
    {
        return false;
    }

    string resolved_content_type = content_type.empty() ? "application/octet-stream" : content_type;
    string resolved_download_name = download_name.empty() ? "download.txt" : download_name;
    normalize_download_metadata(path, resolved_content_type, resolved_download_name);

    strncpy(m_response_content_type, resolved_content_type.c_str(), sizeof(m_response_content_type) - 1);
    string safe_name = http_file_helpers::sanitize_download_filename(resolved_download_name);
    string encoded_name = http_file_helpers::encode_download_filename(resolved_download_name);
    m_extra_headers = string("Content-Disposition: attachment; filename=\"") + safe_name +
                      "\"; filename*=UTF-8''" + encoded_name + "\r\n";
    return true;
}
