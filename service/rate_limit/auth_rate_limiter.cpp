#include "auth_rate_limiter.h"

#ifndef ATLAS_WITH_REDIS_LIMITER
#define ATLAS_WITH_REDIS_LIMITER 0
#endif

#if ATLAS_WITH_REDIS_LIMITER
#include "sliding_window_limiter.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>

namespace
{
using service_rate_limit::AuthBucketSettings;
using service_rate_limit::AuthRateLimitAction;
using service_rate_limit::AuthRateLimitDecision;
using service_rate_limit::AuthRateLimitSettings;

const int kDefaultLoginIpMaxTokens = 60;
const double kDefaultLoginIpRefillRate = 1.0;
const int kDefaultLoginUserMaxTokens = 10;
const double kDefaultLoginUserRefillRate = 0.166667;
const int kDefaultRegisterIpMaxTokens = 30;
const double kDefaultRegisterIpRefillRate = 0.05;

AuthBucketSettings normalize_bucket(AuthBucketSettings bucket, int fallback_tokens, double fallback_refill)
{
    if (bucket.max_tokens <= 0)
    {
        bucket.max_tokens = fallback_tokens;
    }
    if (bucket.refill_rate <= 0.0)
    {
        bucket.refill_rate = fallback_refill;
    }
    return bucket;
}

std::string stable_hash_hex(const std::string &value)
{
    unsigned long long hash = 1469598103934665603ULL;
    for (size_t i = 0; i < value.size(); ++i)
    {
        hash ^= static_cast<unsigned char>(value[i]);
        hash *= 1099511628211ULL;
    }

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%016llx", hash);
    return buffer;
}

std::string key_component(const std::string &value)
{
    if (value.empty())
    {
        return "unknown";
    }

    static const char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (std::isalnum(ch) || ch == '.' || ch == ':' || ch == '_' || ch == '-')
        {
            encoded.push_back(static_cast<char>(ch));
        }
        else
        {
            encoded.push_back('%');
            encoded.push_back(kHex[(ch >> 4) & 0x0F]);
            encoded.push_back(kHex[ch & 0x0F]);
        }
    }

    if (encoded.size() > 160)
    {
        return "h" + stable_hash_hex(value);
    }
    return encoded;
}

class AuthRateLimiter
{
public:
    explicit AuthRateLimiter(AuthRateLimitSettings settings)
        : m_enabled(settings.enabled),
          m_login_ip_settings(normalize_bucket(settings.login_ip, kDefaultLoginIpMaxTokens, kDefaultLoginIpRefillRate)),
          m_login_user_settings(normalize_bucket(settings.login_user, kDefaultLoginUserMaxTokens, kDefaultLoginUserRefillRate)),
          m_register_ip_settings(normalize_bucket(settings.register_ip, kDefaultRegisterIpMaxTokens, kDefaultRegisterIpRefillRate))
    {
        if (!m_enabled)
        {
            return;
        }

#if ATLAS_WITH_REDIS_LIMITER
        rrl::RedisConfig redis_config;
        redis_config.host = settings.redis.host.empty() ? "127.0.0.1" : settings.redis.host;
        redis_config.port = settings.redis.port > 0 ? settings.redis.port : 6379;
        redis_config.password = settings.redis.password;
        redis_config.db = std::max(0, settings.redis.db);
        redis_config.pool_size = std::max(1, settings.redis.pool_size);
        redis_config.connect_timeout_ms = std::max(1, settings.redis.connect_timeout_ms);
        redis_config.socket_timeout_ms = std::max(1, settings.redis.socket_timeout_ms);
        redis_config.max_retries = std::max(0, settings.redis.max_retries);

        m_redis_pool = std::make_shared<rrl::RedisPool>(redis_config);
        const rrl::FallbackMode fallback = parse_fallback_mode(settings.fallback_mode);
        m_login_ip_remote = make_remote_limiter(m_redis_pool, m_login_ip_settings, "atlas:auth:login:ip:", fallback);
        m_login_user_remote = make_remote_limiter(m_redis_pool, m_login_user_settings, "atlas:auth:login:user:", fallback);
        m_register_ip_remote = make_remote_limiter(m_redis_pool, m_register_ip_settings, "atlas:auth:register:ip:", fallback);
#else
        (void)settings;
#endif
    }

    AuthRateLimitDecision check(AuthRateLimitAction action,
                                const std::string &username,
                                const std::string &client_ip)
    {
        if (!m_enabled)
        {
            return AuthRateLimitDecision();
        }

        if (action == AuthRateLimitAction::Login)
        {
            AuthRateLimitDecision ip_decision = allow_login_ip(key_component(client_ip));
            if (!ip_decision.allowed)
            {
                return ip_decision;
            }
            return allow_login_user(key_component(username));
        }

        return allow_register_ip(key_component(client_ip));
    }

private:
#if ATLAS_WITH_REDIS_LIMITER
    static rrl::FallbackMode parse_fallback_mode(const std::string &value)
    {
        if (value == "fail_open" || value == "open")
        {
            return rrl::FallbackMode::FailOpen;
        }
        if (value == "fail_closed" || value == "closed")
        {
            return rrl::FallbackMode::FailClosed;
        }
        return rrl::FallbackMode::LocalTokenBucket;
    }

