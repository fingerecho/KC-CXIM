#pragma once

#include <drogon/orm/DbClient.h>
#include <functional>
#include <memory>
#include "common/Result.h"
#include "models/Stats.h"

class StatsRepository
    : public std::enable_shared_from_this<StatsRepository>
{
  public:
    explicit StatsRepository(drogon::orm::DbClientPtr db);

    void overview(std::function<void(Result<StatsOverview>)> cb);
    void trend(int days, std::function<void(Result<TrendData>)> cb);

  private:
    drogon::orm::DbClientPtr db_;
};
