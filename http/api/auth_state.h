#ifndef HTTP_AUTH_STATE_H
#define HTTP_AUTH_STATE_H

#include <map>
#include <string>

#include "../../lock/locker.h"

locker &auth_cache_lock();
std::map<std::string, std::string> &auth_user_cache();
std::map<std::string, std::string> &auth_session_cache();

#endif
