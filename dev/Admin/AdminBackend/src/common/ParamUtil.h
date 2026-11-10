#pragma once

#include <nlohmann/json.hpp>
#include <drogon/drogon.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include "common/AppError.h"

namespace ParamUtil
{
inline std::string trim(std::string value)
{
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
                value.end());
    return value;
}

inline std::string truncate(std::string value, std::size_t maxLength)
{
    if (value.size() > maxLength)
    {
        value.resize(maxLength);
    }
    return value;
}

inline int parseIntParam(const drogon::HttpRequestPtr &req,
                         const std::string &name,
                         int defaultValue,
                         int minValue,
                         int maxValue)
{
    const std::string &raw = req->getParameter(name);
    if (raw.empty())
    {
        return defaultValue;
    }

    try
    {
        std::size_t pos = 0;
        const long long value = std::stoll(raw, &pos);
        if (pos != raw.size() || value < minValue || value > maxValue)
        {
            throw AppError::invalidParam(
                name, "参数 " + name + " 超出允许范围");
        }
        return static_cast<int>(value);
    }
    catch (const AppError &)
    {
        throw;
    }
    catch (...)
    {
        throw AppError::invalidParam(name, "参数 " + name + " 不是合法整数");
    }
}

inline std::uint64_t parseUint64(const std::string &raw,
                                 const std::string &field)
{
    if (raw.empty())
    {
        throw AppError::invalidParam(field, "参数 " + field + " 不能为空");
    }

    try
    {
        std::size_t pos = 0;
        const unsigned long long value = std::stoull(raw, &pos);
        if (pos != raw.size() || value == 0)
        {
            throw AppError::invalidParam(
                field, "参数 " + field + " 必须是正整数");
        }
        return static_cast<std::uint64_t>(value);
    }
    catch (const AppError &)
    {
        throw;
    }
    catch (...)
    {
        throw AppError::invalidParam(field, "参数 " + field + " 不是合法整数");
    }
}

inline nlohmann::json parseJsonBody(const drogon::HttpRequestPtr &req)
{
    const auto body = req->body();
    if (body.empty())
    {
        throw AppError::invalidBody();
    }

    try
    {
        auto json = nlohmann::json::parse(std::string(body.data(), body.size()));
        if (!json.is_object())
        {
            throw AppError::invalidBody();
        }
        return json;
    }
    catch (const AppError &)
    {
        throw;
    }
    catch (const std::exception &)
    {
        throw AppError::invalidBody();
    }
}

inline nlohmann::json parseOptionalJsonBody(const drogon::HttpRequestPtr &req)
{
    if (req->body().empty())
    {
        return nlohmann::json::object();
    }
    return parseJsonBody(req);
}

inline std::string requestId(const drogon::HttpRequestPtr &req)
{
    return req->attributes()->get<std::string>("request_id");
}

inline std::int64_t adminId(const drogon::HttpRequestPtr &req)
{
    return req->attributes()->get<std::int64_t>("admin_id");
}

inline std::string adminUsername(const drogon::HttpRequestPtr &req)
{
    return req->attributes()->get<std::string>("admin_username");
}

inline std::string adminRole(const drogon::HttpRequestPtr &req)
{
    return req->attributes()->get<std::string>("admin_role");
}

inline std::string clientIp(const drogon::HttpRequestPtr &req)
{
    const std::string forwarded = trim(req->getHeader("x-forwarded-for"));
    if (!forwarded.empty())
    {
        const auto comma = forwarded.find(',');
        return comma == std::string::npos ? forwarded
                                     : trim(forwarded.substr(0, comma));
    }

    const std::string realIp = trim(req->getHeader("x-real-ip"));
    if (!realIp.empty())
    {
        return realIp;
    }

    return req->peerAddr().toIp();
}

inline std::string userAgent(const drogon::HttpRequestPtr &req)
{
    return truncate(trim(req->getHeader("user-agent")), 255);
}
}  // namespace ParamUtil
