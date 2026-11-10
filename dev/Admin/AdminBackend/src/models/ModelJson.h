#pragma once

#include <nlohmann/json.hpp>
#include <utility>
#include "models/AdminUser.h"
#include "models/Stats.h"
#include "models/UserSummary.h"

inline nlohmann::json toJson(const AdminProfile &profile)
{
    return nlohmann::json{
        {"id", profile.id},
        {"username", profile.username},
        {"name", profile.name},
        {"role", profile.role},
        {"roleName", profile.roleName}};
}

inline nlohmann::json toJson(const LoginResult &result)
{
    return nlohmann::json{
        {"token", result.token},
        {"refreshToken", result.refreshToken},
        {"admin", toJson(result.admin)}};
}

inline nlohmann::json toJson(const UserSummary &user)
{
    return nlohmann::json{
        {"id", user.id},
        {"username", user.username},
        {"nickname", user.nickname},
        {"status", user.status},
        {"friends", user.friends},
        {"conversations", user.conversations},
        {"messages", user.messages},
        {"createdAt", user.createdAt},
        {"lastActive", user.lastActive}};
}

inline nlohmann::json toJson(const UserPage &page)
{
    nlohmann::json list = nlohmann::json::array();
    for (const auto &user : page.list)
    {
        list.push_back(toJson(user));
    }
    return nlohmann::json{
        {"list", std::move(list)},
        {"total", page.total},
        {"page", page.page},
        {"pageSize", page.pageSize}};
}

inline nlohmann::json toJson(const UserStatusResult &result)
{
    return nlohmann::json{
        {"id", result.id},
        {"status", result.status}};
}

inline nlohmann::json toJson(const StatsOverview &stats)
{
    return nlohmann::json{
        {"onlineUsers", stats.onlineUsers},
        {"onlineUsersAvailable", stats.onlineUsersAvailable},
        {"todayMessages", stats.todayMessages},
        {"activeUsers", stats.activeUsers},
        {"newUsers", stats.newUsers}};
}

inline nlohmann::json toJson(const TrendData &trend)
{
    return nlohmann::json{
        {"dates", trend.dates},
        {"messages", trend.messages},
        {"actives", trend.actives}};
}
