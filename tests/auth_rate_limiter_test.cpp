#include "../service/rate_limit/auth_rate_limiter.h"

#include <cassert>

#ifndef ATLAS_WITH_REDIS_LIMITER
#define ATLAS_WITH_REDIS_LIMITER 0
#endif

static void login_user_bucket_denies_after_capacity()
{
    service_rate_limit::AuthRateLimitSettings settings;
    settings.enabled = true;
    settings.login_ip = service_rate_limit::AuthBucketSettings(100, 100.0);
    settings.login_user = service_rate_limit::AuthBucketSettings(2, 0.01);
    settings.register_ip = service_rate_limit::AuthBucketSettings(100, 100.0);
    service_rate_limit::configure_auth_rate_limiter(settings);

    const std::string username = "rate-limit-user";
    const std::string client_ip = "203.0.113.10";
    assert(service_rate_limit::check_auth_rate_limit(service_rate_limit::AuthRateLimitAction::Login,
                                                     username, client_ip)
               .allowed);
    assert(service_rate_limit::check_auth_rate_limit(service_rate_limit::AuthRateLimitAction::Login,
                                                     username, client_ip)
               .allowed);

    service_rate_limit::AuthRateLimitDecision denied =
        service_rate_limit::check_auth_rate_limit(service_rate_limit::AuthRateLimitAction::Login,
                                                  username, client_ip);
    assert(!denied.allowed);
    assert(denied.scope == "login_user");
    assert(denied.retry_after_ms > 0);
}

static void register_ip_bucket_denies_after_capacity()
{
    service_rate_limit::AuthRateLimitSettings settings;
    settings.enabled = true;
    settings.login_ip = service_rate_limit::AuthBucketSettings(100, 100.0);
    settings.login_user = service_rate_limit::AuthBucketSettings(100, 100.0);
    settings.register_ip = service_rate_limit::AuthBucketSettings(1, 0.01);
    service_rate_limit::configure_auth_rate_limiter(settings);

    const std::string client_ip = "203.0.113.20";
    assert(service_rate_limit::check_auth_rate_limit(service_rate_limit::AuthRateLimitAction::Register,
                                                     "first-user", client_ip)
               .allowed);

    service_rate_limit::AuthRateLimitDecision denied =
        service_rate_limit::check_auth_rate_limit(service_rate_limit::AuthRateLimitAction::Register,
                                                  "second-user", client_ip);
    assert(!denied.allowed);
    assert(denied.scope == "register_ip");
    assert(denied.retry_after_ms > 0);
}

static void auth_rate_limiter_allows_when_component_unavailable()
{
    service_rate_limit::AuthRateLimitSettings settings;
    settings.enabled = true;
    settings.login_ip = service_rate_limit::AuthBucketSettings(1, 0.01);
    settings.login_user = service_rate_limit::AuthBucketSettings(1, 0.01);
    settings.register_ip = service_rate_limit::AuthBucketSettings(1, 0.01);
    service_rate_limit::configure_auth_rate_limiter(settings);

    assert(service_rate_limit::check_auth_rate_limit(service_rate_limit::AuthRateLimitAction::Login,
                                                     "rate-limit-user", "203.0.113.30")
               .allowed);
    assert(service_rate_limit::check_auth_rate_limit(service_rate_limit::AuthRateLimitAction::Login,
                                                     "rate-limit-user", "203.0.113.30")
               .allowed);
}

int main()
{
#if ATLAS_WITH_REDIS_LIMITER
    login_user_bucket_denies_after_capacity();
    register_ip_bucket_denies_after_capacity();
#else
    auth_rate_limiter_allows_when_component_unavailable();
#endif
    return 0;
}
