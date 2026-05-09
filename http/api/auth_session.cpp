#include "../core/connection.h"
#include "../../service/auth/auth_service.h"

#include <cstring>
#include <strings.h>

namespace
{
bool starts_with_ignore_case_local(const char *text, const char *prefix)
{
    return strncasecmp(text, prefix, strlen(prefix)) == 0;
}
}

std::string HttpConnection::extract_bearer_token() const
{
    if (!starts_with_ignore_case_local(m_authorization.c_str(), "Bearer "))
    {
        return "";
    }
    return trim_copy(m_authorization.substr(7));
}

HttpConnection::HTTP_CODE HttpConnection::middleware_auth()
{
    if (!requires_auth())
    {
        return NO_REQUEST;
    }

    const std::string token = extract_bearer_token();
    if (!token.empty() && service_auth::lookup_session(mysql, token, m_current_user))
    {
        return NO_REQUEST;
    }

    set_memory_response(401, "Unauthorized",
                        "{\"code\":401,\"message\":\"unauthorized\"}",
                        "application/json");
    if (mysql != nullptr)
    {
        write_operation_log("anonymous", "auth_failed", "request", 0, m_url ? m_url : "");
    }
    return MEMORY_REQUEST;
}
