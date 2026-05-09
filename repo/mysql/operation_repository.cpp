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

    char sql[1800];
    snprintf(sql, sizeof(sql),
             "INSERT INTO operation_logs(username, action, resource_type, resource_id, detail) "
             "VALUES('%s', '%s', '%s', %ld, '%s')",
             escape(mysql, username).c_str(),
             escape(mysql, action).c_str(),
             escape(mysql, resource_type).c_str(),
             resource_id,
             escape(mysql, detail).c_str());
    return mysql_query(mysql, sql) == 0;
}

bool fetch_recent_operations(MYSQL *mysql, const std::string &username, std::vector<OperationLogItem> &items)
{
    items.clear();
    if (mysql == nullptr || username.empty())
    {
        return false;
    }

    const std::string sql = "SELECT id, action, resource_type, resource_id, detail, "
                            "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
                            "FROM operation_logs WHERE username='" + escape(mysql, username) +
                            "' ORDER BY id DESC LIMIT 50";
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
                            " AND username='" + escape(mysql, username) + "'";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    deleted = mysql_affected_rows(mysql) > 0;
    return true;
}
}
