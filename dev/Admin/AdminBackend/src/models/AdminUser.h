#pragma once

#include <cstdint>
#include <string>

struct AdminUser
{
    std::uint64_t id{0};
    std::string username;
    std::string passwordHash;
    std::string nickname;
    std::string role;
    int status{1};
};

struct AdminProfile
{
    std::uint64_t id{0};
    std::string username;
    std::string name;
    std::string role;
    std::string roleName;
};

struct LoginResult
{
    std::string token;
    std::string refreshToken;
    AdminProfile admin;
};
