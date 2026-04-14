#include "../core/connection.h"

using namespace std;

bool HttpConnection::write_operation_log(const string &username, const string &action, const string &resource_type,
                                         long resource_id, const string &detail)
{
    if (mysql == nullptr)
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

string HttpConnection::build_operation_list_json(MYSQL_RES *result) const
{
    string items;
    MYSQL_ROW row;
    bool first = true;
    while ((row = mysql_fetch_row(result)) != nullptr)
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

HttpConnection::HTTP_CODE HttpConnection::handle_operation_list()
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
    if (result == nullptr)
    {
        return INTERNAL_ERROR;
    }

    string body = build_operation_list_json(result);
    mysql_free_result(result);
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_operation_delete(const char *path)
{
    HTTP_CODE auth_code = require_user_session("operation delete requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
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
