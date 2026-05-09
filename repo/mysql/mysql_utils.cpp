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
}
