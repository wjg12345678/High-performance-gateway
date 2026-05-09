#include "auth_service.h"

#include "../../infra/lock/locker.h"
#include "../../repo/mysql/session_repository.h"
#include "../../repo/mysql/user_repository.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
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
const size_t kSessionTokenBytes = 32;
const int kPasswordIterations = 210000;
const int kSessionTtlSeconds = 7 * 24 * 3600;
const int kSessionRefreshThresholdSeconds = 24 * 3600;

struct AuthSessionCacheEntry
{
    std::string username;
    time_t expires_at;

    AuthSessionCacheEntry() : expires_at(0) {}
    AuthSessionCacheEntry(const std::string &value, time_t expiry)
        : username(value), expires_at(expiry) {}
};

locker g_auth_cache_lock;
std::map<std::string, std::string> g_auth_users;
std::map<std::string, AuthSessionCacheEntry> g_auth_sessions;

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

std::string make_password_record(const std::string &password)
{
    std::string salt;
    secure_random_hex(kPasswordSaltBytes, salt);
    if (salt.empty())
    {
        return "";
    }

    std::string password_hash;
    if (!derive_pbkdf2_sha256(password, salt, kPasswordIterations, password_hash))
    {
        return "";
    }
    return build_password_record(salt, password_hash);
}

std::string make_session_token()
{
    std::string token;
    secure_random_hex(kSessionTokenBytes, token);
    return token;
}

void set_error(service_auth::AuthResult &result, int status, const char *title, const std::string &message)
{
    result.success = false;
    result.status = status;
    result.title = title;
    result.message = message;
}

bool persist_session(MYSQL *mysql, const std::string &token, const std::string &username, int ttl_seconds)
{
    if (mysql == nullptr || token.empty() || username.empty() || ttl_seconds <= 0)
    {
        return false;
    }
    if (!repo_mysql::upsert_session(mysql, token, username, ttl_seconds))
    {
        return false;
    }

    g_auth_cache_lock.lock();
    g_auth_sessions[token] = AuthSessionCacheEntry(username, time(nullptr) + ttl_seconds);
    g_auth_cache_lock.unlock();
    return true;
}

bool refresh_session(MYSQL *mysql, const std::string &token, const std::string &username, int ttl_seconds)
{
    if (mysql == nullptr || token.empty() || username.empty() || ttl_seconds <= 0)
    {
        return false;
    }

    bool updated = false;
    if (!repo_mysql::refresh_session(mysql, token, username, ttl_seconds, updated) || !updated)
    {
        return false;
    }

    g_auth_cache_lock.lock();
    g_auth_sessions[token] = AuthSessionCacheEntry(username, time(nullptr) + ttl_seconds);
    g_auth_cache_lock.unlock();
    return true;
}

bool update_user_password_hash(MYSQL *mysql, const std::string &username, const std::string &password)
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
    if (!repo_mysql::update_user_password(mysql, username, password_record))
    {
        return false;
    }

    g_auth_cache_lock.lock();
    g_auth_users[username] = password_record;
    g_auth_cache_lock.unlock();
    return true;
}

bool verify_user_password(MYSQL *mysql, const std::string &username, const std::string &password)
{
    if (mysql == nullptr)
    {
        return false;
    }

    repo_mysql::UserPasswordRecord password_record;
    if (!repo_mysql::fetch_user_password(mysql, username, password_record))
    {
        return false;
    }

    const std::string stored_password = password_record.password;
    const std::string stored_salt = password_record.salt;

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
            update_user_password_hash(mysql, username, password);
            return true;
        }
        return false;
    }

    if (secure_equals(stored_password, legacy_sha256_hash(password, stored_salt)))
    {
        update_user_password_hash(mysql, username, password);
        return true;
    }
    return false;
}
}

