#pragma once

#include <memory>
#include <drogon/orm/DbClient.h>

class AppConfig;
class AuthService;
class UserService;
class StatsService;
class AuditService;

class AppContext
{
  public:
    static AppContext &instance();

    void init(const AppConfig &config,
              const drogon::orm::DbClientPtr &db);

    std::shared_ptr<AuthService> authService() const { return authService_; }
    std::shared_ptr<UserService> userService() const { return userService_; }
    std::shared_ptr<StatsService> statsService() const { return statsService_; }
    std::shared_ptr<AuditService> auditService() const { return auditService_; }

  private:
    AppContext() = default;

    std::shared_ptr<AuthService> authService_;
    std::shared_ptr<UserService> userService_;
    std::shared_ptr<StatsService> statsService_;
    std::shared_ptr<AuditService> auditService_;
};
