#include "repositories/StatsRepository.h"

#include <drogon/drogon.h>

#include <ctime>
#include <map>
#include <string>
#include <utility>
#include "common/Once.h"

namespace
{
std::tm localTime(std::time_t value)
{
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &value);
#else
    localtime_r(&value, &tm);
#endif
    return tm;
}

std::string formatTime(const std::tm &tm, const char *format)
{
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), format, &tm);
    return buffer;
}
}  // namespace

StatsRepository::StatsRepository(drogon::orm::DbClientPtr db)
    : db_(std::move(db))
{
}

void StatsRepository::overview(
    std::function<void(Result<StatsOverview>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<StatsOverview>>>(
        std::move(cb));

    db_->execSqlAsync(
        "SELECT "
        "(SELECT COUNT(*) FROM messages WHERE created_at >= CURDATE()) "
        "AS today_messages, "
        "(SELECT COUNT(DISTINCT sender_id) FROM messages "
        " WHERE created_at >= CURDATE()) AS active_users, "
        "(SELECT COUNT(*) FROM users WHERE created_at >= CURDATE()) "
        "AS new_users",
        [once](const drogon::orm::Result &result) {
            try
            {
                StatsOverview stats;
                if (!result.empty())
                {
                    stats.todayMessages =
                        result[0]["today_messages"]
                            .as<unsigned long long>();
                    stats.activeUsers =
                        result[0]["active_users"]
                            .as<unsigned long long>();
                    stats.newUsers =
                        result[0]["new_users"].as<unsigned long long>();
                }
                stats.onlineUsers = 0;
                stats.onlineUsersAvailable = false;
                (*once)(Result<StatsOverview>(stats));
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "map stats overview failed: " << e.what();
                (*once)(AppError::internal());
            }
            catch (...)
            {
                (*once)(AppError::internal());
            }
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "query stats overview failed: " << e.base().what();
            (*once)(AppError::internal());
        });
}

void StatsRepository::trend(
    int days,
    std::function<void(Result<TrendData>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<TrendData>>>(
        std::move(cb));

    std::tm startTm = localTime(std::time(nullptr));
    startTm.tm_hour = 0;
    startTm.tm_min = 0;
    startTm.tm_sec = 0;
    startTm.tm_mday -= (days - 1);
    std::mktime(&startTm);

    const std::string startDate =
        formatTime(startTm, "%Y-%m-%d %H:%M:%S");

    db_->execSqlAsync(
        "SELECT DATE(created_at) AS d, COUNT(*) AS messages, "
        "COUNT(DISTINCT sender_id) AS actives "
        "FROM messages WHERE created_at >= ? "
        "GROUP BY DATE(created_at) ORDER BY d ASC",
        [once, startTm, days](const drogon::orm::Result &result) {
            try
            {
                std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
                    values;
                for (const auto &row : result)
                {
                    const std::string day = row["d"].as<std::string>();
                    values[day] = {
                        row["messages"].as<unsigned long long>(),
                        row["actives"].as<unsigned long long>()};
                }

                TrendData trend;
                for (int i = 0; i < days; ++i)
                {
                    std::tm dayTm = startTm;
                    dayTm.tm_mday += i;
                    std::mktime(&dayTm);

                    trend.dates.push_back(formatTime(dayTm, "%m/%d"));
                    const std::string key = formatTime(dayTm, "%Y-%m-%d");
                    const auto it = values.find(key);
                    if (it == values.end())
                    {
                        trend.messages.push_back(0);
                        trend.actives.push_back(0);
                    }
                    else
                    {
                        trend.messages.push_back(it->second.first);
                        trend.actives.push_back(it->second.second);
                    }
                }
                (*once)(Result<TrendData>(std::move(trend)));
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "map stats trend failed: " << e.what();
                (*once)(AppError::internal());
            }
            catch (...)
            {
                (*once)(AppError::internal());
            }
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "query stats trend failed: " << e.base().what();
            (*once)(AppError::internal());
        },
        startDate);
}
