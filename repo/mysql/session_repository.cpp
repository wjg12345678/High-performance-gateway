#include "session_repository.h"

#include "mysql_utils.h"

#include <cstdlib>

namespace repo_mysql
{
bool find_active_session(MYSQL *mysql, const std::string &token, SessionRecord &record)
{
    if (mysql == nullptr || token.empty())
    {
        return false;
    }

    const std::string sql = "SELECT username, UNIX_TIMESTAMP(expires_at) FROM user_sessions "
                            "WHERE token='" + escape(mysql, token) + "' AND expires_at > NOW() LIMIT 1";
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

    record.username = row[0] ? row[0] : "";
    record.expires_at = row[1] ? static_cast<time_t>(strtoll(row[1], nullptr, 10)) : 0;
    mysql_free_result(result);
    return true;
}

bool cleanup_session_token_or_expired(MYSQL *mysql, const std::string &token)
{
    if (mysql == nullptr)
    {
        return false;
    }

    const std::string sql = "DELETE FROM user_sessions WHERE token='" + escape(mysql, token) +
                            "' OR expires_at <= NOW()";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool upsert_session(MYSQL *mysql, const std::string &token, const std::string &username, int ttl_seconds)
{
    if (mysql == nullptr || token.empty() || username.empty() || ttl_seconds <= 0)
    {
        return false;
    }

    const std::string sql = "INSERT INTO user_sessions(token, username, expires_at) "
                            "VALUES('" + escape(mysql, token) + "', '" + escape(mysql, username) +
                            "', DATE_ADD(NOW(), INTERVAL " + std::to_string(ttl_seconds) + " SECOND)) "
                            "ON DUPLICATE KEY UPDATE username=VALUES(username), expires_at=VALUES(expires_at)";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool refresh_session(MYSQL *mysql, const std::string &token, const std::string &username, int ttl_seconds, bool &updated)
{
    updated = false;
    if (mysql == nullptr || token.empty() || username.empty() || ttl_seconds <= 0)
    {
        return false;
    }

    const std::string sql = "UPDATE user_sessions SET expires_at=DATE_ADD(NOW(), INTERVAL " +
                            std::to_string(ttl_seconds) + " SECOND) WHERE token='" +
                            escape(mysql, token) + "' AND username='" + escape(mysql, username) +
                            "' AND expires_at > NOW()";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    updated = mysql_affected_rows(mysql) > 0;
    return true;
}

bool delete_session(MYSQL *mysql, const std::string &token)
{
    if (mysql == nullptr || token.empty())
    {
        return false;
    }

    const std::string sql = "DELETE FROM user_sessions WHERE token='" + escape(mysql, token) + "'";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool delete_user_sessions(MYSQL *mysql, const std::string &username, const std::string &except_token)
{
    if (mysql == nullptr || username.empty())
    {
        return false;
    }

    std::string sql = "DELETE FROM user_sessions WHERE username='" + escape(mysql, username) + "'";
    if (!except_token.empty())
    {
        sql += " AND token<>'" + escape(mysql, except_token) + "'";
    }
    return mysql_query(mysql, sql.c_str()) == 0;
}
}
