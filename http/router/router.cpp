#include "router.h"

#include "../controllers/auth_controller.h"
#include "../controllers/file_controller.h"
#include "../controllers/operation_controller.h"
#include "../../infra/log/log.h"
#include "../../repo/mysql/operation_repository.h"
#include "../../service/auth/auth_service.h"

#include <cstring>
#include <strings.h>

using namespace std;

namespace
{
bool starts_with_ignore_case(const string &text, const char *prefix)
{
    return http_core::starts_with_ignore_case(text, prefix);
}

bool equals_ignore_case(const string &left, const char *right)
{
    return http_core::equals_ignore_case(left, right);
}

http_core::HttpCode route_healthz(http_core::HttpRequest &request,
                                  http_core::RequestContext &context,
                                  http_core::HttpResponse &response)
{
    (void)request;
    (void)context;
    response.set_body(200, "OK", "{\"code\":0,\"status\":\"ok\"}", "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode route_echo(http_core::HttpRequest &request,
                               http_core::RequestContext &context,
                               http_core::HttpResponse &response)
{
    (void)context;
    string body = "{\"code\":0,\"content_type\":\"" + http_core::json_escape(request.header_value("content-type")) +
                  "\",\"body\":\"" + http_core::json_escape(request.body) + "\"}";
    response.set_body(200, "OK", body, "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode middleware_request_log(const http_core::HttpRequest &request,
                                           const http_core::RequestContext &context)
{
    if (context.close_log == 0 && !request.should_skip_request_log())
    {
        Log::get_instance()->write_log(1, "request %s %s content_type=%s content_length=%ld",
                                       request.method_name(), request.path.c_str(),
                                       request.header_value("content-type").empty()
                                           ? "-"
                                           : request.header_value("content-type").c_str(),
                                       request.content_length);
    }
    return http_core::NO_REQUEST;
}

http_core::HttpCode middleware_auth(const http_core::HttpRequest &request,
                                    http_core::RequestContext &context,
                                    http_core::HttpResponse &response)
{
    if (!request.requires_auth())
    {
        return http_core::NO_REQUEST;
    }

    const string token = request.bearer_token();
    if (!token.empty() && service_auth::lookup_session(context.mysql, token, context.current_user))
    {
        return http_core::NO_REQUEST;
    }

    response.set_body(401, "Unauthorized",
                      "{\"code\":401,\"message\":\"unauthorized\"}",
                      "application/json");
    if (context.mysql != nullptr)
    {
        repo_mysql::insert_operation_log(context.mysql, "anonymous", "auth_failed",
                                         "request", 0, request.path);
    }
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode run_before_middlewares(const http_core::HttpRequest &request,
                                           http_core::RequestContext &context,
                                           http_core::HttpResponse &response)
{
    http_core::HttpCode code = middleware_request_log(request, context);
    if (code != http_core::NO_REQUEST)
    {
        return code;
    }
    return middleware_auth(request, context, response);
}

http_core::HttpCode api_error_response(http_core::HttpCode code, http_core::HttpResponse &response)
{
    int status = 500;
    const char *title = "Internal Error";
    const char *message = "internal server error";
    if (code == http_core::BAD_REQUEST)
    {
        status = 400;
        title = "Bad Request";
        message = "bad request";
    }
    else if (code == http_core::NO_RESOURCE)
    {
        status = 404;
        title = "Not Found";
        message = "resource not found";
    }
    else if (code == http_core::FORBIDDEN_REQUEST)
    {
        status = 403;
        title = "Forbidden";
        message = "forbidden";
    }
    else if (code == http_core::NOT_IMPLEMENTED)
    {
        status = 501;
        title = "Not Implemented";
        message = "not implemented";
    }
    else if (code == http_core::PAYLOAD_TOO_LARGE)
    {
        status = 413;
        title = "Payload Too Large";
        message = "payload too large";
    }

    char body[128];
    snprintf(body, sizeof(body), "{\"code\":%d,\"message\":\"%s\"}", status, message);
    response.set_body(status, title, body, "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode run_after_middlewares(const http_core::HttpRequest &request,
                                          http_core::HttpCode code,
                                          http_core::HttpResponse &response)
{
    if (!request.is_api_request())
    {
        return code;
    }

    if (code == http_core::MEMORY_REQUEST || code == http_core::FILE_REQUEST ||
        code == http_core::OPTIONS_REQUEST)
    {
        return code;
    }

    return api_error_response(code, response);
}

http_core::HttpCode handle_api_drive_folder_item(http_core::HttpRequest &request,
                                                 http_core::RequestContext &context,
                                                 http_core::HttpResponse &response,
                                                 const char *path)
{
    return request.method == http_core::DELETE
               ? http_controllers::FileController::drive_folder_delete(request, context, response, path)
               : http_core::NOT_IMPLEMENTED;
}

http_core::HttpCode handle_api_drive_file_item(http_core::HttpRequest &request,
                                               http_core::RequestContext &context,
                                               http_core::HttpResponse &response,
                                               const char *path)
{
    if (request.method == http_core::GET && strstr(path, "/download") != nullptr)
    {
        return http_controllers::FileController::private_file_download(request, context, response, path);
    }
    if (request.method == http_core::POST && strstr(path, "/share") != nullptr)
    {
        return http_controllers::FileController::share_create(request, context, response, path);
    }
    if (request.method == http_core::POST && strstr(path, "/visibility") != nullptr)
    {
        return http_controllers::FileController::update_visibility(request, context, response, path);
    }
    if (request.method == http_core::POST && strstr(path, "/restore") != nullptr)
    {
        return http_controllers::FileController::restore(request, context, response, path);
    }
    if (request.method == http_core::DELETE && strstr(path, "/permanent") != nullptr)
    {
        return http_controllers::FileController::remove_permanently(request, context, response, path);
    }
    if (request.method == http_core::DELETE)
    {
        return http_controllers::FileController::remove(request, context, response, path);
    }
    return http_core::NOT_IMPLEMENTED;
}

http_core::HttpCode handle_api_public_file_item(http_core::HttpRequest &request,
                                                http_core::RequestContext &context,
                                                http_core::HttpResponse &response,
                                                const char *path)
{
    if (request.method == http_core::GET && strstr(path, "/download") != nullptr)
    {
        return http_controllers::FileController::public_file_download(request, context, response, path);
    }
    if (request.method == http_core::GET)
    {
        return http_controllers::FileController::public_file_detail(request, context, response, path);
    }
    return http_core::NOT_IMPLEMENTED;
}

http_core::HttpCode handle_api_share_item(http_core::HttpRequest &request,
                                          http_core::RequestContext &context,
                                          http_core::HttpResponse &response,
                                          const char *path)
{
    if (request.method == http_core::GET && strstr(path, "/download") != nullptr)
    {
        return http_controllers::FileController::share_download(request, context, response, path);
    }
    if (request.method == http_core::GET)
    {
        return http_controllers::FileController::share_detail(request, context, response, path);
    }
    return http_core::NOT_IMPLEMENTED;
}

http_core::HttpCode handle_api_drive_request(http_core::HttpRequest &request,
                                             http_core::RequestContext &context,
                                             http_core::HttpResponse &response)
{
    if (equals_ignore_case(request.path, "/api/drive/items"))
    {
        return request.method == http_core::GET
                   ? http_controllers::FileController::drive_item_list(request, context, response)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/drive/trash"))
    {
        if (request.method == http_core::GET)
        {
            return http_controllers::FileController::trash_item_list(request, context, response);
        }
        return request.method == http_core::DELETE
                   ? http_controllers::FileController::empty_trash(request, context, response)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/drive/folders"))
    {
        return request.method == http_core::POST
                   ? http_controllers::FileController::drive_folder_create(request, context, response)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/drive/files/upload"))
    {
        return request.method == http_core::POST
                   ? http_controllers::FileController::drive_file_upload(request, context, response)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/drive/files/preflight"))
    {
        return request.method == http_core::POST
                   ? http_controllers::FileController::upload_preflight(request, context, response)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (starts_with_ignore_case(request.path, "/api/drive/folders/"))
    {
        return handle_api_drive_folder_item(request, context, response,
                                            request.path.c_str() + strlen("/api/drive/folders/"));
    }
    if (starts_with_ignore_case(request.path, "/api/drive/files/"))
    {
        return handle_api_drive_file_item(request, context, response,
                                          request.path.c_str() + strlen("/api/drive/files/"));
    }
    return http_core::NOT_IMPLEMENTED;
}

http_core::HttpCode handle_api_request(http_core::HttpRequest &request,
                                       http_core::RequestContext &context,
                                       http_core::HttpResponse &response)
{
    if (equals_ignore_case(request.path, "/healthz"))
    {
        return (request.method == http_core::GET || request.method == http_core::HEAD)
                   ? route_healthz(request, context, response)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/login"))
    {
        return request.method == http_core::POST
                   ? http_controllers::AuthController::login(request, context, response, true)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/register"))
    {
        return request.method == http_core::POST
                   ? http_controllers::AuthController::register_user(request, context, response, true)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/echo"))
    {
        return request.method == http_core::POST ? route_echo(request, context, response)
                                                 : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/private/ping"))
    {
        return request.method == http_core::GET
                   ? http_controllers::AuthController::ping(request, context, response)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/private/logout"))
    {
        return request.method == http_core::POST
                   ? http_controllers::AuthController::logout(request, context, response)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/files/public"))
    {
        return request.method == http_core::GET
                   ? http_controllers::FileController::public_file_list(request, context, response)
                   : http_core::NOT_IMPLEMENTED;
    }
    if (equals_ignore_case(request.path, "/api/private/operations"))
    {
        if (request.method == http_core::GET)
        {
            return http_controllers::OperationController::list(request, context, response);
        }
        return request.method == http_core::DELETE
                   ? http_controllers::OperationController::clear(request, context, response)
                   : http_core::NOT_IMPLEMENTED;
    }

    if (starts_with_ignore_case(request.path, "/api/private/operations/"))
    {
        return request.method == http_core::DELETE
                   ? http_controllers::OperationController::remove(
                         request, context, response,
                         request.path.c_str() + strlen("/api/private/operations/"))
                   : http_core::NOT_IMPLEMENTED;
    }
    if (starts_with_ignore_case(request.path, "/api/drive/"))
    {
        return handle_api_drive_request(request, context, response);
    }
    if (starts_with_ignore_case(request.path, "/api/files/public/"))
    {
        return handle_api_public_file_item(request, context, response,
                                           request.path.c_str() + strlen("/api/files/public/"));
    }
    if (starts_with_ignore_case(request.path, "/api/share/"))
    {
        return handle_api_share_item(request, context, response,
                                     request.path.c_str() + strlen("/api/share/"));
    }

    response.set_body(404, "Not Found", "{\"code\":404,\"message\":\"api not found\"}", "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode route_request(http_core::HttpRequest &request,
                                  http_core::RequestContext &context,
                                  http_core::HttpResponse &response)
{
    if (equals_ignore_case(request.path, "/healthz") &&
        (request.method == http_core::GET || request.method == http_core::HEAD))
    {
        return route_healthz(request, context, response);
    }

    if (starts_with_ignore_case(request.path, "/api/"))
    {
        return handle_api_request(request, context, response);
    }

    response.set_body(404, "Not Found", "{\"code\":404,\"message\":\"not found\"}", "application/json");
    return http_core::MEMORY_REQUEST;
}
}

namespace http_router
{
http_core::HttpCode handle_request(http_core::HttpRequest &request,
                                   http_core::RequestContext &context,
                                   http_core::HttpResponse &response)
{
    if (request.method == http_core::OPTIONS)
    {
        response.set_options();
        return http_core::OPTIONS_REQUEST;
    }

    http_core::HttpCode middleware_code = run_before_middlewares(request, context, response);
    if (middleware_code != http_core::NO_REQUEST)
    {
        return middleware_code;
    }

    return run_after_middlewares(request, route_request(request, context, response), response);
}
}
