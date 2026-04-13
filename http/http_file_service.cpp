#include "http_conn.h"

#include <fstream>

using namespace std;

namespace
{
const size_t kManagedUploadLimitBytes = 64 * 1024;
}

string http_conn::sanitize_filename(const string &value) const
{
    string cleaned;
    cleaned.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        unsigned char ch = (unsigned char)value[i];
        if (isalnum(ch) || ch == '.' || ch == '_' || ch == '-')
        {
            cleaned.push_back((char)ch);
        }
        else if (ch == ' ')
        {
            cleaned.push_back('_');
        }
    }

    if (cleaned.empty() || cleaned == "." || cleaned == "..")
    {
        cleaned = "file.txt";
    }
    if (cleaned.size() > 80)
    {
        cleaned.resize(80);
    }
    return cleaned;
}

string http_conn::file_storage_root() const
{
    return string(doc_root) + "/uploads";
}

string http_conn::build_file_disk_path(const string &stored_name) const
{
    return file_storage_root() + "/" + stored_name;
}

bool http_conn::ensure_directory(const string &path) const
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
    {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

bool http_conn::fetch_file_record(long file_id, ManagedFileRecord &record)
{
    if (mysql == NULL)
    {
        return false;
    }

    char query[512];
    snprintf(query, sizeof(query),
             "SELECT owner_username, stored_name, original_name, content_type, file_size "
             "FROM files WHERE id=%ld LIMIT 1",
             file_id);
    if (mysql_query(mysql, query) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == NULL)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == NULL)
    {
        mysql_free_result(result);
        return false;
    }

    record.file_id = file_id;
    record.owner = row[0] ? row[0] : "";
    record.stored_name = row[1] ? row[1] : "";
    record.original_name = row[2] ? row[2] : "";
    record.content_type = row[3] ? row[3] : "application/octet-stream";
    record.file_size = row[4] ? atol(row[4]) : 0;
    mysql_free_result(result);
    return true;
}

http_conn::HTTP_CODE http_conn::load_owned_file_record(long file_id, ManagedFileRecord &record)
{
    if (!fetch_file_record(file_id, record))
    {
        return respond_json_error(404, "Not Found", "file not found");
    }

    if (record.owner != m_current_user)
    {
        return respond_json_error(403, "Forbidden", "forbidden");
    }

    return NO_REQUEST;
}

bool http_conn::write_operation_log(const string &username, const string &action, const string &resource_type,
                                    long resource_id, const string &detail)
{
    if (mysql == NULL)
    {
        return false;
    }

    char sql[1800];
    snprintf(sql, sizeof(sql),
             "INSERT INTO operation_logs(username, action, resource_type, resource_id, detail) "
             "VALUES('%s', '%s', '%s', %ld, '%s')",
             escape_sql_value(username).c_str(),
             escape_sql_value(action).c_str(),
             escape_sql_value(resource_type).c_str(),
             resource_id,
             escape_sql_value(detail).c_str());
    return mysql_query(mysql, sql) == 0;
}

string http_conn::build_file_list_json(MYSQL_RES *result) const
{
    string items;
    MYSQL_ROW row;
    bool first = true;
    while ((row = mysql_fetch_row(result)) != NULL)
    {
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
        items += ",\"created_at\":\"";
        items += json_escape(row[4] ? row[4] : "");
        items += "\"}";
    }

    return string("{\"code\":0,\"files\":[") + items + "]}";
}

string http_conn::build_operation_list_json(MYSQL_RES *result) const
{
    string items;
    MYSQL_ROW row;
    bool first = true;
    while ((row = mysql_fetch_row(result)) != NULL)
    {
        if (!first)
        {
            items += ",";
        }
        first = false;
        items += "{\"id\":";
        items += row[0] ? row[0] : "0";
        items += ",\"action\":\"";
        items += json_escape(row[1] ? row[1] : "");
        items += "\",\"resource_type\":\"";
        items += json_escape(row[2] ? row[2] : "");
        items += "\",\"resource_id\":";
        items += row[3] ? row[3] : "0";
        items += ",\"detail\":\"";
        items += json_escape(row[4] ? row[4] : "");
        items += "\",\"created_at\":\"";
        items += json_escape(row[5] ? row[5] : "");
        items += "\"}";
    }

    return string("{\"code\":0,\"operations\":[") + items + "]}";
}

