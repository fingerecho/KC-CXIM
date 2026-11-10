#pragma once

#include <memory>
#include "models/AuditLog.h"

class AuditRepository;

class AuditService : public std::enable_shared_from_this<AuditService>
{
  public:
    explicit AuditService(std::shared_ptr<AuditRepository> auditRepo);

    void record(const AuditLog &log);

  private:
    std::shared_ptr<AuditRepository> auditRepo_;
};
