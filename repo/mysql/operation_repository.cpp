#include "operation_repository.h"

#include "mysql_utils.h"

#include <cstdlib>
#include <cstdio>

namespace repo_mysql
{
bool insert_operation_log(MYSQL *mysql, const std::string &username, const std::string &action,
                          const std::string &resource_type, long resource_id, const std::string &detail)
{
    if (mysql == nullptr)
    {
        return false;
    }

    const std::string escaped_username = escape(mysql, username);
    const std::string sql =
        "INSERT INTO operation_logs(user_id, username_snapshot, action, resource_type, resource_id, detail) "
        "VALUES((SELECT id FROM users WHERE username='" + escaped_username +
        "' AND disabled_at IS NULL), '" + escaped_username + "', '" +
        escape(mysql, action) + "', '" + escape(mysql, resource_type) + "', " +
        std::to_string(resource_id) + ", JSON_OBJECT('message', '" + escape(mysql, detail) + "'))";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool fetch_recent_operations(MYSQL *mysql, const std::string &username, std::vector<OperationLogItem> &items)
{
    items.clear();
    if (mysql == nullptr || username.empty())
    {
        return false;
    }

    const std::string sql = "SELECT id, action, resource_type, resource_id, "
                            "COALESCE(JSON_UNQUOTE(JSON_EXTRACT(detail, '$.message')), CAST(detail AS CHAR)), "
                            "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
                            "FROM operation_logs WHERE user_id=" + user_id_subquery(mysql, username) +
                            " ORDER BY id DESC LIMIT 50";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        OperationLogItem item;
        item.id = row[0] ? atol(row[0]) : 0;
        item.action = row[1] ? row[1] : "";
        item.resource_type = row[2] ? row[2] : "";
        item.resource_id = row[3] ? atol(row[3]) : 0;
        item.detail = row[4] ? row[4] : "";
        item.created_at = row[5] ? row[5] : "";
        items.push_back(item);
    }

    mysql_free_result(result);
    return true;
}

bool delete_operation_log(MYSQL *mysql, const std::string &username, long operation_id, bool &deleted)
{
    deleted = false;
    if (mysql == nullptr || username.empty())
    {
        return false;
    }

    const std::string sql = "DELETE FROM operation_logs WHERE id=" + std::to_string(operation_id) +
                            " AND user_id=" + user_id_subquery(mysql, username);
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    deleted = mysql_affected_rows(mysql) > 0;
    return true;
}

bool delete_user_operation_logs(MYSQL *mysql, const std::string &username, long &deleted_count)
{
    deleted_count = 0;
    if (mysql == nullptr || username.empty())
    {
        return false;
    }

    const std::string sql = "DELETE FROM operation_logs WHERE user_id=" + user_id_subquery(mysql, username);
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    deleted_count = static_cast<long>(mysql_affected_rows(mysql));
    return true;
}
}
