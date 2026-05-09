#include "file_controller.h"

#include "../files/file_helpers.h"
#include "../../infra/storage/storage.h"
#include "../../repo/mysql/file_repository.h"
#include "../../service/files/file_service.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace std;

namespace
{
const int kDefaultListLimit = 20;
const int kMaxListLimit = 100;

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

namespace http_controllers
{
HttpConnection::HTTP_CODE FileController::upload_preflight(HttpConnection &conn)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("file upload requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    string size_value = conn.request_value("size", "file_size");
    if (size_value.empty())
    {
        size_value = conn.request_value("bytes");
    }
    if (size_value.empty())
    {
        size_value = conn.query_value("size");
    }

    long requested_bytes = 0;
    if (!parse_non_negative_long(size_value, requested_bytes))
    {
        return conn.respond_json_error(400, "Bad Request", "invalid file size");
    }

    const string request_folder = conn.request_value("folder_id");
    const string folder_value = request_folder.empty() ? conn.query_value("folder_id") : request_folder;
    if (!folder_value.empty())
    {
        long folder_id = 0;
        if (!parse_non_negative_long(folder_value, folder_id))
        {
            return conn.respond_json_error(400, "Bad Request", "invalid folder_id");
        }

        bool exists = false;
        if (!repo_mysql::folder_exists(conn.mysql, conn.m_current_user, folder_id, exists))
        {
            return HttpConnection::INTERNAL_ERROR;
        }
        if (!exists)
        {
            return conn.respond_json_error(404, "Not Found", "folder not found");
        }
    }

    service_files::UploadQuota quota;
    quota.max_single_file_bytes = static_cast<long>(conn.m_upload_max_bytes);
    quota.max_total_bytes = static_cast<long>(conn.m_user_storage_quota_bytes);

    service_files::UploadQuotaStatus quota_status;
    service_files::ServiceError error;
    if (!service_files::inspect_upload_quota(conn.mysql, conn.m_current_user, requested_bytes, quota, quota_status, error))
    {
        if (!error.message.empty())
        {
            return conn.respond_json_error(error.status, error.title, error.message);
        }
        return HttpConnection::INTERNAL_ERROR;
    }

    conn.set_memory_response(200, "OK", service_files::build_upload_preflight_json(quota_status), "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::upload(HttpConnection &conn)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("file upload requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    service_files::UploadPayload payload;
    int error_status = 200;
    const char *error_title = "OK";
    string error_message;
    if (!conn.parse_managed_upload_payload(payload, error_status, error_title, error_message))
    {
        return conn.respond_json_error(error_status, error_title, error_message);
    }

    const bool is_public = service_files::parse_public_flag(conn.request_value("is_public"));
    const string stored_name_prefix = conn.make_session_token(payload.filename);
    if (stored_name_prefix.empty())
    {
        return HttpConnection::INTERNAL_ERROR;
    }

    service_files::UploadResult result;
    service_files::ServiceError error;
    service_files::UploadQuota quota;
    quota.max_single_file_bytes = static_cast<long>(conn.m_upload_max_bytes);
    quota.max_total_bytes = static_cast<long>(conn.m_user_storage_quota_bytes);
    if (!service_files::create_uploaded_file(conn.mysql, conn.doc_root, conn.m_current_user, stored_name_prefix,
                                             payload, is_public, quota, result, &error))
    {
        if (!error.message.empty())
        {
            return conn.respond_json_error(error.status, error.title, error.message);
        }
        return HttpConnection::INTERNAL_ERROR;
    }
    if (payload.use_temp_file && payload.temp_path == conn.m_upload_tmp_path)
    {
        conn.m_upload_tmp_path.clear();
    }

    conn.write_operation_log(conn.m_current_user, "upload", "file", result.file_id, result.filename);
    conn.set_memory_response(200, "OK", service_files::build_upload_success_json(result), "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::drive_item_list(HttpConnection &conn)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("drive list requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    const long folder_id = conn.query_long_value("folder_id", 0, 0, 2147483647L);
    string body;
    service_files::ServiceError error;
    if (!service_files::list_drive_items(conn.mysql, conn.m_current_user, folder_id, body, error))
    {
        if (!error.message.empty())
        {
            return conn.respond_json_error(error.status, error.title, error.message);
        }
        return HttpConnection::INTERNAL_ERROR;
    }

    conn.set_memory_response(200, "OK", body, "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::drive_folder_create(HttpConnection &conn)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("folder create requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    const string parent_value = conn.request_value("parent_id", "folder_id");
    long parent_id = 0;
    if (!parent_value.empty() && !parse_non_negative_long(parent_value, parent_id))
    {
        return conn.respond_json_error(400, "Bad Request", "invalid parent_id");
    }

    repo_mysql::FolderListItem folder;
    service_files::ServiceError error;
    if (!service_files::create_folder(conn.mysql, conn.m_current_user, parent_id, conn.request_value("name"), folder, error))
    {
        if (!error.message.empty())
        {
            return conn.respond_json_error(error.status, error.title, error.message);
        }
        return HttpConnection::INTERNAL_ERROR;
    }

    conn.write_operation_log(conn.m_current_user, "create_folder", "folder", folder.id, folder.name);
    conn.set_memory_response(200, "OK", service_files::build_folder_created_json(folder), "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::drive_folder_delete(HttpConnection &conn, const char *path)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("folder delete requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long folder_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return HttpConnection::BAD_REQUEST;
    }

    repo_mysql::FolderListItem folder;
    service_files::ServiceError error;
    if (!service_files::delete_empty_folder(conn.mysql, conn.m_current_user, folder_id, folder, error))
    {
        if (!error.message.empty())
        {
            return conn.respond_json_error(error.status, error.title, error.message);
        }
        return HttpConnection::INTERNAL_ERROR;
    }

    conn.write_operation_log(conn.m_current_user, "delete_folder", "folder", folder_id, folder.name);
    conn.set_memory_response(200, "OK", "{\"code\":0,\"message\":\"folder deleted\"}", "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::drive_file_upload(HttpConnection &conn)
{
    return upload(conn);
}

HttpConnection::HTTP_CODE FileController::private_file_list(HttpConnection &conn)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("file list requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    const long limit = conn.query_long_value("limit", kDefaultListLimit, 1, kMaxListLimit);
    const long cursor = conn.query_long_value("cursor", 0, 0, 2147483647L);
    const bool include_deleted = conn.query_truthy_value("include_deleted") || conn.query_truthy_value("trash");

    string body;
    if (!service_files::list_private_files(conn.mysql, conn.m_current_user, include_deleted, cursor, limit, body))
    {
        return HttpConnection::INTERNAL_ERROR;
    }

    conn.set_memory_response(200, "OK", body, "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::public_file_list(HttpConnection &conn)
{
    const long limit = conn.query_long_value("limit", kDefaultListLimit, 1, kMaxListLimit);
    const long cursor = conn.query_long_value("cursor", 0, 0, 2147483647L);

    string body;
    if (!service_files::list_public_files(conn.mysql, cursor, limit, body))
    {
        return HttpConnection::INTERNAL_ERROR;
    }

    conn.set_memory_response(200, "OK", body, "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::public_file_detail(HttpConnection &conn, const char *path)
{
    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return HttpConnection::BAD_REQUEST;
    }

    ManagedFileRecord record;
    long actual_size = 0;
    service_files::ServiceError error;
    if (!service_files::load_public_file_detail(conn.mysql, conn.doc_root, file_id, record, actual_size, error))
    {
        return conn.respond_json_error(error.status, error.title, error.message);
    }

    conn.set_memory_response(200, "OK", service_files::build_public_file_detail_json(record, actual_size),
                             "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::private_file_download(HttpConnection &conn, const char *path)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("file download requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/download") != 0)
    {
        return HttpConnection::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::load_owned_file_record(conn.mysql, conn.m_current_user, file_id, record, error))
    {
        return conn.respond_json_error(error.status, error.title, error.message);
    }

    if (!conn.open_managed_file(infra_storage::file_path(conn.doc_root, record.stored_name),
                                record.content_type, record.original_name))
    {
        return conn.respond_json_error(404, "Not Found", "file content not found");
    }

    conn.write_operation_log(conn.m_current_user, "download", "file", file_id, record.original_name);
    return HttpConnection::FILE_REQUEST;
}

HttpConnection::HTTP_CODE FileController::public_file_download(HttpConnection &conn, const char *path)
{
    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/download") != 0)
    {
        return HttpConnection::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::load_public_file_record(conn.mysql, file_id, record, error))
    {
        return conn.respond_json_error(error.status, error.title, error.message);
    }

    if (!conn.open_managed_file(infra_storage::file_path(conn.doc_root, record.stored_name),
                                record.content_type, record.original_name))
    {
        return conn.respond_json_error(404, "Not Found", "file content not found");
    }

    conn.write_operation_log(conn.m_current_user.empty() ? "guest" : conn.m_current_user,
                             "public_download", "file", file_id, record.original_name);
    return HttpConnection::FILE_REQUEST;
}

HttpConnection::HTTP_CODE FileController::share_create(HttpConnection &conn, const char *path)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("file share requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/share") != 0)
    {
        return HttpConnection::BAD_REQUEST;
    }

    service_files::ShareOptions options;
    options.access_code = conn.request_value("access_code", "code");

    const string expires_value = conn.request_value("expires_in_seconds", "expires_in");
    if (!expires_value.empty() && !parse_non_negative_long(expires_value, options.expires_in_seconds))
    {
        return conn.respond_json_error(400, "Bad Request", "invalid expires_in_seconds");
    }
    const string max_downloads_value = conn.request_value("max_downloads", "download_limit");
    if (!max_downloads_value.empty() && !parse_non_negative_long(max_downloads_value, options.max_downloads))
    {
        return conn.respond_json_error(400, "Bad Request", "invalid max_downloads");
    }

    service_files::ShareResult share;
    service_files::ServiceError error;
    if (!service_files::create_share_link(conn.mysql, conn.m_current_user, file_id, options, share, error))
    {
        if (error.message.empty())
        {
            return HttpConnection::INTERNAL_ERROR;
        }
        return conn.respond_json_error(error.status, error.title, error.message);
    }

    conn.write_operation_log(conn.m_current_user, "create_share", "file", file_id, share.filename);
    conn.set_memory_response(200, "OK", service_files::build_share_json(share), "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::share_detail(HttpConnection &conn, const char *path)
{
    string token;
    if (!parse_share_path(path, false, token))
    {
        return HttpConnection::BAD_REQUEST;
    }

    string access_code = conn.request_value("access_code", "code");
    if (access_code.empty())
    {
        access_code = conn.query_value("access_code");
    }
    if (access_code.empty())
    {
        access_code = conn.query_value("code");
    }

    service_files::ShareResult share;
    service_files::ServiceError error;
    if (!service_files::load_share_detail(conn.mysql, token, access_code, share, error))
    {
        return conn.respond_json_error(error.status, error.title, error.message);
    }

    conn.set_memory_response(200, "OK", service_files::build_share_json(share), "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::share_download(HttpConnection &conn, const char *path)
{
    string token;
    if (!parse_share_path(path, true, token))
    {
        return HttpConnection::BAD_REQUEST;
    }

    string access_code = conn.request_value("access_code", "code");
    if (access_code.empty())
    {
        access_code = conn.query_value("access_code");
    }
    if (access_code.empty())
    {
        access_code = conn.query_value("code");
    }

    ManagedFileRecord record;
    service_files::ShareResult share;
    service_files::ServiceError error;
    if (!service_files::load_share_download(conn.mysql, token, access_code, record, share, error))
    {
        return conn.respond_json_error(error.status, error.title, error.message);
    }

    if (!conn.open_managed_file(infra_storage::file_path(conn.doc_root, record.stored_name),
                                record.content_type, record.original_name))
    {
        return conn.respond_json_error(404, "Not Found", "file content not found");
    }

    conn.write_operation_log("guest", "share_download", "file", record.file_id, record.original_name);
    return HttpConnection::FILE_REQUEST;
}

HttpConnection::HTTP_CODE FileController::remove(HttpConnection &conn, const char *path)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("file delete requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return HttpConnection::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::soft_delete_file(conn.mysql, conn.m_current_user, file_id, record, error))
    {
        if (error.message.empty())
        {
            return HttpConnection::INTERNAL_ERROR;
        }
        return conn.respond_json_error(error.status, error.title, error.message);
    }

    conn.write_operation_log(conn.m_current_user, "delete", "file", file_id, record.original_name);
    conn.set_memory_response(200, "OK", "{\"code\":0,\"message\":\"file moved to recycle bin\"}",
                             "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::update_visibility(HttpConnection &conn, const char *path)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("file visibility update requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/visibility") != 0)
    {
        return HttpConnection::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    const bool is_public = service_files::parse_public_flag(conn.request_value("is_public"));
    if (!service_files::update_file_visibility(conn.mysql, conn.m_current_user, file_id, is_public, record, error))
    {
        if (error.message.empty())
        {
            return HttpConnection::INTERNAL_ERROR;
        }
        return conn.respond_json_error(error.status, error.title, error.message);
    }

    conn.write_operation_log(conn.m_current_user, is_public ? "publish" : "unpublish", "file", file_id, record.original_name);
    conn.set_memory_response(200, "OK",
                             is_public ? "{\"code\":0,\"message\":\"file is now public\"}"
                                       : "{\"code\":0,\"message\":\"file is now private\"}",
                             "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE FileController::restore(HttpConnection &conn, const char *path)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("file restore requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/restore") != 0)
    {
        return HttpConnection::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    string restored_name;
    if (!service_files::restore_file(conn.mysql, conn.doc_root, conn.m_current_user, file_id, record, restored_name, error))
    {
        if (error.message.empty())
        {
            return HttpConnection::INTERNAL_ERROR;
        }
        return conn.respond_json_error(error.status, error.title, error.message);
    }

    conn.write_operation_log(conn.m_current_user, "restore", "file", file_id, restored_name);
    conn.set_memory_response(200, "OK", service_files::build_restore_success_json(file_id, restored_name),
                             "application/json");
    return HttpConnection::MEMORY_REQUEST;
}
}
