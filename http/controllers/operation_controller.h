#ifndef HTTP_CONTROLLERS_OPERATION_CONTROLLER_H
#define HTTP_CONTROLLERS_OPERATION_CONTROLLER_H

#include "../core/connection.h"

namespace http_controllers
{
class OperationController
{
public:
    static HttpConnection::HTTP_CODE list(HttpConnection &conn);
    static HttpConnection::HTTP_CODE remove(HttpConnection &conn, const char *path);
};
}

#endif
