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
http_core::HttpCode OperationController::list(const http_core::HttpRequest &request,
                                              http_core::RequestContext &context,
                                              http_core::HttpResponse &response)
{
    (void)request;
    if (context.current_user.empty())
    {
        response.set_json_error(403, "Forbidden", "operation list requires user session");
        return http_core::MEMORY_REQUEST;
    }

    vector<repo_mysql::OperationLogItem> operations;
    if (!repo_mysql::fetch_recent_operations(context.mysql, context.current_user, operations))
    {
        return http_core::INTERNAL_ERROR;
    }

    string body = build_operation_list_json(operations);
    response.set_body(200, "OK", body, "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode OperationController::remove(const http_core::HttpRequest &request,
                                                http_core::RequestContext &context,
                                                http_core::HttpResponse &response,
                                                const char *path)
{
    (void)request;
    if (context.current_user.empty())
    {
        response.set_json_error(403, "Forbidden", "operation delete requires user session");
        return http_core::MEMORY_REQUEST;
    }

    char *endptr = nullptr;
    long operation_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return http_core::BAD_REQUEST;
    }

    bool deleted = false;
    if (!repo_mysql::delete_operation_log(context.mysql, context.current_user, operation_id, deleted))
    {
        return http_core::INTERNAL_ERROR;
    }

    if (!deleted)
    {
        response.set_json_error(404, "Not Found", "operation log not found");
        return http_core::MEMORY_REQUEST;
    }

    response.set_body(200, "OK", "{\"code\":0,\"message\":\"delete success\"}", "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode OperationController::clear(const http_core::HttpRequest &request,
                                               http_core::RequestContext &context,
                                               http_core::HttpResponse &response)
{
    (void)request;
    if (context.current_user.empty())
    {
        response.set_json_error(403, "Forbidden", "operation clear requires user session");
        return http_core::MEMORY_REQUEST;
    }

    long deleted_count = 0;
    if (!repo_mysql::delete_user_operation_logs(context.mysql, context.current_user, deleted_count))
    {
        return http_core::INTERNAL_ERROR;
    }

    response.set_body(200, "OK",
                      "{\"code\":0,\"message\":\"delete success\",\"deleted_count\":" +
                          to_string(deleted_count) + "}",
                      "application/json");
    return http_core::MEMORY_REQUEST;
}
}
