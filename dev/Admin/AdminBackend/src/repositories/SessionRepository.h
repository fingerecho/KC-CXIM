#pragma once

#include <drogon/orm/DbClient.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "common/Result.h"

struct SessionInfo
{
    std::uint64_t id{0};
    std::uint64_t adminId{0};
};

class SessionRepository
    : public std::enable_shared_from_this<SessionRepository>
{
  public:
    explicit SessionRepository(drogon::orm::DbClientPtr db);

    void create(std::uint64_t adminId,
                const std::string &tokenHash,
                const std::string &ip,
                const std::string &userAgent,
                std::int64_t expiresAtEpoch,
                std::function<void(Result<bool>)> cb);

    void findValidByHash(
        const std::string &tokenHash,
        std::function<void(Result<SessionInfo>)> cb);

    void revokeByHash(const std::string &tokenHash,
                      std::function<void(Result<bool>)> cb);

    void revokeAllForAdmin(std::uint64_t adminId,
                           std::function<void(Result<bool>)> cb);

  private:
    drogon::orm::DbClientPtr db_;
};
