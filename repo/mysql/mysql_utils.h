#ifndef REPO_MYSQL_UTILS_H
#define REPO_MYSQL_UTILS_H

#include <mysql/mysql.h>
#include <string>
#include <vector>

namespace repo_mysql
{
std::string escape(MYSQL *mysql, const std::string &value);
std::string join_numeric_ids(const std::vector<long> &ids);
}

#endif
