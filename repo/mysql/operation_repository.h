#ifndef REPO_MYSQL_OPERATION_REPOSITORY_H
#define REPO_MYSQL_OPERATION_REPOSITORY_H

#include <mysql/mysql.h>
#include <string>
#include <vector>

namespace repo_mysql
{
struct OperationLogItem
{
    long id;
    long resource_id;
    std::string action;
    std::string resource_type;
    std::string detail;
    std::string created_at;

    OperationLogItem() : id(0), resource_id(0) {}
};

bool insert_operation_log(MYSQL *mysql, const std::string &username, const std::string &action,
                          const std::string &resource_type, long resource_id, const std::string &detail);
bool fetch_recent_operations(MYSQL *mysql, const std::string &username, std::vector<OperationLogItem> &items);
bool delete_operation_log(MYSQL *mysql, const std::string &username, long operation_id, bool &deleted);
}

#endif
