#include "../core/connection.h"
#include "file_helpers.h"
#include "file_store.h"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

using namespace std;

namespace
{
const size_t kManagedUploadLimitBytes = 64 * 1024;

bool parse_public_flag(const string &value)
{
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on";
}

bool file_exists_at_path(const string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}
}

HttpConnection::HTTP_CODE HttpConnection::load_owned_file_record(long file_id, ManagedFileRecord &record)
{
    if (!http_file_store::fetch_file_record(mysql, file_id, record))
    {
        return respond_json_error(404, "Not Found", "file not found");
    }

    if (record.owner != m_current_user)
    {
        return respond_json_error(403, "Forbidden", "forbidden");
    }

    return NO_REQUEST;
}

string HttpConnection::build_file_list_json(MYSQL_RES *result) const
{
    string items;
    MYSQL_ROW row;
    bool first = true;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        const string stored_name = row[7] ? row[7] : "";
        if (!stored_name.empty() &&
            !file_exists_at_path(http_file_helpers::build_file_disk_path(doc_root, stored_name)))
        {
            continue;
        }

        if (!first)
        {
            items += ",";
        }
        first = false;
        items += "{\"id\":";
        items += row[0] ? row[0] : "0";
        items += ",\"filename\":\"";
        items += json_escape(row[1] ? row[1] : "");
        items += "\",\"content_type\":\"";
        items += json_escape(row[2] ? row[2] : "");
        items += "\",\"size\":";
        items += row[3] ? row[3] : "0";
        items += ",\"is_public\":";
        items += row[5] ? row[5] : "0";
        items += ",\"owner\":\"";
        items += json_escape(row[6] ? row[6] : "");
        items += "\"";
        items += ",\"created_at\":\"";
        items += json_escape(row[4] ? row[4] : "");
        items += "\"}";
    }

    return string("{\"code\":0,\"files\":[") + items + "]}";
}

