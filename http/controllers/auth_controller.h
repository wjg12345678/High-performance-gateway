#ifndef HTTP_CONTROLLERS_AUTH_CONTROLLER_H
#define HTTP_CONTROLLERS_AUTH_CONTROLLER_H

#include "../core/http_message.h"

namespace http_controllers
{
class AuthController
{
public:
    static http_core::HttpCode login(const http_core::HttpRequest &request,
                                     http_core::RequestContext &context,
                                     http_core::HttpResponse &response,
                                     bool api_mode);
    static http_core::HttpCode register_user(const http_core::HttpRequest &request,
                                             http_core::RequestContext &context,
                                             http_core::HttpResponse &response,
                                             bool api_mode);
    static http_core::HttpCode logout(const http_core::HttpRequest &request,
                                      http_core::RequestContext &context,
                                      http_core::HttpResponse &response);
    static http_core::HttpCode ping(const http_core::HttpRequest &request,
                                    http_core::RequestContext &context,
                                    http_core::HttpResponse &response);
};
}

#endif
