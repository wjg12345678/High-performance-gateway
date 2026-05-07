#ifndef HTTP_AUTH_STATE_H
#define HTTP_AUTH_STATE_H

#include <ctime>
#include <map>
#include <string>

#include "../../lock/locker.h"

struct AuthSessionCacheEntry
{
    std::string username;
    time_t expires_at;

    AuthSessionCacheEntry() : expires_at(0) {}
    AuthSessionCacheEntry(const std::string &value, time_t expiry)
        : username(value), expires_at(expiry) {}
};

locker &auth_cache_lock();
std::map<std::string, std::string> &auth_user_cache();
std::map<std::string, AuthSessionCacheEntry> &auth_session_cache();

#endif
