#include "auth_state.h"

namespace
{
locker g_auth_cache_lock;
std::map<std::string, std::string> g_auth_users;
std::map<std::string, std::string> g_auth_sessions;
}

locker &auth_cache_lock()
{
    return g_auth_cache_lock;
}

std::map<std::string, std::string> &auth_user_cache()
{
    return g_auth_users;
}

std::map<std::string, std::string> &auth_session_cache()
{
    return g_auth_sessions;
}
