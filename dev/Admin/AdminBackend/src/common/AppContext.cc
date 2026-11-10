#include "common/AppContext.h"

#include "common/JwtCodec.h"
#include "config/AppConfig.h"
#include "repositories/AdminRepository.h"
#include "repositories/AuditRepository.h"
#include "repositories/SessionRepository.h"
#include "repositories/StatsRepository.h"
#include "repositories/UserRepository.h"
#include "services/AuditService.h"
#include "services/AuthService.h"
#include "services/StatsService.h"
#include "services/UserService.h"

AppContext &AppContext::instance()
{
    static AppContext context;
    return context;
}

void AppContext::init(const AppConfig &config,
                      const drogon::orm::DbClientPtr &db)
{
    if (authService_)
    {
        return;
    }

    auto adminRepo = std::make_shared<AdminRepository>(db);
    auto sessionRepo = std::make_shared<SessionRepository>(db);
    auto userRepo = std::make_shared<UserRepository>(db);
    auto statsRepo = std::make_shared<StatsRepository>(db);
    auto auditRepo = std::make_shared<AuditRepository>(db);
    auto jwt = std::make_shared<JwtCodec>(config.jwtSecret, config.jwtIssuer);

    auditService_ = std::make_shared<AuditService>(auditRepo);
    authService_ = std::make_shared<AuthService>(
        adminRepo,
        sessionRepo,
        jwt,
        config.accessTokenTtlSeconds,
        config.refreshTokenTtlSeconds);
    userService_ =
        std::make_shared<UserService>(userRepo, auditService_);
    statsService_ = std::make_shared<StatsService>(statsRepo);
}
