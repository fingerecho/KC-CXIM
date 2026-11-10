#include "config/AppConfig.h"

#include <drogon/drogon.h>
#include <json/json.h>

#include <stdexcept>

AppConfig AppConfig::fromDrogon()
{
    const auto &custom = drogon::app().getCustomConfig();
    const auto &admin = custom["admin"];

    if (!admin.isObject())
    {
        throw std::runtime_error(
            "config custom_config.admin is required");
    }

    AppConfig config;
    config.jwtSecret = admin["jwt_secret"].asString();
    config.jwtIssuer = admin["jwt_issuer"].asString();
    config.accessTokenTtlSeconds =
        admin["access_token_ttl_seconds"].asInt();
    config.refreshTokenTtlSeconds =
        admin["refresh_token_ttl_seconds"].asInt();
    config.corsOrigin = admin["cors_origin"].asString();
    config.devMode = admin["dev_mode"].asBool();

    if (config.jwtSecret.size() < 16)
    {
        throw std::runtime_error(
            "config custom_config.admin.jwt_secret must be at least 16 characters");
    }
    if (config.jwtIssuer.empty())
    {
        throw std::runtime_error(
            "config custom_config.admin.jwt_issuer is required");
    }
    if (config.accessTokenTtlSeconds <= 0 ||
        config.refreshTokenTtlSeconds <= 0)
    {
        throw std::runtime_error(
            "config token ttl values must be positive");
    }
    if (config.corsOrigin.empty())
    {
        config.corsOrigin = "http://localhost:5173";
    }

    return config;
}
