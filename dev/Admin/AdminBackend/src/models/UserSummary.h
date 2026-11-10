#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct UserSummary
{
    std::uint64_t id{0};
    std::string username;
    std::string nickname;
    std::string status;
    std::uint64_t friends{0};
    std::uint64_t conversations{0};
    std::uint64_t messages{0};
    std::string createdAt;
    std::string lastActive;
};

struct UserPage
{
    std::vector<UserSummary> list;
    std::uint64_t total{0};
    int page{1};
    int pageSize{20};
};

struct UserStatusResult
{
    std::uint64_t id{0};
    std::string status;
};
