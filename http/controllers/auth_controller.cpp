#include "auth_controller.h"

#include "../../service/auth/auth_service.h"

#include <string>

using namespace std;

namespace http_controllers
{
HttpConnection::HTTP_CODE AuthController::login(HttpConnection &conn, bool api_mode)
{
    string name = conn.request_value("username", "user");
    string password = conn.request_value("passwd", "password");
    if (name.empty() || password.empty())
    {
        if (api_mode)
        {
            conn.set_memory_response(400, "Bad Request",
                                     "{\"code\":400,\"message\":\"username or password is empty\"}",
                                     "application/json");
            return HttpConnection::MEMORY_REQUEST;
        }
        return HttpConnection::BAD_REQUEST;
    }

    service_auth::AuthResult result;
    service_auth::login_user(conn.mysql, name, password, result);
    if (api_mode)
    {
        if (result.success)
        {
            conn.m_current_user = name;
            conn.write_operation_log(name, "login", "user", 0, "login success");

            string response = string("{\"code\":0,\"message\":\"login success\",\"target\":\"/welcome\",\"token\":\"") +
                              conn.json_escape(result.token) + "\",\"expires_in\":" + to_string(result.expires_in) + "}";
            conn.set_memory_response(200, "OK", response, "application/json");
        }
        else
        {
            if (result.status >= 500)
            {
                return conn.respond_json_error(result.status, result.title, result.message);
            }
            conn.set_memory_response(401, "Unauthorized",
                                     "{\"code\":401,\"message\":\"login failed\"}",
                                     "application/json");
            conn.write_operation_log(name, "login_failed", "user", 0, "invalid username or password");
        }
        return HttpConnection::MEMORY_REQUEST;
    }

    conn.set_request_target(result.success ? "/welcome" : "/login-error");
    return conn.do_request();
}

HttpConnection::HTTP_CODE AuthController::register_user(HttpConnection &conn, bool api_mode)
{
    string name = conn.request_value("username", "user");
    string password = conn.request_value("passwd", "password");
    if (name.empty() || password.empty())
    {
        if (api_mode)
        {
            conn.set_memory_response(400, "Bad Request",
                                     "{\"code\":400,\"message\":\"username or password is empty\"}",
                                     "application/json");
            return HttpConnection::MEMORY_REQUEST;
        }
        return HttpConnection::BAD_REQUEST;
    }

    service_auth::AuthResult result;
    service_auth::register_user(conn.mysql, name, password, result);
    if (result.success)
    {
        conn.write_operation_log(name, "register", "user", 0, "register success");
    }

    if (api_mode)
    {
        const string body = result.success
                                ? "{\"code\":0,\"message\":\"register success\"}"
                                : string("{\"code\":") + to_string(result.status) + ",\"message\":\"" +
                                      conn.json_escape(result.message) + "\"}";
        conn.set_memory_response(result.status, result.title, body, "application/json");
        return HttpConnection::MEMORY_REQUEST;
    }

    if (!result.success && result.status >= 500)
    {
        return HttpConnection::INTERNAL_ERROR;
    }
    conn.set_request_target(result.success ? "/login" : "/register-error");
    return conn.do_request();
}

HttpConnection::HTTP_CODE AuthController::logout(HttpConnection &conn)
{
    const string token = conn.extract_bearer_token();
    if (token.empty())
    {
        conn.set_memory_response(400, "Bad Request",
                                 "{\"code\":400,\"message\":\"missing bearer token\"}",
                                 "application/json");
        return HttpConnection::MEMORY_REQUEST;
    }

    const bool logout_all = conn.request_value("scope") == "all" ||
                            service_auth::is_truthy_value(conn.request_value("all_sessions"));
    if (!service_auth::logout(conn.mysql, conn.m_current_user, token, logout_all))
    {
        return conn.respond_json_error(500, "Internal Error", "failed to invalidate session");
    }

    if (!conn.m_current_user.empty())
    {
        conn.write_operation_log(conn.m_current_user,
                                 logout_all ? "logout_all" : "logout",
                                 "user", 0,
                                 logout_all ? "all sessions revoked" : "logout success");
    }

    const string response = logout_all
                                ? "{\"code\":0,\"message\":\"logout success\",\"scope\":\"all\"}"
                                : "{\"code\":0,\"message\":\"logout success\",\"scope\":\"current\"}";
    conn.set_memory_response(200, "OK", response, "application/json");
    return HttpConnection::MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE AuthController::ping(HttpConnection &conn)
{
    string body = string("{\"code\":0,\"message\":\"pong\",\"user\":\"") +
                  conn.json_escape(conn.m_current_user) + "\"}";
    conn.set_memory_response(200, "OK", body, "application/json");
    return HttpConnection::MEMORY_REQUEST;
}
}
