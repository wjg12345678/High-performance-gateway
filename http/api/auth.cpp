#include "../core/connection.h"
#include "../../service/auth/auth_service.h"

using namespace std;

namespace
{
string auth_json_escape(const string &value)
{
    string escaped;
    escaped.reserve(value.size());
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                escaped += " ";
            }
            else
            {
                escaped += ch;
            }
            break;
        }
    }
    return escaped;
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

    service_auth::AuthResult result;
    if (is_register)
    {
        service_auth::register_user(mysql, name, password, result);
        if (result.success)
        {
            write_operation_log(name, "register", "user", 0, "register success");
        }

        if (api_mode)
        {
            const string body = result.success
                                    ? "{\"code\":0,\"message\":\"register success\"}"
                                    : string("{\"code\":") + to_string(result.status) + ",\"message\":\"" +
                                          auth_json_escape(result.message) + "\"}";
            set_memory_response(result.status, result.title, body, "application/json");
            return MEMORY_REQUEST;
        }

        if (!result.success && result.status >= 500)
        {
            return INTERNAL_ERROR;
        }
        set_request_target(result.success ? "/login" : "/register-error");
        return do_request();
    }

    service_auth::login_user(mysql, name, password, result);
    if (api_mode)
    {
        if (result.success)
        {
            m_current_user = name;
            write_operation_log(name, "login", "user", 0, "login success");

            string response = string("{\"code\":0,\"message\":\"login success\",\"target\":\"/welcome\",\"token\":\"") +
                              auth_json_escape(result.token) + "\",\"expires_in\":" + to_string(result.expires_in) + "}";
            set_memory_response(200, "OK", response, "application/json");
        }
        else
        {
            if (result.status >= 500)
            {
                return respond_json_error(result.status, result.title, result.message);
            }
            set_memory_response(401, "Unauthorized",
                                "{\"code\":401,\"message\":\"login failed\"}",
                                "application/json");
            write_operation_log(name, "login_failed", "user", 0, "invalid username or password");
        }
        return MEMORY_REQUEST;
    }

    set_request_target(result.success ? "/welcome" : "/login-error");
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
                            service_auth::is_truthy_value(request_value("all_sessions"));
    if (!service_auth::logout(mysql, m_current_user, token, logout_all))
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
