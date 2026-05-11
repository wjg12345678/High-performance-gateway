#include "mysql_utils.h"

#include <vector>

namespace repo_mysql
{
std::string escape(MYSQL *mysql, const std::string &value)
{
    if (mysql == nullptr || value.empty())
    {
        return value;
    }

    std::string escaped;
    escaped.resize(value.size() * 2 + 1);
    const unsigned long length = mysql_real_escape_string(mysql,
                                                          &escaped[0],
                                                          value.c_str(),
                                                          static_cast<unsigned long>(value.size()));
    escaped.resize(length);
    return escaped;
}

std::string join_numeric_ids(const std::vector<long> &ids)
{
    std::string joined;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (i > 0)
        {
            joined += ",";
        }
        joined += std::to_string(ids[i]);
    }
    return joined;
}

std::string user_id_subquery(MYSQL *mysql, const std::string &username)
{
    return "(SELECT id FROM users WHERE username='" + escape(mysql, username) +
           "' AND disabled_at IS NULL)";
}

std::string nullable_id_value(long id)
{
    return id > 0 ? std::to_string(id) : "NULL";
}

std::string nullable_id_condition(const std::string &column, long id)
{
    return id > 0 ? column + "=" + std::to_string(id) : column + " IS NULL";
}

bool begin_transaction(MYSQL *mysql)
{
    return mysql != nullptr && mysql_query(mysql, "START TRANSACTION") == 0;
}

bool commit_transaction(MYSQL *mysql)
{
    return mysql != nullptr && mysql_query(mysql, "COMMIT") == 0;
}

bool rollback_transaction(MYSQL *mysql)
{
    return mysql != nullptr && mysql_query(mysql, "ROLLBACK") == 0;
}
}
