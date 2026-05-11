#ifndef HTTP_CONTROLLERS_OPERATION_CONTROLLER_H
#define HTTP_CONTROLLERS_OPERATION_CONTROLLER_H

#include "../core/http_message.h"

namespace http_controllers
{
class OperationController
{
public:
    static http_core::HttpCode list(const http_core::HttpRequest &request,
                                    http_core::RequestContext &context,
                                    http_core::HttpResponse &response);
    static http_core::HttpCode remove(const http_core::HttpRequest &request,
                                      http_core::RequestContext &context,
                                      http_core::HttpResponse &response,
                                      const char *path);
    static http_core::HttpCode clear(const http_core::HttpRequest &request,
                                     http_core::RequestContext &context,
                                     http_core::HttpResponse &response);
};
}

#endif
