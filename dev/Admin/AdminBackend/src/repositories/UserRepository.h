#pragma once

#include <drogon/orm/DbClient.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "common/Result.h"
#include "models/UserSummary.h"

class UserRepository
    : public std::enable_shared_from_this<UserRepository>
{
  public:
    explicit UserRepository(drogon::orm::DbClientPtr db);

    void countAndList(
        int page,
        int pageSize,
        const std::string &keyword,
        const std::string &status,
        std::function<void(Result<UserPage>)> cb);

    void findById(std::uint64_t id,
                  std::function<void(Result<UserSummary>)> cb);

    void updateStatus(std::uint64_t id,
                      int status,
                      std::function<void(Result<bool>)> cb);

  private:
    drogon::orm::DbClientPtr db_;
};
