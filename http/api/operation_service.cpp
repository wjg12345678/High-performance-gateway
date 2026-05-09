#include "../core/connection.h"
#include "../../repo/mysql/operation_repository.h"

using namespace std;

bool HttpConnection::write_operation_log(const string &username, const string &action, const string &resource_type,
                                         long resource_id, const string &detail)
{
    return repo_mysql::insert_operation_log(mysql, username, action, resource_type, resource_id, detail);
}

string HttpConnection::build_operation_list_json(const vector<repo_mysql::OperationLogItem> &operations) const
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

HttpConnection::HTTP_CODE HttpConnection::handle_operation_list()
{
    HTTP_CODE auth_code = require_user_session("operation list requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    vector<repo_mysql::OperationLogItem> operations;
    if (!repo_mysql::fetch_recent_operations(mysql, m_current_user, operations))
    {
        return INTERNAL_ERROR;
    }

    string body = build_operation_list_json(operations);
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

    bool deleted = false;
    if (!repo_mysql::delete_operation_log(mysql, m_current_user, operation_id, deleted))
    {
        return INTERNAL_ERROR;
    }

    if (!deleted)
    {
        return respond_json_error(404, "Not Found", "operation log not found");
    }

    set_memory_response(200, "OK", "{\"code\":0,\"message\":\"delete success\"}", "application/json");
    return MEMORY_REQUEST;
}
