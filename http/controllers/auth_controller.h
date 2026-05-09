#ifndef HTTP_CONTROLLERS_AUTH_CONTROLLER_H
#define HTTP_CONTROLLERS_AUTH_CONTROLLER_H

#include "../core/connection.h"

namespace http_controllers
{
class AuthController
{
public:
    static HttpConnection::HTTP_CODE login(HttpConnection &conn, bool api_mode);
    static HttpConnection::HTTP_CODE register_user(HttpConnection &conn, bool api_mode);
    static HttpConnection::HTTP_CODE logout(HttpConnection &conn);
    static HttpConnection::HTTP_CODE ping(HttpConnection &conn);
};
}

#endif