bool HttpConnection::parse_managed_upload_payload(ManagedUploadPayload &payload, int &status, const char *&title, string &message)
{
    payload.filename = http_file_helpers::sanitize_filename(request_value("filename", "name"));
    payload.content = request_value("content", "file");
    payload.content_type = request_value("content_type", "file_content_type");
    if (payload.content_type.empty())
    {
        payload.content_type = "text/plain";
    }

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

    if (payload.content.size() > kManagedUploadLimitBytes)
    {
        status = 413;
        title = "Payload Too Large";
        message = "current upload limit is 64 KB";
        return false;
    }

    return true;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_upload()
{
    HTTP_CODE auth_code = require_user_session("file upload requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    ManagedUploadPayload payload;
    int error_status = 200;
    const char *error_title = "OK";
    string error_message;
    if (!parse_managed_upload_payload(payload, error_status, error_title, error_message))
    {
        return respond_json_error(error_status, error_title, error_message);
    }

    if (!http_file_helpers::ensure_directory(http_file_helpers::file_storage_root(doc_root)))
    {
        return INTERNAL_ERROR;
    }

    string stored_name = make_session_token(m_current_user) + "_" + payload.filename;
    string disk_path = http_file_helpers::build_file_disk_path(doc_root, stored_name);

    ofstream out(disk_path.c_str(), ios::out | ios::binary | ios::trunc);
    if (!out.is_open())
    {
        return INTERNAL_ERROR;
    }
    out.write(payload.content.data(), payload.content.size());
    out.close();
    if (!out.good())
    {
        unlink(disk_path.c_str());
        return INTERNAL_ERROR;
    }

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO files(owner_username, stored_name, original_name, content_type, file_size, is_public) "
             "VALUES('%s', '%s', '%s', '%s', %lu, %d)",
             escape_sql_value(m_current_user).c_str(),
             escape_sql_value(stored_name).c_str(),
             escape_sql_value(payload.filename).c_str(),
             escape_sql_value(payload.content_type).c_str(),
             (unsigned long)payload.content.size(),
             parse_public_flag(request_value("is_public")) ? 1 : 0);
    if (mysql_query(mysql, sql) != 0)
    {
        unlink(disk_path.c_str());
        return INTERNAL_ERROR;
    }

    long file_id = (long)mysql_insert_id(mysql);
    write_operation_log(m_current_user, "upload", "file", file_id, payload.filename);

    char body[512];
    snprintf(body, sizeof(body),
             "{\"code\":0,\"message\":\"upload success\",\"file\":{\"id\":%ld,\"filename\":\"%s\",\"size\":%lu,\"is_public\":%s}}",
             file_id, payload.filename.c_str(), (unsigned long)payload.content.size(),
             parse_public_flag(request_value("is_public")) ? "true" : "false");
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_list()
{
    HTTP_CODE auth_code = require_user_session("file list requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT id, original_name, content_type, file_size, DATE_FORMAT(created_at, '%%Y-%%m-%%d %%H:%%i:%%s'), is_public, owner_username, stored_name "
             "FROM files WHERE owner_username='%s' ORDER BY id DESC LIMIT 50",
             escape_sql_value(m_current_user).c_str());
    if (mysql_query(mysql, sql) != 0)
    {
        return INTERNAL_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return INTERNAL_ERROR;
    }

    string body = build_file_list_json(result);
    mysql_free_result(result);
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_public_file_list()
{
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT id, original_name, content_type, file_size, DATE_FORMAT(created_at, '%%Y-%%m-%%d %%H:%%i:%%s'), is_public, owner_username, stored_name "
             "FROM files WHERE is_public=1 ORDER BY id DESC LIMIT 100");
    if (mysql_query(mysql, sql) != 0)
    {
        return INTERNAL_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return INTERNAL_ERROR;
    }

    string body = build_file_list_json(result);
    mysql_free_result(result);
    set_memory_response(200, "OK", body, "application/json");
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
    HTTP_CODE record_code = load_owned_file_record(file_id, record);
    if (record_code != NO_REQUEST)
    {
        return record_code;
    }

    if (!open_managed_file(http_file_helpers::build_file_disk_path(doc_root, record.stored_name),
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
    if (!http_file_store::fetch_file_record(mysql, file_id, record))
    {
        return respond_json_error(404, "Not Found", "file not found");
    }
    if (!record.is_public)
    {
        return respond_json_error(403, "Forbidden", "file is private");
    }

    if (!open_managed_file(http_file_helpers::build_file_disk_path(doc_root, record.stored_name),
                           record.content_type, record.original_name))
    {
        return respond_json_error(404, "Not Found", "file content not found");
    }

    write_operation_log(m_current_user.empty() ? "guest" : m_current_user,
                        "public_download", "file", file_id, record.original_name);
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
    HTTP_CODE record_code = load_owned_file_record(file_id, record);
    if (record_code != NO_REQUEST)
    {
        return record_code;
    }

    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM files WHERE id=%ld", file_id);
    if (mysql_query(mysql, sql) != 0)
    {
        return INTERNAL_ERROR;
    }

    unlink(http_file_helpers::build_file_disk_path(doc_root, record.stored_name).c_str());
    write_operation_log(m_current_user, "delete", "file", file_id, record.original_name);
    set_memory_response(200, "OK", "{\"code\":0,\"message\":\"delete success\"}", "application/json");
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
    HTTP_CODE record_code = load_owned_file_record(file_id, record);
    if (record_code != NO_REQUEST)
    {
        return record_code;
    }

    const bool is_public = parse_public_flag(request_value("is_public"));
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE files SET is_public=%d WHERE id=%ld", is_public ? 1 : 0, file_id);
    if (mysql_query(mysql, sql) != 0)
    {
        return INTERNAL_ERROR;
    }

    write_operation_log(m_current_user, is_public ? "publish" : "unpublish", "file", file_id, record.original_name);
    set_memory_response(200, "OK",
                        is_public ? "{\"code\":0,\"message\":\"file is now public\"}"
                                  : "{\"code\":0,\"message\":\"file is now private\"}",
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

    strncpy(m_response_content_type, content_type.c_str(), sizeof(m_response_content_type) - 1);
    string safe_name = http_file_helpers::sanitize_download_filename(download_name);
    string encoded_name = http_file_helpers::encode_download_filename(download_name);
    m_extra_headers = string("Content-Disposition: attachment; filename=\"") + safe_name +
                      "\"; filename*=UTF-8''" + encoded_name + "\r\n";
    return true;
}
