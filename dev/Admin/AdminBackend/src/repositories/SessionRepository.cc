#include "repositories/SessionRepository.h"

#include <drogon/drogon.h>

#include <utility>
#include "common/Once.h"

SessionRepository::SessionRepository(drogon::orm::DbClientPtr db)
    : db_(std::move(db))
{
}

void SessionRepository::create(
    std::uint64_t adminId,
    const std::string &tokenHash,
    const std::string &ip,
    const std::string &userAgent,
    std::int64_t expiresAtEpoch,
    std::function<void(Result<bool>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<bool>>>(std::move(cb));

    db_->execSqlAsync(
        "INSERT INTO admin_sessions "
        "(admin_id, refresh_token_hash, ip, user_agent, expires_at, revoked) "
        "VALUES (?, ?, ?, ?, FROM_UNIXTIME(?), 0)",
        [once](const drogon::orm::Result &result) {
            try
            {
                (*once)(Result<bool>(result.affectedRows() > 0));
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "session result callback failed: "
                          << e.what();
                (*once)(AppError::internal());
            }
            catch (...)
            {
                (*once)(AppError::internal());
            }
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "create admin session failed: " << e.base().what();
            (*once)(AppError::internal());
        },
        static_cast<long long>(adminId),
        tokenHash,
        ip,
        userAgent,
        static_cast<long long>(expiresAtEpoch));
}

void SessionRepository::findValidByHash(
    const std::string &tokenHash,
    std::function<void(Result<SessionInfo>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<SessionInfo>>>(
        std::move(cb));

    db_->execSqlAsync(
        "SELECT id, admin_id FROM admin_sessions "
        "WHERE refresh_token_hash = ? AND revoked = 0 "
        "AND expires_at > NOW() LIMIT 1",
        [once](const drogon::orm::Result &result) {
            try
            {
                if (result.empty())
                {
                    (*once)(AppError::unauthorized(
                        "登录已过期，请重新登录"));
                    return;
                }
                SessionInfo info;
                info.id = result[0]["id"].as<unsigned long long>();
                info.adminId =
                    result[0]["admin_id"].as<unsigned long long>();
                (*once)(Result<SessionInfo>(info));
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "map admin session failed: " << e.what();
                (*once)(AppError::internal());
            }
            catch (...)
            {
                (*once)(AppError::internal());
            }
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "find admin session failed: " << e.base().what();
            (*once)(AppError::internal());
        },
        tokenHash);
}

void SessionRepository::revokeByHash(
    const std::string &tokenHash,
    std::function<void(Result<bool>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<bool>>>(std::move(cb));

    db_->execSqlAsync(
        "UPDATE admin_sessions SET revoked = 1 "
        "WHERE refresh_token_hash = ? AND revoked = 0",
        [once](const drogon::orm::Result &result) {
            try
            {
                (*once)(Result<bool>(result.affectedRows() > 0));
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "session result callback failed: "
                          << e.what();
                (*once)(AppError::internal());
            }
            catch (...)
            {
                (*once)(AppError::internal());
            }
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "revoke admin session failed: " << e.base().what();
            (*once)(AppError::internal());
        },
        tokenHash);
}

void SessionRepository::revokeAllForAdmin(
    std::uint64_t adminId,
    std::function<void(Result<bool>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<bool>>>(std::move(cb));

    db_->execSqlAsync(
        "UPDATE admin_sessions SET revoked = 1 "
        "WHERE admin_id = ? AND revoked = 0",
        [once](const drogon::orm::Result &result) {
            try
            {
                (*once)(Result<bool>(result.affectedRows() > 0));
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "session result callback failed: "
                          << e.what();
                (*once)(AppError::internal());
            }
            catch (...)
            {
                (*once)(AppError::internal());
            }
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "revoke admin sessions failed: "
                      << e.base().what();
            (*once)(AppError::internal());
        },
        static_cast<long long>(adminId));
}