bool http_conn::parse_managed_upload_payload(ManagedUploadPayload &payload, int &status, const char *&title, string &message)
{
    payload.filename = sanitize_filename(request_value("filename", "name"));
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

http_conn::HTTP_CODE http_conn::handle_file_upload()
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

    if (!ensure_directory(file_storage_root()))
    {
        return INTERNAL_ERROR;
    }

    string stored_name = make_session_token(m_current_user) + "_" + payload.filename;
    string disk_path = build_file_disk_path(stored_name);

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
             "INSERT INTO files(owner_username, stored_name, original_name, content_type, file_size) "
             "VALUES('%s', '%s', '%s', '%s', %lu)",
             escape_sql_value(m_current_user).c_str(),
             escape_sql_value(stored_name).c_str(),
             escape_sql_value(payload.filename).c_str(),
             escape_sql_value(payload.content_type).c_str(),
             (unsigned long)payload.content.size());
    if (mysql_query(mysql, sql) != 0)
    {
        unlink(disk_path.c_str());
        return INTERNAL_ERROR;
    }

    long file_id = (long)mysql_insert_id(mysql);
    write_operation_log(m_current_user, "upload", "file", file_id, payload.filename);

    char body[512];
    snprintf(body, sizeof(body),
             "{\"code\":0,\"message\":\"upload success\",\"file\":{\"id\":%ld,\"filename\":\"%s\",\"size\":%lu}}",
             file_id, payload.filename.c_str(), (unsigned long)payload.content.size());
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

http_conn::HTTP_CODE http_conn::handle_file_list()
{
    HTTP_CODE auth_code = require_user_session("file list requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT id, original_name, content_type, file_size, DATE_FORMAT(created_at, '%%Y-%%m-%%d %%H:%%i:%%s') "
             "FROM files WHERE owner_username='%s' ORDER BY id DESC LIMIT 50",
             escape_sql_value(m_current_user).c_str());
    if (mysql_query(mysql, sql) != 0)
    {
        return INTERNAL_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == NULL)
    {
        return INTERNAL_ERROR;
    }

    string body = build_file_list_json(result);
    mysql_free_result(result);
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

http_conn::HTTP_CODE http_conn::handle_file_download(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file download requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = NULL;
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

    if (!open_managed_file(build_file_disk_path(record.stored_name), record.content_type, record.original_name))
    {
        return respond_json_error(404, "Not Found", "file content not found");
    }

    write_operation_log(m_current_user, "download", "file", file_id, record.original_name);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::handle_file_delete(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file delete requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = NULL;
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

    unlink(build_file_disk_path(record.stored_name).c_str());
    write_operation_log(m_current_user, "delete", "file", file_id, record.original_name);
    set_memory_response(200, "OK", "{\"code\":0,\"message\":\"delete success\"}", "application/json");
    return MEMORY_REQUEST;
}

http_conn::HTTP_CODE http_conn::handle_operation_list()
{
    HTTP_CODE auth_code = require_user_session("operation list requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT id, action, resource_type, resource_id, detail, "
             "DATE_FORMAT(created_at, '%%Y-%%m-%%d %%H:%%i:%%s') "
             "FROM operation_logs WHERE username='%s' ORDER BY id DESC LIMIT 50",
             escape_sql_value(m_current_user).c_str());
    if (mysql_query(mysql, sql) != 0)
    {
        return INTERNAL_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == NULL)
    {
        return INTERNAL_ERROR;
    }

    string body = build_operation_list_json(result);
    mysql_free_result(result);
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

http_conn::HTTP_CODE http_conn::handle_operation_delete(const char *path)
{
    HTTP_CODE auth_code = require_user_session("operation delete requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = NULL;
    long operation_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return BAD_REQUEST;
    }

    char sql[512];
    snprintf(sql, sizeof(sql),
             "DELETE FROM operation_logs WHERE id=%ld AND username='%s'",
             operation_id,
             escape_sql_value(m_current_user).c_str());
    if (mysql_query(mysql, sql) != 0)
    {
        return INTERNAL_ERROR;
    }

    if (mysql_affected_rows(mysql) == 0)
    {
        return respond_json_error(404, "Not Found", "operation log not found");
    }

    set_memory_response(200, "OK", "{\"code\":0,\"message\":\"delete success\"}", "application/json");
    return MEMORY_REQUEST;
}

bool http_conn::open_managed_file(const string &path, const string &content_type, const string &download_name)
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
    string safe_name = sanitize_filename(download_name);
    m_extra_headers = string("Content-Disposition: attachment; filename=\"") + safe_name + "\"\r\n";
    return true;
}
