#pragma once

#include <string>

struct AppConfig
{
    std::string jwtSecret;
    std::string jwtIssuer;
    int accessTokenTtlSeconds{900};
    int refreshTokenTtlSeconds{604800};
    std::string corsOrigin{"http://localhost:5173"};
    bool devMode{false};

    static AppConfig fromDrogon();
};
