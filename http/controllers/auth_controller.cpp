#include "auth_controller.h"

#include "../../repo/mysql/operation_repository.h"
#include "../../service/auth/auth_service.h"
#include "../../service/rate_limit/auth_rate_limiter.h"

#include <string>

using namespace std;

namespace
{
bool write_operation_log(MYSQL *mysql, const string &username, const string &action,
                         const string &resource_type, long resource_id, const string &detail)
{
    return repo_mysql::insert_operation_log(mysql, username, action, resource_type, resource_id, detail);
}

string first_forwarded_ip(const string &value)
{
    const size_t comma = value.find(',');
    return http_core::trim_copy(comma == string::npos ? value : value.substr(0, comma));
}

string request_client_ip(const http_core::HttpRequest &request, const http_core::RequestContext &context)
{
    const string real_ip = http_core::trim_copy(request.header_value("x-real-ip"));
    if (!real_ip.empty())
    {
        return real_ip;
    }

    const string forwarded_for = first_forwarded_ip(request.header_value("x-forwarded-for"));
    if (!forwarded_for.empty())
    {
        return forwarded_for;
    }

    return context.client_ip.empty() ? "unknown" : context.client_ip;
}

http_core::HttpCode write_rate_limited_response(http_core::HttpResponse &response,
                                                const service_rate_limit::AuthRateLimitDecision &decision)
{
    int retry_after_seconds = decision.retry_after_ms > 0
                                  ? (decision.retry_after_ms + 999) / 1000
                                  : 1;
    if (retry_after_seconds <= 0)
    {
        retry_after_seconds = 1;
    }

    response.set_body(429, "Too Many Requests",
                      "{\"code\":429,\"message\":\"请求过于频繁，请稍后再试。\"}",
                      "application/json");
    response.headers["Retry-After"] = to_string(retry_after_seconds);
    response.headers["X-RateLimit-Backend"] = decision.backend;
    response.headers["X-RateLimit-Scope"] = decision.scope;
    return http_core::MEMORY_REQUEST;
}

int auth_status(service_auth::AuthError error)
{
    switch (error)
    {
    case service_auth::AuthError::None: return 200;
    case service_auth::AuthError::Unauthorized: return 401;
    case service_auth::AuthError::NotFound: return 404;
    case service_auth::AuthError::Conflict: return 409;
    case service_auth::AuthError::Internal:
    default: return 500;
    }
}

const char *auth_title(service_auth::AuthError error)
{
    switch (error)
    {
    case service_auth::AuthError::None: return "OK";
    case service_auth::AuthError::Unauthorized: return "Unauthorized";
    case service_auth::AuthError::NotFound: return "Not Found";
    case service_auth::AuthError::Conflict: return "Conflict";
    case service_auth::AuthError::Internal:
    default: return "Internal Error";
    }
}
}

