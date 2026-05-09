#include "operation_controller.h"

#include "../../repo/mysql/operation_repository.h"

#include <cstdlib>
#include <string>
#include <vector>

using namespace std;

namespace
{
string json_escape(const string &value)
{
    string escaped;
    escaped.reserve(value.size());
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                escaped += " ";
            }
            else
            {
                escaped += ch;
            }
            break;
        }
    }
    return escaped;
}

string build_operation_list_json(const vector<repo_mysql::OperationLogItem> &operations)
{
    string items;
    bool first = true;
    for (const repo_mysql::OperationLogItem &operation : operations)
    {
        if (!first)
        {
            items += ",";
        }
        first = false;
        items += "{\"id\":";
        items += to_string(operation.id);
        items += ",\"action\":\"";
        items += json_escape(operation.action);
        items += "\",\"resource_type\":\"";
        items += json_escape(operation.resource_type);
        items += "\",\"resource_id\":";
        items += to_string(operation.resource_id);
        items += ",\"detail\":\"";
        items += json_escape(operation.detail);
        items += "\",\"created_at\":\"";
        items += json_escape(operation.created_at);
        items += "\"}";
    }

    return string("{\"code\":0,\"operations\":[") + items + "]}";
}
}

namespace http_controllers
{
HttpConnection::HTTP_CODE OperationController::list(HttpConnection &conn)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("operation list requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    vector<repo_mysql::OperationLogItem> operations;
    if (!repo_mysql::fetch_recent_operations(conn.mysql, conn.m_current_user, operations))
    {
        return HttpConnection::INTERNAL_ERROR;
    }

    string body = build_operation_list_json(operations);
    conn.set_memory_response(200, "OK", body, "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE OperationController::remove(HttpConnection &conn, const char *path)
{
    HttpConnection::HTTP_CODE auth_code = conn.require_user_session("operation delete requires user session");
    if (auth_code != HttpConnection::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long operation_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return HttpConnection::BAD_REQUEST;
    }

    bool deleted = false;
    if (!repo_mysql::delete_operation_log(conn.mysql, conn.m_current_user, operation_id, deleted))
    {
        return HttpConnection::INTERNAL_ERROR;
    }

    if (!deleted)
    {
        return conn.respond_json_error(404, "Not Found", "operation log not found");
    }

    conn.set_memory_response(200, "OK", "{\"code\":0,\"message\":\"delete success\"}", "application/json");
    return HttpConnection::MEMORY_REQUEST;
}
}
