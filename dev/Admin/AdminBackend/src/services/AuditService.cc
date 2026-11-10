#include "services/AuditService.h"

#include <drogon/drogon.h>

#include <utility>
#include "common/Result.h"
#include "repositories/AuditRepository.h"

AuditService::AuditService(std::shared_ptr<AuditRepository> auditRepo)
    : auditRepo_(std::move(auditRepo))
{
}

void AuditService::record(const AuditLog &log)
{
    if (!auditRepo_)
    {
        return;
    }

    auto self = shared_from_this();
    auditRepo_->insert(
        log,
        [self](Result<bool> result) {
            (void)self;
            try
            {
                if (!result.ok())
                {
                    LOG_WARN << "audit log write failed: "
                             << result.error().message();
                }
            }
            catch (...)
            {
                LOG_WARN << "audit log callback failed";
            }
        });
}