namespace http_controllers
{
http_core::HttpCode AuthController::login(const http_core::HttpRequest &request,
                                          http_core::RequestContext &context,
                                          http_core::HttpResponse &response,
                                          bool api_mode)
{
    string name = request.value("username", "user");
    string password = request.value("passwd", "password");
    if (name.empty() || password.empty())
    {
        if (api_mode)
        {
            response.set_body(400, "Bad Request",
                              "{\"code\":400,\"message\":\"请输入用户名和密码。\"}",
                              "application/json");
            return http_core::MEMORY_REQUEST;
        }
        return http_core::BAD_REQUEST;
    }

    service_rate_limit::AuthRateLimitDecision rate_limit =
        service_rate_limit::check_auth_rate_limit(service_rate_limit::AuthRateLimitAction::Login,
                                                  name, request_client_ip(request, context));
    if (!rate_limit.allowed)
    {
        write_operation_log(context.mysql, name, "login_rate_limited", "user", 0, rate_limit.scope);
        return write_rate_limited_response(response, rate_limit);
    }

    service_auth::AuthResult result;
    service_auth::login_user(context.mysql, name, password, result);
    if (api_mode)
    {
        if (result.success)
        {
            context.current_user = name;
            write_operation_log(context.mysql, name, "login", "user", 0, "login success");

            string body = string("{\"code\":0,\"message\":\"login success\",\"target\":\"/welcome\",\"token\":\"") +
                          http_core::json_escape(result.token) + "\",\"expires_in\":" +
                          to_string(result.expires_in) + "}";
            response.set_body(200, "OK", body, "application/json");
        }
        else
        {
            const int status = auth_status(result.error);
            string body = string("{\"code\":") + to_string(status) + ",\"message\":\"" +
                          http_core::json_escape(result.message) + "\"}";
            response.set_body(status, auth_title(result.error), body, "application/json");
            write_operation_log(context.mysql, name, "login_failed", "user", 0, result.message);
        }
        return http_core::MEMORY_REQUEST;
    }

    return result.success ? http_core::NO_RESOURCE : http_core::BAD_REQUEST;
}

http_core::HttpCode AuthController::register_user(const http_core::HttpRequest &request,
                                                  http_core::RequestContext &context,
                                                  http_core::HttpResponse &response,
                                                  bool api_mode)
{
    string name = request.value("username", "user");
    string password = request.value("passwd", "password");
    if (name.empty() || password.empty())
    {
        if (api_mode)
        {
            response.set_body(400, "Bad Request",
                              "{\"code\":400,\"message\":\"请输入用户名和密码。\"}",
                              "application/json");
            return http_core::MEMORY_REQUEST;
        }
        return http_core::BAD_REQUEST;
    }

    service_rate_limit::AuthRateLimitDecision rate_limit =
        service_rate_limit::check_auth_rate_limit(service_rate_limit::AuthRateLimitAction::Register,
                                                  name, request_client_ip(request, context));
    if (!rate_limit.allowed)
    {
        write_operation_log(context.mysql, name, "register_rate_limited", "user", 0, rate_limit.scope);
        return write_rate_limited_response(response, rate_limit);
    }

    service_auth::AuthResult result;
    service_auth::register_user(context.mysql, name, password, result);
    if (result.success)
    {
        write_operation_log(context.mysql, name, "register", "user", 0, "register success");
    }

    if (api_mode)
    {
        const int status = result.success ? 200 : auth_status(result.error);
        const string body = result.success
                                ? "{\"code\":0,\"message\":\"register success\"}"
                                : string("{\"code\":") + to_string(status) + ",\"message\":\"" +
                                      http_core::json_escape(result.message) + "\"}";
        response.set_body(status, result.success ? "OK" : auth_title(result.error), body, "application/json");
        return http_core::MEMORY_REQUEST;
    }

    return result.success ? http_core::NO_RESOURCE : http_core::BAD_REQUEST;
}

http_core::HttpCode AuthController::logout(const http_core::HttpRequest &request,
                                           http_core::RequestContext &context,
                                           http_core::HttpResponse &response)
{
    const string token = request.bearer_token();
    if (token.empty())
    {
        response.set_body(400, "Bad Request",
                          "{\"code\":400,\"message\":\"missing bearer token\"}",
                          "application/json");
        return http_core::MEMORY_REQUEST;
    }

    const bool logout_all = request.value("scope") == "all" ||
                            service_auth::is_truthy_value(request.value("all_sessions"));
    if (!service_auth::logout(context.mysql, context.current_user, token, logout_all))
    {
        response.set_json_error(500, "Internal Error", "failed to invalidate session");
        return http_core::MEMORY_REQUEST;
    }

    if (!context.current_user.empty())
    {
        write_operation_log(context.mysql,
                            context.current_user,
                            logout_all ? "logout_all" : "logout",
                            "user", 0,
                            logout_all ? "all sessions revoked" : "logout success");
    }

    const string body = logout_all
                            ? "{\"code\":0,\"message\":\"logout success\",\"scope\":\"all\"}"
                            : "{\"code\":0,\"message\":\"logout success\",\"scope\":\"current\"}";
    response.set_body(200, "OK", body, "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode AuthController::ping(const http_core::HttpRequest &request,
                                         http_core::RequestContext &context,
                                         http_core::HttpResponse &response)
{
    (void)request;
    string body = string("{\"code\":0,\"message\":\"pong\",\"user\":\"") +
                  http_core::json_escape(context.current_user) + "\"}";
    response.set_body(200, "OK", body, "application/json");
    return http_core::MEMORY_REQUEST;
}
}
