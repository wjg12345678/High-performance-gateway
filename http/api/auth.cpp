#include "../core/connection.h"
#include "../controllers/auth_controller.h"

HttpConnection::HTTP_CODE HttpConnection::route_api_login()
{
    return http_controllers::AuthController::login(*this, true);
}

HttpConnection::HTTP_CODE HttpConnection::route_api_register()
{
    return http_controllers::AuthController::register_user(*this, true);
}

HttpConnection::HTTP_CODE HttpConnection::route_api_private_logout()
{
    return http_controllers::AuthController::logout(*this);
}

HttpConnection::HTTP_CODE HttpConnection::handle_auth_request(bool is_register, bool api_mode)
{
    return is_register ? http_controllers::AuthController::register_user(*this, api_mode)
                       : http_controllers::AuthController::login(*this, api_mode);
}

HttpConnection::HTTP_CODE HttpConnection::handle_logout_request()
{
    return http_controllers::AuthController::logout(*this);
}
