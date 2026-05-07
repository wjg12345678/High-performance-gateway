#include "../core/connection.h"
#include "auth_state.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace
{
const char *kPasswordScheme = "pbkdf2_sha256";
const size_t kPasswordSaltBytes = 16;
const size_t kPasswordHashBytes = 32;
const int kPasswordIterations = 210000;
const int kSessionTtlSeconds = 7 * 24 * 3600;
const int kSessionRefreshThresholdSeconds = 24 * 3600;

bool starts_with_ignore_case_local(const char *text, const char *prefix)
{
    return strncasecmp(text, prefix, strlen(prefix)) == 0;
}

std::string encode_hex(const unsigned char *data, size_t len)
{
    static const char kHexChars[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        hex.push_back(kHexChars[(data[i] >> 4) & 0x0F]);
        hex.push_back(kHexChars[data[i] & 0x0F]);
    }
    return hex;
}

bool decode_hex_nibble(char ch, unsigned char &value)
{
    if (ch >= '0' && ch <= '9')
    {
        value = static_cast<unsigned char>(ch - '0');
        return true;
    }
    if (ch >= 'a' && ch <= 'f')
    {
        value = static_cast<unsigned char>(ch - 'a' + 10);
        return true;
    }
    if (ch >= 'A' && ch <= 'F')
    {
        value = static_cast<unsigned char>(ch - 'A' + 10);
        return true;
    }
    return false;
}

bool decode_hex(const std::string &hex, std::vector<unsigned char> &output)
{
    if (hex.empty() || hex.size() % 2 != 0)
    {
        return false;
    }

    output.clear();
    output.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        unsigned char high = 0;
        unsigned char low = 0;
        if (!decode_hex_nibble(hex[i], high) || !decode_hex_nibble(hex[i + 1], low))
        {
            output.clear();
            return false;
        }
        output.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return true;
}

bool secure_random_hex(size_t byte_count, std::string &output)
{
    std::vector<unsigned char> bytes(byte_count, 0);
    if (bytes.empty() || RAND_bytes(&bytes[0], static_cast<int>(bytes.size())) != 1)
    {
        output.clear();
        return false;
    }

    output = encode_hex(&bytes[0], bytes.size());
    return true;
}

std::string legacy_sha256_hash(const std::string &password, const std::string &salt)
{
    const std::string input = salt + password;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest);
    return encode_hex(digest, sizeof(digest));
}

bool derive_pbkdf2_sha256(const std::string &password, const std::string &salt_hex,
                          int iterations, std::string &derived_hex)
{
    if (iterations <= 0)
    {
        return false;
    }

    std::vector<unsigned char> salt_bytes;
    if (!decode_hex(salt_hex, salt_bytes) || salt_bytes.empty())
    {
        return false;
    }

    unsigned char derived[kPasswordHashBytes];
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          &salt_bytes[0], static_cast<int>(salt_bytes.size()),
                          iterations, EVP_sha256(), sizeof(derived), derived) != 1)
    {
        return false;
    }

    derived_hex = encode_hex(derived, sizeof(derived));
    return true;
}

std::string build_password_record(const std::string &salt_hex, const std::string &hash_hex)
{
    char iterations[32];
    snprintf(iterations, sizeof(iterations), "%d", kPasswordIterations);
    return std::string(kPasswordScheme) + "$" + iterations + "$" + salt_hex + "$" + hash_hex;
}

bool parse_password_record(const std::string &record, int &iterations,
                           std::string &salt_hex, std::string &hash_hex)
{
    const size_t first_sep = record.find('$');
    if (first_sep == std::string::npos || record.compare(0, first_sep, kPasswordScheme) != 0)
    {
        return false;
    }

    const size_t second_sep = record.find('$', first_sep + 1);
    const size_t third_sep = record.find('$', second_sep == std::string::npos ? second_sep : second_sep + 1);
    if (second_sep == std::string::npos || third_sep == std::string::npos)
    {
        return false;
    }

    const std::string iterations_str = record.substr(first_sep + 1, second_sep - first_sep - 1);
    const std::string parsed_salt = record.substr(second_sep + 1, third_sep - second_sep - 1);
    const std::string parsed_hash = record.substr(third_sep + 1);
    if (iterations_str.empty() || parsed_salt.empty() || parsed_hash.empty())
    {
        return false;
    }

    iterations = atoi(iterations_str.c_str());
    if (iterations <= 0)
    {
        return false;
    }

    salt_hex = parsed_salt;
    hash_hex = parsed_hash;
    return true;
}

