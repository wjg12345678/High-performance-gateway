#include "../core/connection.h"
#include "auth_state.h"

#include <cstring>

using namespace std;

namespace
{
void copy_route_target(char *target, const char *path, size_t capacity)
{
    if (capacity == 0)
    {
        return;
    }
    strncpy(target, path, capacity - 1);
    target[capacity - 1] = '\0';
}

struct JsonResponseSpec
{
    int status;
    const char *title;
    const char *body;
};
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
        string password_salt = make_password_salt();
        string password_hash = hash_password(password, password_salt);
        const string sql_insert = "INSERT INTO user(username, passwd, passwd_salt) VALUES('" +
                                  escape_sql_value(name) + "', '" +
                                  escape_sql_value(password_hash) + "', '" +
                                  escape_sql_value(password_salt) + "')";

        map<string, string> &user_cache = auth_user_cache();
        locker &cache_lock = auth_cache_lock();
        if (user_cache.find(name) == user_cache.end())
        {
            cache_lock.lock();
            int res = mysql_query(mysql, sql_insert.c_str());
            if (!res)
            {
                user_cache.insert(pair<string, string>(name, password_hash));
                success = true;
                write_operation_log(name, "register", "user", 0, "register success");
            }
            cache_lock.unlock();
        }

        if (api_mode)
        {
            const JsonResponseSpec response = success
                                                  ? JsonResponseSpec{200, "OK", "{\"code\":0,\"message\":\"register success\"}"}
                                                  : JsonResponseSpec{409, "Conflict", "{\"code\":409,\"message\":\"register failed\"}"};
            set_memory_response(response.status, response.title, response.body, "application/json");
            return MEMORY_REQUEST;
        }

        copy_route_target(m_url, success ? "/login.html" : "/register-error.html", READ_BUFFER_INITIAL_SIZE);
        return do_request();
    }

    success = verify_user_password(name, password);
    if (api_mode)
    {
        if (success)
        {
            string token = make_session_token(name);
            if (!persist_session(token, name, 7 * 24 * 3600))
            {
                return INTERNAL_ERROR;
            }
            m_current_user = name;
            write_operation_log(name, "login", "user", 0, "login success");

            string response = string("{\"code\":0,\"message\":\"login success\",\"target\":\"/welcome.html\",\"token\":\"") +
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

    copy_route_target(m_url, success ? "/welcome.html" : "/login-error.html", READ_BUFFER_INITIAL_SIZE);
    return do_request();
}

HttpConnection::HTTP_CODE HttpConnection::handle_logout_request()
{
    string token = extract_bearer_token();
    if (token.empty())
    {
        set_memory_response(400, "Bad Request",
                            "{\"code\":400,\"message\":\"missing bearer token\"}",
                            "application/json");
        return MEMORY_REQUEST;
    }

    remove_session(token);
    if (!m_current_user.empty() && m_current_user != "admin")
    {
        write_operation_log(m_current_user, "logout", "user", 0, "logout success");
    }
    set_memory_response(200, "OK",
                        "{\"code\":0,\"message\":\"logout success\"}",
                        "application/json");
    return MEMORY_REQUEST;
}
