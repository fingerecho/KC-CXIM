#include "services/UserService.h"

#include <nlohmann/json.hpp>
#include <drogon/drogon.h>

#include <cstddef>
#include <utility>
#include "common/Once.h"
#include "models/AuditLog.h"
#include "repositories/UserRepository.h"
#include "services/AuditService.h"

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

UserService::UserService(std::shared_ptr<UserRepository> userRepo,
                         std::shared_ptr<AuditService> auditService)
    : userRepo_(std::move(userRepo)),
      auditService_(std::move(auditService))
{
}

void UserService::listUsers(
    int page,
    int pageSize,
    std::string keyword,
    std::string status,
    std::function<void(Result<UserPage>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<UserPage>>>(
        std::move(cb));

    try
    {
        if (page < 1)
        {
            (*once)(AppError::invalidParam("page", "页码必须大于 0"));
            return;
        }
        if (pageSize < 1 || pageSize > 100)
        {
            (*once)(AppError::invalidParam(
                "page_size", "每页数量必须在 1 到 100 之间"));
            return;
        }
        if (keyword.size() > 128)
        {
            (*once)(AppError::invalidParam("keyword", "关键词过长"));
            return;
        }
        if (status.empty())
        {
            status = "all";
        }
        if (status != "all" && status != "active" && status != "banned")
        {
            (*once)(AppError::invalidParam(
                "status", "状态只能是 all、active 或 banned"));
            return;
        }

        userRepo_->countAndList(
            page,
            pageSize,
            keyword,
            status,
            [once](Result<UserPage> result) {
                try
                {
                    (*once)(std::move(result));
                }
                catch (const AppError &e)
                {
                    (*once)(e);
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR << "list users callback failed: "
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
        LOG_ERROR << "list users failed: " << e.what();
        (*once)(AppError::internal());
    }
    catch (...)
    {
        (*once)(AppError::internal());
    }
}

void UserService::getUser(
    std::uint64_t id,
    std::function<void(Result<UserSummary>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<UserSummary>>>(
        std::move(cb));

    try
    {
        if (id == 0)
        {
            (*once)(AppError::invalidParam("id", "用户 ID 不能为空"));
            return;
        }

        userRepo_->findById(
            id,
            [once](Result<UserSummary> result) {
                try
                {
                    (*once)(std::move(result));
                }
                catch (const AppError &e)
                {
                    (*once)(e);
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR << "get user callback failed: "
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
        LOG_ERROR << "get user failed: " << e.what();
        (*once)(AppError::internal());
    }
    catch (...)
    {
        (*once)(AppError::internal());
    }
}

void UserService::updateStatus(
    std::uint64_t adminId,
    std::string adminName,
    std::uint64_t userId,
    std::string status,
    std::string ip,
    std::string userAgent,
    std::string requestId,
    std::function<void(Result<UserStatusResult>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<UserStatusResult>>>(
        std::move(cb));

    try
    {
        if (adminId == 0)
        {
            (*once)(AppError::unauthorized());
            return;
        }
        if (userId == 0)
        {
            (*once)(AppError::invalidParam("id", "用户 ID 不能为空"));
            return;
        }
        if (status != "active" && status != "banned")
        {
            (*once)(AppError::invalidParam(
                "status", "状态只能是 active 或 banned"));
            return;
        }

        adminName = limit(std::move(adminName), 64);
        ip = limit(std::move(ip), 64);
        userAgent = limit(std::move(userAgent), 255);
        requestId = limit(std::move(requestId), 64);

        auto self = shared_from_this();
        userRepo_->findById(
            userId,
            [self,
             once,
             adminId,
             adminName,
             userId,
             status,
             ip,
             userAgent,
             requestId](Result<UserSummary> beforeResult) mutable {
                try
                {
                    if (!beforeResult.ok())
                    {
                        (*once)(beforeResult.error());
                        return;
                    }

                    const UserSummary before = beforeResult.value();
                    if (before.status == status)
                    {
                        (*once)(AppError::conflict(
                            "status", "账号状态未发生变化"));
                        return;
                    }

                    const int targetStatus =
                        status == "banned" ? 0 : 1;
                    self->userRepo_->updateStatus(
                        userId,
                        targetStatus,
                        [self,
                         once,
                         adminId,
                         adminName,
                         userId,
                         status,
                         ip,
                         userAgent,
                         requestId,
                         before](Result<bool> updateResult) mutable {
                            try
                            {
                                if (!updateResult.ok())
                                {
                                    (*once)(updateResult.error());
                                    return;
                                }
                                if (!updateResult.value())
                                {
                                    (*once)(AppError::notFound(
                                        "id", "用户不存在"));
                                    return;
                                }

                                if (self->auditService_)
                                {
                                    AuditLog log;
                                    log.adminId = adminId;
                                    log.adminName = adminName;
                                    log.action = status == "banned"
                                                     ? "ban"
                                                     : "unban";
                                    log.module = "users";
                                    log.targetType = "user";
                                    log.targetId =
                                        std::to_string(userId);
                                    log.beforeJson =
                                        nlohmann::json{{"status",
                                                        before.status}}
                                            .dump();
                                    log.afterJson =
                                        nlohmann::json{{"status", status}}
                                            .dump();
                                    log.ip = ip;
                                    log.userAgent = userAgent;
                                    log.requestId = requestId;
                                    log.result = "success";
                                    self->auditService_->record(log);
                                }

                                UserStatusResult result;
                                result.id = userId;
                                result.status = status;
                                (*once)(Result<UserStatusResult>(
                                    std::move(result)));
                            }
                            catch (const AppError &e)
                            {
                                (*once)(e);
                            }
                            catch (const std::exception &e)
                            {
                                LOG_ERROR << "update user status failed: "
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
                    LOG_ERROR << "load user before update failed: "
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
        LOG_ERROR << "update user status failed: " << e.what();
        (*once)(AppError::internal());
    }
    catch (...)
    {
        (*once)(AppError::internal());
    }
}

void UserService::resetPassword(
    std::uint64_t adminId,
    std::string adminName,
    std::uint64_t userId,
    std::string ip,
    std::string userAgent,
    std::string requestId,
    std::function<void(Result<bool>)> cb)
{
    if (adminId == 0)
    {
        cb(Result<bool>(AppError::unauthorized()));
        return;
    }
    if (userId == 0)
    {
        cb(Result<bool>(AppError::invalidParam("id", "用户 ID 不能为空")));
        return;
    }
    cb(Result<bool>(AppError::notImplemented()));
}

void UserService::kick(
    std::uint64_t adminId,
    std::string adminName,
    std::uint64_t userId,
    std::string ip,
    std::string userAgent,
    std::string requestId,
    std::function<void(Result<bool>)> cb)
{
    if (adminId == 0)
    {
        cb(Result<bool>(AppError::unauthorized()));
        return;
    }
    if (userId == 0)
    {
        cb(Result<bool>(AppError::invalidParam("id", "用户 ID 不能为空")));
        return;
    }
    cb(Result<bool>(AppError::notImplemented()));
}