bool secure_equals(const std::string &left, const std::string &right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}
}

std::string HttpConnection::make_password_salt() const
{
    std::string salt;
    secure_random_hex(kPasswordSaltBytes, salt);
    return salt;
}

std::string HttpConnection::hash_password(const string &password, const string &salt) const
{
    std::string derived_hex;
    if (!derive_pbkdf2_sha256(password, salt, kPasswordIterations, derived_hex))
    {
        return "";
    }
    return derived_hex;
}

std::string HttpConnection::make_password_record(const string &password) const
{
    const std::string salt = make_password_salt();
    if (salt.empty())
    {
        return "";
    }

    const std::string password_hash = hash_password(password, salt);
    if (password_hash.empty())
    {
        return "";
    }

    return build_password_record(salt, password_hash);
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
    if (mysql == nullptr || token.empty())
    {
        return false;
    }

    const time_t now = time(nullptr);
    locker &cache_lock = auth_cache_lock();
    std::map<std::string, AuthSessionCacheEntry> &session_cache = auth_session_cache();
    cache_lock.lock();
    std::map<std::string, AuthSessionCacheEntry>::iterator session_it = session_cache.find(token);
    if (session_it != session_cache.end())
    {
        if (session_it->second.expires_at > now)
        {
            username = session_it->second.username;
            const bool refresh_needed =
                session_it->second.expires_at - now <= kSessionRefreshThresholdSeconds;
            cache_lock.unlock();
            if (!refresh_needed)
            {
                return !username.empty();
            }
            if (refresh_session(token, username, kSessionTtlSeconds))
            {
                return !username.empty();
            }

            cache_lock.lock();
            session_cache.erase(token);
            cache_lock.unlock();
            username.clear();
        }
        else
        {
            session_cache.erase(session_it);
            cache_lock.unlock();
        }
    }
    else
    {
        cache_lock.unlock();
    }

    const std::string escaped_token = escape_sql_value(token);
    const std::string sql = "SELECT username, UNIX_TIMESTAMP(expires_at) FROM user_sessions "
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
        const std::string cleanup = "DELETE FROM user_sessions WHERE token='" + escaped_token +
                                    "' OR expires_at <= NOW()";
        mysql_query(mysql, cleanup.c_str());
        return false;
    }

    username = row[0] ? row[0] : "";
    const time_t expires_at = row[1] ? static_cast<time_t>(strtoll(row[1], nullptr, 10)) : 0;
    mysql_free_result(result);

    if (username.empty() || expires_at <= now)
    {
        return false;
    }

    cache_lock.lock();
    session_cache[token] = AuthSessionCacheEntry(username, expires_at);
    cache_lock.unlock();

    if (expires_at - now <= kSessionRefreshThresholdSeconds)
    {
        refresh_session(token, username, kSessionTtlSeconds);
    }
    return true;
}

bool HttpConnection::persist_session(const string &token, const string &username, int ttl_seconds)
{
    if (mysql == nullptr || token.empty() || username.empty() || ttl_seconds <= 0)
    {
        return false;
    }

    char ttl_buffer[32];
    snprintf(ttl_buffer, sizeof(ttl_buffer), "%d", ttl_seconds);
    const std::string sql = "INSERT INTO user_sessions(token, username, expires_at) "
                            "VALUES('" + escape_sql_value(token) + "', '" + escape_sql_value(username) +
                            "', DATE_ADD(NOW(), INTERVAL " + std::string(ttl_buffer) + " SECOND)) "
                            "ON DUPLICATE KEY UPDATE username=VALUES(username), expires_at=VALUES(expires_at)";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    auth_cache_lock().lock();
    auth_session_cache()[token] = AuthSessionCacheEntry(username, time(nullptr) + ttl_seconds);
    auth_cache_lock().unlock();
    return true;
}

