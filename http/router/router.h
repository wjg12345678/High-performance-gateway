#ifndef ATLAS_HTTP_ROUTER_ROUTER_H
#define ATLAS_HTTP_ROUTER_ROUTER_H

#include "../core/http_message.h"

namespace http_router
{
http_core::HttpCode handle_request(http_core::HttpRequest &request,
                                   http_core::RequestContext &context,
                                   http_core::HttpResponse &response);
}

#endif
