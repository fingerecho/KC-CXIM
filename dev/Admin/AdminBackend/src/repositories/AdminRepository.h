#pragma once

#include <drogon/orm/DbClient.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "common/Result.h"
#include "models/AdminUser.h"

class AdminRepository
    : public std::enable_shared_from_this<AdminRepository>
{
  public:
    explicit AdminRepository(drogon::orm::DbClientPtr db);

    void findByUsername(
        const std::string &username,
        std::function<void(Result<AdminUser>)> cb);

    void findById(std::uint64_t id,
                  std::function<void(Result<AdminUser>)> cb);

    void updateLastLogin(std::uint64_t id);

  private:
    drogon::orm::DbClientPtr db_;
};
