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

    const std::string sql = "SELECT u.username, UNIX_TIMESTAMP(s.expires_at) "
                            "FROM user_sessions s JOIN users u ON u.id=s.user_id "
                            "WHERE s.token='" + escape(mysql, token) +
                            "' AND s.expires_at > NOW() AND u.disabled_at IS NULL LIMIT 1";
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

    const std::string sql = "INSERT INTO user_sessions(token, user_id, expires_at) "
                            "SELECT '" + escape(mysql, token) + "', id, DATE_ADD(NOW(), INTERVAL " +
                            std::to_string(ttl_seconds) + " SECOND) FROM users WHERE username='" +
                            escape(mysql, username) + "' AND disabled_at IS NULL "
                            "ON DUPLICATE KEY UPDATE user_id=VALUES(user_id), expires_at=VALUES(expires_at)";
    return mysql_query(mysql, sql.c_str()) == 0 && mysql_affected_rows(mysql) > 0;
}

bool refresh_session(MYSQL *mysql, const std::string &token, const std::string &username, int ttl_seconds, bool &updated)
{
    updated = false;
    if (mysql == nullptr || token.empty() || username.empty() || ttl_seconds <= 0)
    {
        return false;
    }

    const std::string sql = "UPDATE user_sessions s JOIN users u ON u.id=s.user_id "
                            "SET s.expires_at=DATE_ADD(NOW(), INTERVAL " +
                            std::to_string(ttl_seconds) + " SECOND) WHERE s.token='" +
                            escape(mysql, token) + "' AND u.username='" + escape(mysql, username) +
                            "' AND s.expires_at > NOW() AND u.disabled_at IS NULL";
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

    std::string sql = "DELETE s FROM user_sessions s JOIN users u ON u.id=s.user_id "
                      "WHERE u.username='" + escape(mysql, username) + "'";
    if (!except_token.empty())
    {
        sql += " AND s.token<>'" + escape(mysql, except_token) + "'";
    }
    return mysql_query(mysql, sql.c_str()) == 0;
}
}
