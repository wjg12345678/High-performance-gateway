#include "../core/connection.h"
#include "auth_state.h"

#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <openssl/sha.h>

namespace
{
bool starts_with_ignore_case_local(const char *text, const char *prefix)
{
    return strncasecmp(text, prefix, strlen(prefix)) == 0;
}
}

std::string HttpConnection::make_password_salt() const
{
    char salt[64];
    unsigned int seed = static_cast<unsigned int>(time(nullptr) ^ m_sockfd ^ static_cast<unsigned int>(getpid()));
    snprintf(salt, sizeof(salt), "%08x%08x",
             static_cast<unsigned int>(rand_r(&seed) ^ static_cast<unsigned int>(time(nullptr))),
             static_cast<unsigned int>(rand_r(&seed) ^ static_cast<unsigned int>(getpid())));
    return salt;
}

std::string HttpConnection::hash_password(const string &password, const string &salt) const
{
    string input = salt + password;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest);

    static const char hex_chars[] = "0123456789abcdef";
    string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
    {
        hex.push_back(hex_chars[(digest[i] >> 4) & 0x0F]);
        hex.push_back(hex_chars[digest[i] & 0x0F]);
    }
    return hex;
}

std::string HttpConnection::extract_bearer_token() const
{
    if (!starts_with_ignore_case_local(m_authorization.c_str(), "Bearer "))
    {
        return "";
    }
    return trim_copy(m_authorization.substr(7));
}

bool HttpConnection::lookup_session(const string &token, string &username)
{
    if (mysql == nullptr)
    {
        return false;
    }

    locker &cache_lock = auth_cache_lock();
    map<string, string> &session_cache = auth_session_cache();

    cache_lock.lock();
    map<string, string>::iterator session_it = session_cache.find(token);
    if (session_it != session_cache.end())
    {
        username = session_it->second;
        cache_lock.unlock();
        return true;
    }
    cache_lock.unlock();

    const string escaped_token = escape_sql_value(token);
    const string sql = "SELECT username FROM user_sessions "
                       "WHERE token='" + escaped_token + "' AND expires_at > NOW() LIMIT 1";
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
        const string cleanup = "DELETE FROM user_sessions WHERE token='" + escaped_token +
                               "' OR expires_at <= NOW()";
        mysql_query(mysql, cleanup.c_str());
        return false;
    }

    username = row[0] ? row[0] : "";
    mysql_free_result(result);

    if (!username.empty())
    {
        cache_lock.lock();
        session_cache[token] = username;
        cache_lock.unlock();
        return true;
    }
    return false;
}

bool HttpConnection::persist_session(const string &token, const string &username, int ttl_seconds)
{
    if (mysql == nullptr)
    {
        return false;
    }

    char ttl_buffer[32];
    snprintf(ttl_buffer, sizeof(ttl_buffer), "%d", ttl_seconds);
    const string sql = "INSERT INTO user_sessions(token, username, expires_at) "
                       "VALUES('" + escape_sql_value(token) + "', '" + escape_sql_value(username) +
                       "', DATE_ADD(NOW(), INTERVAL " + string(ttl_buffer) + " SECOND)) "
                       "ON DUPLICATE KEY UPDATE username=VALUES(username), expires_at=VALUES(expires_at)";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    auth_cache_lock().lock();
    auth_session_cache()[token] = username;
    auth_cache_lock().unlock();
    return true;
}

bool HttpConnection::remove_session(const string &token)
{
    if (token.empty())
    {
        return false;
    }

    auth_cache_lock().lock();
    auth_session_cache().erase(token);
    auth_cache_lock().unlock();

    if (mysql == nullptr)
    {
        return true;
    }

    const string sql = "DELETE FROM user_sessions WHERE token='" + escape_sql_value(token) + "'";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool HttpConnection::update_user_password_hash(const string &username, const string &password)
{
    if (mysql == nullptr)
    {
        return false;
    }

    string salt = make_password_salt();
    string password_hash = hash_password(password, salt);
    const string sql = "UPDATE user SET passwd='" + escape_sql_value(password_hash) +
                       "', passwd_salt='" + escape_sql_value(salt) +
                       "' WHERE username='" + escape_sql_value(username) + "'";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    auth_cache_lock().lock();
    auth_user_cache()[username] = password_hash;
    auth_cache_lock().unlock();
    return true;
}

bool HttpConnection::verify_user_password(const string &username, const string &password)
{
    if (mysql == nullptr)
    {
        return false;
    }

    const string sql = "SELECT passwd, COALESCE(passwd_salt, '') FROM user WHERE username='" +
                       escape_sql_value(username) + "' LIMIT 1";
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

    string stored_password = row[0] ? row[0] : "";
    string stored_salt = row[1] ? row[1] : "";
    mysql_free_result(result);

    if (stored_salt.empty())
    {
        if (stored_password == password)
        {
            update_user_password_hash(username, password);
            return true;
        }
        return false;
    }

    return stored_password == hash_password(password, stored_salt);
}

HttpConnection::HTTP_CODE HttpConnection::middleware_auth()
{
    if (!requires_auth())
    {
        return NO_REQUEST;
    }

    string token = extract_bearer_token();
    if (!token.empty())
    {
        if (lookup_session(token, m_current_user))
        {
            return NO_REQUEST;
        }

        if (!m_auth_token.empty() && token == m_auth_token)
        {
            m_current_user = "admin";
            return NO_REQUEST;
        }
    }

    set_memory_response(401, "Unauthorized",
                        "{\"code\":401,\"message\":\"unauthorized\"}",
                        "application/json");
    if (mysql != nullptr)
    {
        write_operation_log("anonymous", "auth_failed", "request", 0, m_url ? m_url : "");
    }
    return MEMORY_REQUEST;
}