    static std::shared_ptr<rrl::ResilientTokenBucketLimiter> make_remote_limiter(
        const std::shared_ptr<rrl::RedisPool> &pool,
        const AuthBucketSettings &settings,
        const std::string &prefix,
        rrl::FallbackMode fallback)
    {
        std::shared_ptr<rrl::TokenBucketLimiter> remote(
            new rrl::TokenBucketLimiter(pool, settings.max_tokens, settings.refill_rate, prefix));
        return std::shared_ptr<rrl::ResilientTokenBucketLimiter>(
            new rrl::ResilientTokenBucketLimiter(remote, fallback, settings.max_tokens, settings.refill_rate));
    }

    static std::string backend_name(rrl::BackendStatus status)
    {
        switch (status)
        {
        case rrl::BackendStatus::Healthy: return "redis";
        case rrl::BackendStatus::Fallback: return "fallback";
        case rrl::BackendStatus::Unavailable:
        default: return "unavailable";
        }
    }

    static AuthRateLimitDecision from_remote_result(const rrl::RateLimitResult &result, const std::string &scope)
    {
        AuthRateLimitDecision decision;
        decision.allowed = result.allowed;
        decision.retry_after_ms = result.retry_after_ms;
        decision.scope = scope;
        decision.backend = backend_name(result.backend_status);
        return decision;
    }
#endif

    AuthRateLimitDecision allow_login_ip(const std::string &key)
    {
#if ATLAS_WITH_REDIS_LIMITER
        if (m_login_ip_remote)
        {
            return from_remote_result(m_login_ip_remote->allow(key), "login_ip");
        }
#else
        (void)key;
#endif
        return AuthRateLimitDecision();
    }

    AuthRateLimitDecision allow_login_user(const std::string &key)
    {
#if ATLAS_WITH_REDIS_LIMITER
        if (m_login_user_remote)
        {
            return from_remote_result(m_login_user_remote->allow(key), "login_user");
        }
#else
        (void)key;
#endif
        return AuthRateLimitDecision();
    }

    AuthRateLimitDecision allow_register_ip(const std::string &key)
    {
#if ATLAS_WITH_REDIS_LIMITER
        if (m_register_ip_remote)
        {
            return from_remote_result(m_register_ip_remote->allow(key), "register_ip");
        }
#else
        (void)key;
#endif
        return AuthRateLimitDecision();
    }

    bool m_enabled;
    AuthBucketSettings m_login_ip_settings;
    AuthBucketSettings m_login_user_settings;
    AuthBucketSettings m_register_ip_settings;
#if ATLAS_WITH_REDIS_LIMITER
    std::shared_ptr<rrl::RedisPool> m_redis_pool;
    std::shared_ptr<rrl::ResilientTokenBucketLimiter> m_login_ip_remote;
    std::shared_ptr<rrl::ResilientTokenBucketLimiter> m_login_user_remote;
    std::shared_ptr<rrl::ResilientTokenBucketLimiter> m_register_ip_remote;
#endif
};

std::mutex g_limiter_mutex;
std::shared_ptr<AuthRateLimiter> g_limiter;
}

namespace service_rate_limit
{
RedisSettings::RedisSettings()
    : host("127.0.0.1"),
      port(6379),
      db(0),
      pool_size(4),
      connect_timeout_ms(80),
      socket_timeout_ms(80),
      max_retries(0)
{
}

AuthBucketSettings::AuthBucketSettings() : max_tokens(1), refill_rate(1.0) {}

AuthBucketSettings::AuthBucketSettings(int max_tokens_value, double refill_rate_value)
    : max_tokens(max_tokens_value), refill_rate(refill_rate_value)
{
}

AuthRateLimitSettings::AuthRateLimitSettings()
    : enabled(false),
      login_ip(kDefaultLoginIpMaxTokens, kDefaultLoginIpRefillRate),
      login_user(kDefaultLoginUserMaxTokens, kDefaultLoginUserRefillRate),
      register_ip(kDefaultRegisterIpMaxTokens, kDefaultRegisterIpRefillRate),
      fallback_mode("local")
{
}

AuthRateLimitDecision::AuthRateLimitDecision()
    : allowed(true), retry_after_ms(0), scope("none"), backend("disabled")
{
}

void configure_auth_rate_limiter(const AuthRateLimitSettings &settings)
{
    std::lock_guard<std::mutex> lock(g_limiter_mutex);
    g_limiter = std::shared_ptr<AuthRateLimiter>(new AuthRateLimiter(settings));
}

AuthRateLimitDecision check_auth_rate_limit(AuthRateLimitAction action,
                                            const std::string &username,
                                            const std::string &client_ip)
{
    std::shared_ptr<AuthRateLimiter> limiter;
    {
        std::lock_guard<std::mutex> lock(g_limiter_mutex);
        limiter = g_limiter;
    }

    if (!limiter)
    {
        return AuthRateLimitDecision();
    }
    return limiter->check(action, username, client_ip);
}

} // namespace service_rate_limit
