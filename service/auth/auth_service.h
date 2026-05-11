#ifndef SERVICE_AUTH_AUTH_SERVICE_H
#define SERVICE_AUTH_AUTH_SERVICE_H

#include <mysql/mysql.h>
#include <string>

namespace service_auth
{
enum class AuthError
{
    None,
    Unauthorized,
    NotFound,
    Conflict,
    Internal
};

struct AuthResult
{
    bool success;
    AuthError error;
    std::string message;
    std::string token;
    int expires_in;

    AuthResult() : success(false), error(AuthError::Internal), expires_in(0) {}
};

bool is_truthy_value(const std::string &value);
bool load_user_cache(MYSQL *mysql);
bool register_user(MYSQL *mysql, const std::string &username, const std::string &password, AuthResult &result);
bool login_user(MYSQL *mysql, const std::string &username, const std::string &password, AuthResult &result);
bool lookup_session(MYSQL *mysql, const std::string &token, std::string &username);
bool remove_session(MYSQL *mysql, const std::string &token);
bool remove_user_sessions(MYSQL *mysql, const std::string &username, const std::string &except_token);
bool logout(MYSQL *mysql, const std::string &username, const std::string &token, bool logout_all);
}

#endif
