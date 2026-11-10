#include "services/AuthService.h"

#include <drogon/drogon.h>

#include <cstddef>
#include <ctime>
#include <utility>
#include "common/CryptoUtil.h"
#include "common/JwtCodec.h"
#include "common/PasswordHasher.h"
#include "repositories/AdminRepository.h"
#include "repositories/SessionRepository.h"

namespace
{
std::string limit(std::string value, std::size_t maxLength)
{
    if (value.size() > maxLength)
    {
        value.resize(maxLength);
    }
    return value;
}
}  // namespace

AuthService::AuthService(std::shared_ptr<AdminRepository> adminRepo,
                         std::shared_ptr<SessionRepository> sessionRepo,
                         std::shared_ptr<JwtCodec> jwt,
                         int accessTtlSeconds,
                         int refreshTtlSeconds)
    : adminRepo_(std::move(adminRepo)),
      sessionRepo_(std::move(sessionRepo)),
      jwt_(std::move(jwt)),
      accessTtlSeconds_(accessTtlSeconds),
      refreshTtlSeconds_(refreshTtlSeconds)
{
}

void AuthService::login(
    std::string username,
    std::string password,
    std::string ip,
    std::string userAgent,
    std::function<void(Result<LoginResult>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<LoginResult>>>(
        std::move(cb));

    try
    {
        if (username.empty())
        {
            (*once)(AppError::invalidParam("username", "用户名不能为空"));
            return;
        }
        if (password.empty())
        {
            (*once)(AppError::invalidParam("password", "密码不能为空"));
            return;
        }
        if (username.size() > 64)
        {
            (*once)(AppError::invalidParam("username", "用户名过长"));
            return;
        }
        if (password.size() > 256)
        {
            (*once)(AppError::invalidParam("password", "密码过长"));
            return;
        }

        auto self = shared_from_this();
        adminRepo_->findByUsername(
            username,
            [self,
             once,
             password = std::move(password),
             ip = limit(std::move(ip), 64),
             userAgent = std::move(userAgent)](
                Result<AdminUser> result) mutable {
                try
                {
                    if (!result.ok())
                    {
                        if (result.error().code() == "NOT_FOUND")
                        {
                            (*once)(AppError::invalidCredentials());
                        }
                        else
                        {
                            (*once)(result.error());
                        }
                        return;
                    }

                    AdminUser admin = result.value();
                    if (!PasswordHasher::verify(
                            password, admin.passwordHash))
                    {
                        (*once)(AppError::invalidCredentials());
                        return;
                    }
                    if (admin.status != 1)
                    {
                        (*once)(AppError::forbidden("账号已被禁用"));
                        return;
                    }

                    self->createSessionAndIssue(
                        admin,
                        std::move(ip),
                        std::move(userAgent),
                        once);
                }
                catch (const AppError &e)
                {
                    (*once)(e);
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR << "login callback failed: " << e.what();
                    (*once)(AppError::internal());
                }
                catch (...)
                {
                    (*once)(AppError::internal());
                }
            });
    }
    catch (const AppError &e)
    {
        (*once)(e);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "login failed: " << e.what();
        (*once)(AppError::internal());
    }
    catch (...)
    {
        (*once)(AppError::internal());
    }
}

void AuthService::createSessionAndIssue(
    const AdminUser &admin,
    std::string ip,
    std::string userAgent,
    std::shared_ptr<OnceCallback<Result<LoginResult>>> once)
{
    try
    {
        std::string refreshToken = CryptoUtil::randomHex(32);
        const std::string tokenHash =
            CryptoUtil::sha256Hex(refreshToken);
        const std::int64_t expiresAt =
            static_cast<std::int64_t>(std::time(nullptr)) +
            refreshTtlSeconds_;

        auto self = shared_from_this();
        sessionRepo_->create(
            admin.id,
            tokenHash,
            ip,
            userAgent,
            expiresAt,
            [self,
             once,
             admin,
             refreshToken = std::move(refreshToken)](
                Result<bool> result) mutable {
                try
                {
                    if (!result.ok())
                    {
                        (*once)(result.error());
                        return;
                    }
                    if (!result.value())
                    {
                        (*once)(AppError::internal());
                        return;
                    }

                    self->adminRepo_->updateLastLogin(admin.id);

                    LoginResult loginResult;
                    loginResult.token = self->makeAccessToken(admin);
                    loginResult.refreshToken = std::move(refreshToken);
                    loginResult.admin = self->makeProfile(admin);
                    (*once)(Result<LoginResult>(std::move(loginResult)));
                }
                catch (const AppError &e)
                {
                    (*once)(e);
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR << "issue session failed: " << e.what();
                    (*once)(AppError::internal());
                }
                catch (...)
                {
                    (*once)(AppError::internal());
                }
            });
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "create session failed: " << e.what();
        (*once)(AppError::internal());
    }
    catch (...)
    {
        (*once)(AppError::internal());
    }
}

void AuthService::refresh(
    std::string refreshToken,
    std::string ip,
    std::string userAgent,
    std::function<void(Result<LoginResult>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<LoginResult>>>(
        std::move(cb));

    try
    {
        if (refreshToken.empty())
        {
            (*once)(AppError::unauthorized("登录已过期，请重新登录"));
            return;
        }

        auto self = shared_from_this();
        const std::string oldHash =
            CryptoUtil::sha256Hex(refreshToken);
        ip = limit(std::move(ip), 64);

        sessionRepo_->findValidByHash(
            oldHash,
            [self, once, oldHash, ip, userAgent](
                Result<SessionInfo> result) mutable {
                try
                {
                    if (!result.ok())
                    {
                        (*once)(AppError::unauthorized(
                            "登录已过期，请重新登录"));
                        return;
                    }
                    if (!result.value().adminId)
                    {
                        (*once)(AppError::unauthorized(
                            "登录已过期，请重新登录"));
                        return;
                    }

                    const std::uint64_t adminId =
                        result.value().adminId;
                    self->adminRepo_->findById(
                        adminId,
                        [self, once, oldHash, ip, userAgent](
                            Result<AdminUser> adminResult) mutable {
                            try
                            {
                                if (!adminResult.ok())
                                {
                                    (*once)(AppError::unauthorized(
                                        "登录已过期，请重新登录"));
                                    return;
                                }
                                if (adminResult.value().status != 1)
                                {
                                    (*once)(AppError::forbidden(
                                        "账号已被禁用"));
                                    return;
                                }

                                AdminUser admin = adminResult.value();
                                self->sessionRepo_->revokeByHash(
                                    oldHash,
                                    [self, once, admin, ip, userAgent](
                                        Result<bool> revokeResult) mutable {
                                        try
                                        {
                                            if (!revokeResult.ok())
                                            {
                                                (*once)(
                                                    revokeResult.error());
                                                return;
                                            }
                                            if (!revokeResult.value())
                                            {
                                                (*once)(
                                                    AppError::unauthorized(
                                                        "登录已过期，请重新登录"));
                                                return;
                                            }
                                            self->createSessionAndIssue(
                                                admin, ip, userAgent, once);
                                        }
                                        catch (const AppError &e)
                                        {
                                            (*once)(e);
                                        }
                                        catch (const std::exception &e)
                                        {
                                            LOG_ERROR
                                                << "refresh revoke failed: "
                                                << e.what();
                                            (*once)(AppError::internal());
                                        }
                                        catch (...)
                                        {
                                            (*once)(AppError::internal());
                                        }
                                    });
                            }
                            catch (const AppError &e)
                            {
                                (*once)(e);
                            }
                            catch (const std::exception &e)
                            {
                                LOG_ERROR << "refresh admin failed: "
                                          << e.what();
                                (*once)(AppError::internal());
                            }
                            catch (...)
                            {
                                (*once)(AppError::internal());
                            }
                        });
                }
                catch (const AppError &e)
                {
                    (*once)(e);
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR << "refresh session failed: " << e.what();
                    (*once)(AppError::internal());
                }
                catch (...)
                {
                    (*once)(AppError::internal());
                }
            });
    }
    catch (const AppError &e)
    {
        (*once)(e);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "refresh failed: " << e.what();
        (*once)(AppError::internal());
    }
    catch (...)
    {
        (*once)(AppError::internal());
    }
}

void AuthService::logout(
    std::string refreshToken,
    std::uint64_t adminId,
    std::function<void(Result<bool>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<bool>>>(std::move(cb));

    try
    {
        if (!refreshToken.empty())
        {
            const std::string tokenHash =
                CryptoUtil::sha256Hex(refreshToken);
            sessionRepo_->revokeByHash(
                tokenHash,
                [once](Result<bool> result) {
                    if (!result.ok())
                    {
                        (*once)(result.error());
                        return;
                    }
                    (*once)(Result<bool>(true));
                });
            return;
        }

        if (adminId > 0)
        {
            sessionRepo_->revokeAllForAdmin(
                adminId,
                [once](Result<bool> result) {
                    if (!result.ok())
                    {
                        (*once)(result.error());
                        return;
                    }
                    (*once)(Result<bool>(true));
                });
            return;
        }

        (*once)(AppError::unauthorized());
    }
    catch (const AppError &e)
    {
        (*once)(e);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "logout failed: " << e.what();
        (*once)(AppError::internal());
    }
    catch (...)
    {
        (*once)(AppError::internal());
    }
}

void AuthService::me(
    std::uint64_t adminId,
    std::function<void(Result<AdminProfile>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<AdminProfile>>>(
        std::move(cb));

    try
    {
        if (adminId == 0)
        {
            (*once)(AppError::unauthorized());
            return;
        }

        adminRepo_->findById(
            adminId,
            [once](Result<AdminUser> result) {
                try
                {
                    if (!result.ok())
                    {
                        (*once)(result.error());
                        return;
                    }
                    if (result.value().status != 1)
                    {
                        (*once)(AppError::forbidden("账号已被禁用"));
                        return;
                    }
                    (*once)(Result<AdminProfile>(
                        makeProfile(result.value())));
                }
                catch (const AppError &e)
                {
                    (*once)(e);
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR << "me failed: " << e.what();
                    (*once)(AppError::internal());
                }
                catch (...)
                {
                    (*once)(AppError::internal());
                }
            });
    }
    catch (const AppError &e)
    {
        (*once)(e);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "me failed: " << e.what();
        (*once)(AppError::internal());
    }
    catch (...)
    {
        (*once)(AppError::internal());
    }
}

std::string AuthService::makeAccessToken(const AdminUser &admin) const
{
    JwtClaims claims;
    claims.adminId = admin.id;
    claims.username = admin.username;
    claims.role = admin.role;
    claims.issuedAt = static_cast<std::int64_t>(std::time(nullptr));
    claims.expiresAt = claims.issuedAt + accessTtlSeconds_;
    claims.jti = drogon::utils::getUuid();
    return jwt_->sign(claims);
}

AdminProfile AuthService::makeProfile(const AdminUser &admin)
{
    AdminProfile profile;
    profile.id = admin.id;
    profile.username = admin.username;
    profile.name = admin.nickname.empty() ? admin.username : admin.nickname;
    profile.role = admin.role;
    profile.roleName = roleName(admin.role);
    return profile;
}

std::string AuthService::roleName(const std::string &role)
{
    if (role == "super_admin")
    {
        return "超级管理员";
    }
    if (role == "sys_admin")
    {
        return "系统管理员";
    }
    if (role == "operator")
    {
        return "运营";
    }
    if (role == "moderator")
    {
        return "审核员";
    }
    if (role == "support")
    {
        return "客服";
    }
    if (role == "auditor")
    {
        return "审计员";
    }
    return role;
}
