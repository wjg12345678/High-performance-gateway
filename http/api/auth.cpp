#include "../core/connection.h"
#include "auth_state.h"

#include <cstring>

using namespace std;

namespace
{
const int kSessionTtlSeconds = 7 * 24 * 3600;

struct JsonResponseSpec
{
    int status;
    const char *title;
    const char *body;
};

bool is_truthy_value(const string &value)
{
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "on";
}
}

HttpConnection::HTTP_CODE HttpConnection::route_api_login()
{
    return handle_auth_request(false, true);
}

HttpConnection::HTTP_CODE HttpConnection::route_api_register()
{
    return handle_auth_request(true, true);
}

HttpConnection::HTTP_CODE HttpConnection::route_api_private_logout()
{
    return handle_logout_request();
}

HttpConnection::HTTP_CODE HttpConnection::handle_auth_request(bool is_register, bool api_mode)
{
    string name = request_value("username", "user");
    string password = request_value("passwd", "password");
    if (name.empty() || password.empty())
    {
        if (api_mode)
        {
            set_memory_response(400, "Bad Request",
                                "{\"code\":400,\"message\":\"username or password is empty\"}",
                                "application/json");
            return MEMORY_REQUEST;
        }
        return BAD_REQUEST;
    }

    bool success = false;
    if (is_register)
    {
        const string password_record = make_password_record(password);
        if (password_record.empty())
        {
            return api_mode
                       ? respond_json_error(500, "Internal Error", "failed to prepare password hash")
                       : INTERNAL_ERROR;
        }

        const string sql_insert = "INSERT INTO user(username, passwd, passwd_salt) VALUES('" +
                                  escape_sql_value(name) + "', '" +
                                  escape_sql_value(password_record) + "', '')";

        map<string, string> &user_cache = auth_user_cache();
        locker &cache_lock = auth_cache_lock();
        cache_lock.lock();
        if (user_cache.find(name) == user_cache.end())
        {
            int res = mysql_query(mysql, sql_insert.c_str());
            if (!res)
            {
                user_cache[name] = password_record;
                success = true;
                write_operation_log(name, "register", "user", 0, "register success");
            }
        }
        cache_lock.unlock();

        if (api_mode)
        {
            const JsonResponseSpec response = success
                                                  ? JsonResponseSpec{200, "OK", "{\"code\":0,\"message\":\"register success\"}"}
                                                  : JsonResponseSpec{409, "Conflict", "{\"code\":409,\"message\":\"register failed\"}"};
            set_memory_response(response.status, response.title, response.body, "application/json");
            return MEMORY_REQUEST;
        }

        set_request_target(success ? "/login" : "/register-error");
        return do_request();
    }

    success = verify_user_password(name, password);
    if (api_mode)
    {
        if (success)
        {
            const string token = make_session_token(name);
            if (token.empty())
            {
                return respond_json_error(500, "Internal Error", "failed to issue session");
            }
            if (!persist_session(token, name, kSessionTtlSeconds))
            {
                return respond_json_error(500, "Internal Error", "failed to persist session");
            }
            if (!remove_user_sessions(name, token))
            {
                remove_session(token);
                return respond_json_error(500, "Internal Error", "failed to revoke previous sessions");
            }
            m_current_user = name;
            write_operation_log(name, "login", "user", 0, "login success");

            string response = string("{\"code\":0,\"message\":\"login success\",\"target\":\"/welcome\",\"token\":\"") +
                              json_escape(token) + "\",\"expires_in\":604800}";
            set_memory_response(200, "OK", response, "application/json");
        }
        else
        {
            set_memory_response(401, "Unauthorized",
                                "{\"code\":401,\"message\":\"login failed\"}",
                                "application/json");
            write_operation_log(name, "login_failed", "user", 0, "invalid username or password");
        }
        return MEMORY_REQUEST;
    }

    set_request_target(success ? "/welcome" : "/login-error");
    return do_request();
}

HttpConnection::HTTP_CODE HttpConnection::handle_logout_request()
{
    const string token = extract_bearer_token();
    if (token.empty())
    {
        set_memory_response(400, "Bad Request",
                            "{\"code\":400,\"message\":\"missing bearer token\"}",
                            "application/json");
        return MEMORY_REQUEST;
    }

    const bool logout_all = request_value("scope") == "all" ||
                            is_truthy_value(request_value("all_sessions"));
    const bool removed = logout_all ? remove_user_sessions(m_current_user, "")
                                    : remove_session(token);
    if (!removed)
    {
        return respond_json_error(500, "Internal Error", "failed to invalidate session");
    }

    if (!m_current_user.empty())
    {
        write_operation_log(m_current_user,
                            logout_all ? "logout_all" : "logout",
                            "user", 0,
                            logout_all ? "all sessions revoked" : "logout success");
    }

    const string response = logout_all
                                ? "{\"code\":0,\"message\":\"logout success\",\"scope\":\"all\"}"
                                : "{\"code\":0,\"message\":\"logout success\",\"scope\":\"current\"}";
    set_memory_response(200, "OK", response, "application/json");
    return MEMORY_REQUEST;
}
