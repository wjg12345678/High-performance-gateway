#ifndef REPO_MYSQL_UTILS_H
#define REPO_MYSQL_UTILS_H

#include <mysql/mysql.h>
#include <string>
#include <vector>

namespace repo_mysql
{
std::string escape(MYSQL *mysql, const std::string &value);
std::string join_numeric_ids(const std::vector<long> &ids);
std::string user_id_subquery(MYSQL *mysql, const std::string &username);
std::string nullable_id_value(long id);
std::string nullable_id_condition(const std::string &column, long id);
bool begin_transaction(MYSQL *mysql);
bool commit_transaction(MYSQL *mysql);
bool rollback_transaction(MYSQL *mysql);
}

#endif
