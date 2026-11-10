#include "repositories/AuditRepository.h"

#include <drogon/drogon.h>

#include <utility>
#include "common/Once.h"

AuditRepository::AuditRepository(drogon::orm::DbClientPtr db)
    : db_(std::move(db))
{
}

void AuditRepository::insert(
    const AuditLog &log,
    std::function<void(Result<bool>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<bool>>>(std::move(cb));

    db_->execSqlAsync(
        "INSERT INTO audit_logs "
        "(admin_id, admin_name, action, module, target_type, target_id, "
        "before_json, after_json, ip, user_agent, request_id, result) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        [once](const drogon::orm::Result &result) {
            try
            {
                (*once)(Result<bool>(result.affectedRows() > 0));
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "audit result callback failed: "
                          << e.what();
                (*once)(AppError::internal());
            }
            catch (...)
            {
                (*once)(AppError::internal());
            }
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "insert audit log failed: " << e.base().what();
            (*once)(AppError::internal());
        },
        static_cast<long long>(log.adminId),
        log.adminName,
        log.action,
        log.module,
        log.targetType,
        log.targetId,
        log.beforeJson,
        log.afterJson,
        log.ip,
        log.userAgent,
        log.requestId,
        log.result);
}
