#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct StatsOverview
{
    std::uint64_t onlineUsers{0};
    bool onlineUsersAvailable{false};
    std::uint64_t todayMessages{0};
    std::uint64_t activeUsers{0};
    std::uint64_t newUsers{0};
};

struct TrendData
{
    std::vector<std::string> dates;
    std::vector<std::uint64_t> messages;
    std::vector<std::uint64_t> actives;
};