bool HttpConnection::refresh_session(const string &token, const string &username, int ttl_seconds)
{
    if (mysql == nullptr || token.empty() || username.empty() || ttl_seconds <= 0)
    {
        return false;
    }

    char ttl_buffer[32];
    snprintf(ttl_buffer, sizeof(ttl_buffer), "%d", ttl_seconds);
    const std::string sql = "UPDATE user_sessions SET expires_at=DATE_ADD(NOW(), INTERVAL " +
                            std::string(ttl_buffer) + " SECOND) WHERE token='" +
                            escape_sql_value(token) + "' AND username='" +
                            escape_sql_value(username) + "' AND expires_at > NOW()";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    if (mysql_affected_rows(mysql) == 0)
    {
        return false;
    }

    auth_cache_lock().lock();
    auth_session_cache()[token] = AuthSessionCacheEntry(username, time(nullptr) + ttl_seconds);
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

    const std::string sql = "DELETE FROM user_sessions WHERE token='" + escape_sql_value(token) + "'";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool HttpConnection::remove_user_sessions(const string &username, const string &except_token)
{
    if (username.empty())
    {
        return false;
    }

    auth_cache_lock().lock();
    std::map<std::string, AuthSessionCacheEntry> &session_cache = auth_session_cache();
    for (std::map<std::string, AuthSessionCacheEntry>::iterator it = session_cache.begin();
         it != session_cache.end();)
    {
        const bool same_user = it->second.username == username;
        const bool keep_session = !except_token.empty() && it->first == except_token;
        if (same_user && !keep_session)
        {
            it = session_cache.erase(it);
            continue;
        }
        ++it;
    }
    auth_cache_lock().unlock();

    if (mysql == nullptr)
    {
        return true;
    }

    std::string sql = "DELETE FROM user_sessions WHERE username='" + escape_sql_value(username) + "'";
    if (!except_token.empty())
    {
        sql += " AND token<>'" + escape_sql_value(except_token) + "'";
    }
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool HttpConnection::update_user_password_hash(const string &username, const string &password)
{
    if (mysql == nullptr)
    {
        return false;
    }

    const std::string password_record = make_password_record(password);
    if (password_record.empty())
    {
        return false;
    }
    const std::string sql = "UPDATE user SET passwd='" + escape_sql_value(password_record) +
                            "', passwd_salt='' WHERE username='" + escape_sql_value(username) + "'";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    auth_cache_lock().lock();
    auth_user_cache()[username] = password_record;
    auth_cache_lock().unlock();
    return true;
}

bool HttpConnection::verify_user_password(const string &username, const string &password)
{
    if (mysql == nullptr)
    {
        return false;
    }

    const std::string sql = "SELECT passwd, COALESCE(passwd_salt, '') FROM user WHERE username='" +
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

    const std::string stored_password = row[0] ? row[0] : "";
    const std::string stored_salt = row[1] ? row[1] : "";
    mysql_free_result(result);

    int iterations = 0;
    std::string salt_hex;
    std::string expected_hash;
    if (parse_password_record(stored_password, iterations, salt_hex, expected_hash))
    {
        std::string candidate_hash;
        return derive_pbkdf2_sha256(password, salt_hex, iterations, candidate_hash) &&
               secure_equals(candidate_hash, expected_hash);
    }

    if (stored_salt.empty())
    {
        if (stored_password == password)
        {
            update_user_password_hash(username, password);
            return true;
        }
        return false;
    }

    if (secure_equals(stored_password, legacy_sha256_hash(password, stored_salt)))
    {
        update_user_password_hash(username, password);
        return true;
    }
    return false;
}

HttpConnection::HTTP_CODE HttpConnection::middleware_auth()
{
    if (!requires_auth())
    {
        return NO_REQUEST;
    }

    const std::string token = extract_bearer_token();
    if (!token.empty() && lookup_session(token, m_current_user))
    {
        return NO_REQUEST;
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
