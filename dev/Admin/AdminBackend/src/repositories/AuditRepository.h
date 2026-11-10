#pragma once

#include <drogon/orm/DbClient.h>
#include <functional>
#include <memory>
#include "common/Result.h"
#include "models/AuditLog.h"

class AuditRepository
    : public std::enable_shared_from_this<AuditRepository>
{
  public:
    explicit AuditRepository(drogon::orm::DbClientPtr db);

    void insert(const AuditLog &log,
                std::function<void(Result<bool>)> cb);

  private:
    drogon::orm::DbClientPtr db_;
};
