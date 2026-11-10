#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "common/Result.h"
#include "models/UserSummary.h"

class UserRepository;
class AuditService;

class UserService : public std::enable_shared_from_this<UserService>
{
  public:
    UserService(std::shared_ptr<UserRepository> userRepo,
                std::shared_ptr<AuditService> auditService);

    void listUsers(int page,
                   int pageSize,
                   std::string keyword,
                   std::string status,
                   std::function<void(Result<UserPage>)> cb);

    void getUser(std::uint64_t id,
                 std::function<void(Result<UserSummary>)> cb);

    void updateStatus(std::uint64_t adminId,
                      std::string adminName,
                      std::uint64_t userId,
                      std::string status,
                      std::string ip,
                      std::string userAgent,
                      std::string requestId,
                      std::function<void(Result<UserStatusResult>)> cb);

    void resetPassword(std::uint64_t adminId,
                       std::string adminName,
                       std::uint64_t userId,
                       std::string ip,
                       std::string userAgent,
                       std::string requestId,
                       std::function<void(Result<bool>)> cb);

    void kick(std::uint64_t adminId,
              std::string adminName,
              std::uint64_t userId,
              std::string ip,
              std::string userAgent,
              std::string requestId,
              std::function<void(Result<bool>)> cb);

  private:
    std::shared_ptr<UserRepository> userRepo_;
    std::shared_ptr<AuditService> auditService_;
};
