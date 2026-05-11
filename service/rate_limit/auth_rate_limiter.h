#ifndef ATLAS_SERVICE_RATE_LIMIT_AUTH_RATE_LIMITER_H
#define ATLAS_SERVICE_RATE_LIMIT_AUTH_RATE_LIMITER_H

#include <string>

namespace service_rate_limit
{
struct RedisSettings
{
    std::string host;
    int port;
    std::string password;
    int db;
    int pool_size;
    int connect_timeout_ms;
    int socket_timeout_ms;
    int max_retries;

    RedisSettings();
};

struct AuthBucketSettings
{
    int max_tokens;
    double refill_rate;

    AuthBucketSettings();
    AuthBucketSettings(int max_tokens_value, double refill_rate_value);
};

struct AuthRateLimitSettings
{
    bool enabled;
    RedisSettings redis;
    AuthBucketSettings login_ip;
    AuthBucketSettings login_user;
    AuthBucketSettings register_ip;
    std::string fallback_mode;

    AuthRateLimitSettings();
};

enum class AuthRateLimitAction
{
    Login,
    Register
};

struct AuthRateLimitDecision
{
    bool allowed;
    int retry_after_ms;
    std::string scope;
    std::string backend;

    AuthRateLimitDecision();
};

void configure_auth_rate_limiter(const AuthRateLimitSettings &settings);
AuthRateLimitDecision check_auth_rate_limit(AuthRateLimitAction action,
                                            const std::string &username,
                                            const std::string &client_ip);

} // namespace service_rate_limit

#endif
