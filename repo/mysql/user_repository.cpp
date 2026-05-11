#include "user_repository.h"

#include "mysql_utils.h"

namespace repo_mysql
{
bool insert_user(MYSQL *mysql, const std::string &username, const std::string &password_record)
{
    if (mysql == nullptr || username.empty() || password_record.empty())
    {
        return false;
    }

    const std::string sql = "INSERT INTO users(username, password_hash) VALUES('" +
                            escape(mysql, username) + "', '" +
                            escape(mysql, password_record) + "')";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool fetch_user_password(MYSQL *mysql, const std::string &username, UserPasswordRecord &record)
{
    if (mysql == nullptr || username.empty())
    {
        return false;
    }

    const std::string sql = "SELECT password_hash FROM users WHERE username='" +
                            escape(mysql, username) + "' AND disabled_at IS NULL LIMIT 1";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == nullptr)
    {
        mysql_free_result(result);
        return false;
    }

    record.password = row[0] ? row[0] : "";
    mysql_free_result(result);
    return true;
}

bool update_user_password(MYSQL *mysql, const std::string &username, const std::string &password_record)
{
    if (mysql == nullptr || username.empty() || password_record.empty())
    {
        return false;
    }

    const std::string sql = "UPDATE users SET password_hash='" + escape(mysql, password_record) +
                            "' WHERE username='" + escape(mysql, username) + "' AND disabled_at IS NULL";
    return mysql_query(mysql, sql.c_str()) == 0;
}
bool fetch_all_user_passwords(MYSQL *mysql, std::map<std::string, std::string> &users)
{
    if (mysql == nullptr)
    {
        return false;
    }

    if (mysql_query(mysql, "SELECT username,password_hash FROM users WHERE disabled_at IS NULL") != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    users.clear();
    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        const std::string username = row[0] ? row[0] : "";
        const std::string password = row[1] ? row[1] : "";
        if (!username.empty())
        {
            users[username] = password;
        }
    }

    mysql_free_result(result);
    return true;
}

bool mark_user_login(MYSQL *mysql, const std::string &username)
{
    if (mysql == nullptr || username.empty())
    {
        return false;
    }

    const std::string sql = "UPDATE users SET last_login_at=NOW() WHERE username='" +
                            escape(mysql, username) + "' AND disabled_at IS NULL";
    return mysql_query(mysql, sql.c_str()) == 0;
}

}
