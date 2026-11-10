#include "services/StatsService.h"

#include <drogon/drogon.h>

#include <utility>
#include "common/Once.h"
#include "repositories/StatsRepository.h"

StatsService::StatsService(std::shared_ptr<StatsRepository> statsRepo)
    : statsRepo_(std::move(statsRepo))
{
}

void StatsService::overview(
    std::function<void(Result<StatsOverview>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<StatsOverview>>>(
        std::move(cb));

    try
    {
        auto self = shared_from_this();
        statsRepo_->overview(
            [self, once](Result<StatsOverview> result) {
                (void)self;
                try
                {
                    (*once)(std::move(result));
                }
                catch (const AppError &e)
                {
                    (*once)(e);
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR << "stats overview callback failed: "
                              << e.what();
                    (*once)(AppError::internal());
                }
                catch (...)
                {
                    (*once)(AppError::internal());
                }
            });
    }
    catch (const AppError &e)
    {
        (*once)(e);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "stats overview failed: " << e.what();
        (*once)(AppError::internal());
    }
    catch (...)
    {
        (*once)(AppError::internal());
    }
}

void StatsService::trend(
    int days,
    std::function<void(Result<TrendData>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<TrendData>>>(
        std::move(cb));

    try
    {
        if (days != 7 && days != 30)
        {
            (*once)(AppError::invalidParam(
                "days", "days 只能是 7 或 30"));
            return;
        }

        auto self = shared_from_this();
        statsRepo_->trend(
            days,
            [self, once](Result<TrendData> result) {
                (void)self;
                try
                {
                    (*once)(std::move(result));
                }
                catch (const AppError &e)
                {
                    (*once)(e);
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR << "stats trend callback failed: "
                              << e.what();
                    (*once)(AppError::internal());
                }
                catch (...)
                {
                    (*once)(AppError::internal());
                }
            });
    }
    catch (const AppError &e)
    {
        (*once)(e);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "stats trend failed: " << e.what();
        (*once)(AppError::internal());
    }
    catch (...)
    {
        (*once)(AppError::internal());
    }
}