namespace service_auth
{
bool is_truthy_value(const std::string &value)
{
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on";
}

bool load_user_cache(MYSQL *mysql)
{
    if (mysql == nullptr)
    {
        return false;
    }

    std::map<std::string, std::string> users;
    if (!repo_mysql::fetch_all_user_passwords(mysql, users))
    {
        return false;
    }

    g_auth_cache_lock.lock();
    g_auth_users.swap(users);
    g_auth_cache_lock.unlock();
    return true;
}

bool register_user(MYSQL *mysql, const std::string &username, const std::string &password, AuthResult &result)
{
    const std::string password_record = make_password_record(password);
    if (password_record.empty())
    {
        set_error(result, 500, "Internal Error", "failed to prepare password hash");
        return false;
    }

    bool success = false;
    g_auth_cache_lock.lock();
    if (g_auth_users.find(username) == g_auth_users.end())
    {
        if (repo_mysql::insert_user(mysql, username, password_record))
        {
            g_auth_users[username] = password_record;
            success = true;
        }
    }
    g_auth_cache_lock.unlock();

    if (!success)
    {
        set_error(result, 409, "Conflict", "register failed");
        return false;
    }

    result.success = true;
    result.status = 200;
    result.title = "OK";
    result.message = "register success";
    return true;
}

bool login_user(MYSQL *mysql, const std::string &username, const std::string &password, AuthResult &result)
{
    if (!verify_user_password(mysql, username, password))
    {
        set_error(result, 401, "Unauthorized", "login failed");
        return false;
    }

    const std::string token = make_session_token();
    if (token.empty())
    {
        set_error(result, 500, "Internal Error", "failed to issue session");
        return false;
    }
    if (!persist_session(mysql, token, username, kSessionTtlSeconds))
    {
        set_error(result, 500, "Internal Error", "failed to persist session");
        return false;
    }
    if (!remove_user_sessions(mysql, username, token))
    {
        remove_session(mysql, token);
        set_error(result, 500, "Internal Error", "failed to revoke previous sessions");
        return false;
    }

    result.success = true;
    result.status = 200;
    result.title = "OK";
    result.message = "login success";
    result.token = token;
    result.expires_in = kSessionTtlSeconds;
    return true;
}

bool lookup_session(MYSQL *mysql, const std::string &token, std::string &username)
{
    if (mysql == nullptr || token.empty())
    {
        return false;
    }

    const time_t now = time(nullptr);
    g_auth_cache_lock.lock();
    std::map<std::string, AuthSessionCacheEntry>::iterator session_it = g_auth_sessions.find(token);
    if (session_it != g_auth_sessions.end())
    {
        if (session_it->second.expires_at > now)
        {
            username = session_it->second.username;
            const bool refresh_needed = session_it->second.expires_at - now <= kSessionRefreshThresholdSeconds;
            g_auth_cache_lock.unlock();
            if (!refresh_needed)
            {
                return !username.empty();
            }
            if (refresh_session(mysql, token, username, kSessionTtlSeconds))
            {
                return !username.empty();
            }

            g_auth_cache_lock.lock();
            g_auth_sessions.erase(token);
            g_auth_cache_lock.unlock();
            username.clear();
        }
        else
        {
            g_auth_sessions.erase(session_it);
            g_auth_cache_lock.unlock();
        }
    }
    else
    {
        g_auth_cache_lock.unlock();
    }

    repo_mysql::SessionRecord session_record;
    if (!repo_mysql::find_active_session(mysql, token, session_record))
    {
        repo_mysql::cleanup_session_token_or_expired(mysql, token);
        return false;
    }

    username = session_record.username;
    const time_t expires_at = session_record.expires_at;
    if (username.empty() || expires_at <= now)
    {
        return false;
    }

    g_auth_cache_lock.lock();
    g_auth_sessions[token] = AuthSessionCacheEntry(username, expires_at);
    g_auth_cache_lock.unlock();

    if (expires_at - now <= kSessionRefreshThresholdSeconds)
    {
        refresh_session(mysql, token, username, kSessionTtlSeconds);
    }
    return true;
}

bool remove_session(MYSQL *mysql, const std::string &token)
{
    if (token.empty())
    {
        return false;
    }

    g_auth_cache_lock.lock();
    g_auth_sessions.erase(token);
    g_auth_cache_lock.unlock();

    if (mysql == nullptr)
    {
        return true;
    }
    return repo_mysql::delete_session(mysql, token);
}

bool remove_user_sessions(MYSQL *mysql, const std::string &username, const std::string &except_token)
{
    if (username.empty())
    {
        return false;
    }

    g_auth_cache_lock.lock();
    for (std::map<std::string, AuthSessionCacheEntry>::iterator it = g_auth_sessions.begin();
         it != g_auth_sessions.end();)
    {
        const bool same_user = it->second.username == username;
        const bool keep_session = !except_token.empty() && it->first == except_token;
        if (same_user && !keep_session)
        {
            it = g_auth_sessions.erase(it);
            continue;
        }
        ++it;
    }
    g_auth_cache_lock.unlock();

    if (mysql == nullptr)
    {
        return true;
    }
    return repo_mysql::delete_user_sessions(mysql, username, except_token);
}

bool logout(MYSQL *mysql, const std::string &username, const std::string &token, bool logout_all)
{
    return logout_all ? remove_user_sessions(mysql, username, "") : remove_session(mysql, token);
}
}
