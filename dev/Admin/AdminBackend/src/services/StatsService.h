#pragma once

#include <functional>
#include <memory>
#include "common/Result.h"
#include "models/Stats.h"

class StatsRepository;

class StatsService : public std::enable_shared_from_this<StatsService>
{
  public:
    explicit StatsService(std::shared_ptr<StatsRepository> statsRepo);

    void overview(std::function<void(Result<StatsOverview>)> cb);
    void trend(int days, std::function<void(Result<TrendData>)> cb);

  private:
    std::shared_ptr<StatsRepository> statsRepo_;
};
