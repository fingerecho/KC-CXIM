#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "common/Once.h"
#include "common/Result.h"
#include "models/AdminUser.h"

class AdminRepository;
class SessionRepository;
class JwtCodec;

class AuthService : public std::enable_shared_from_this<AuthService>
{
  public:
    AuthService(std::shared_ptr<AdminRepository> adminRepo,
                std::shared_ptr<SessionRepository> sessionRepo,
                std::shared_ptr<JwtCodec> jwt,
                int accessTtlSeconds,
                int refreshTtlSeconds);

    void login(std::string username,
               std::string password,
               std::string ip,
               std::string userAgent,
               std::function<void(Result<LoginResult>)> cb);

    void refresh(std::string refreshToken,
                 std::string ip,
                 std::string userAgent,
                 std::function<void(Result<LoginResult>)> cb);

    void logout(std::string refreshToken,
                std::uint64_t adminId,
                std::function<void(Result<bool>)> cb);

    void me(std::uint64_t adminId,
            std::function<void(Result<AdminProfile>)> cb);

  private:
    void createSessionAndIssue(
        const AdminUser &admin,
        std::string ip,
        std::string userAgent,
        std::shared_ptr<OnceCallback<Result<LoginResult>>> once);

    std::string makeAccessToken(const AdminUser &admin) const;
    static AdminProfile makeProfile(const AdminUser &admin);
    static std::string roleName(const std::string &role);

    std::shared_ptr<AdminRepository> adminRepo_;
    std::shared_ptr<SessionRepository> sessionRepo_;
    std::shared_ptr<JwtCodec> jwt_;
    int accessTtlSeconds_;
    int refreshTtlSeconds_;
};
