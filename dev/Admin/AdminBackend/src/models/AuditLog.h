#pragma once

#include <cstdint>
#include <string>

struct AuditLog
{
    std::uint64_t adminId{0};
    std::string adminName;
    std::string action;
    std::string module;
    std::string targetType;
    std::string targetId;
    std::string beforeJson;
    std::string afterJson;
    std::string ip;
    std::string userAgent;
    std::string requestId;
    std::string result{"success"};
};
