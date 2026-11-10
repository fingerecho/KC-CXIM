#include "repositories/AdminRepository.h"

#include <drogon/drogon.h>
#include <drogon/orm/Row.h>

#include <utility>
#include "common/Once.h"

namespace
{
AdminUser mapAdmin(const drogon::orm::Row &row, bool withPassword)
{
    AdminUser admin;
    admin.id = row["id"].as<unsigned long long>();
    admin.username = row["username"].as<std::string>();
    admin.nickname = row["nickname"].as<std::string>();
    admin.role = row["role"].as<std::string>();
    admin.status = row["status"].as<int>();
    if (withPassword)
    {
        admin.passwordHash = row["password_hash"].as<std::string>();
    }
    return admin;
}
}  // namespace

AdminRepository::AdminRepository(drogon::orm::DbClientPtr db)
    : db_(std::move(db))
{
}

void AdminRepository::findByUsername(
    const std::string &username,
    std::function<void(Result<AdminUser>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<AdminUser>>>(
        std::move(cb));

    db_->execSqlAsync(
        "SELECT id, username, password_hash, nickname, role, status "
        "FROM admin_users WHERE username = ? LIMIT 1",
        [once](const drogon::orm::Result &result) {
            try
            {
                if (result.empty())
                {
                    (*once)(AppError::notFound("username",
                                               "管理员不存在"));
                    return;
                }
                (*once)(Result<AdminUser>(mapAdmin(result[0], true)));
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "map admin failed: " << e.what();
                (*once)(AppError::internal());
            }
            catch (...)
            {
                (*once)(AppError::internal());
            }
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "find admin by username failed: " << e.base().what();
            (*once)(AppError::internal());
        },
        username);
}

void AdminRepository::findById(
    std::uint64_t id,
    std::function<void(Result<AdminUser>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<AdminUser>>>(
        std::move(cb));

    db_->execSqlAsync(
        "SELECT id, username, nickname, role, status "
        "FROM admin_users WHERE id = ? LIMIT 1",
        [once](const drogon::orm::Result &result) {
            try
            {
                if (result.empty())
                {
                    (*once)(AppError::notFound("id", "管理员不存在"));
                    return;
                }
                (*once)(Result<AdminUser>(mapAdmin(result[0], false)));
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "map admin failed: " << e.what();
                (*once)(AppError::internal());
            }
            catch (...)
            {
                (*once)(AppError::internal());
            }
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "find admin by id failed: " << e.base().what();
            (*once)(AppError::internal());
        },
        static_cast<long long>(id));
}

void AdminRepository::updateLastLogin(std::uint64_t id)
{
    db_->execSqlAsync(
        "UPDATE admin_users SET last_login_at = NOW() WHERE id = ?",
        [](const drogon::orm::Result &) {},
        [](const drogon::orm::DrogonDbException &e) {
            LOG_WARN << "update admin last_login_at failed: "
                     << e.base().what();
        },
        static_cast<long long>(id));
}
