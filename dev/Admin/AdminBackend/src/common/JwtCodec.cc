#include "common/JwtCodec.h"

#include <nlohmann/json.hpp>
#include <drogon/utils/Utilities.h>

#include <ctime>
#include <stdexcept>
#include <string_view>
#include <utility>
#include "common/CryptoUtil.h"

namespace
{
std::string base64UrlEncode(std::string_view data)
{
    return drogon::utils::base64EncodeUnpadded(data, true);
}

bool base64UrlDecode(std::string_view input, std::string &out)
{
    std::string normalized(input);
    for (char &ch : normalized)
    {
        if (ch == '-')
        {
            ch = '+';
        }
        else if (ch == '_')
        {
            ch = '/';
        }
    }

    if (normalized.size() % 4 == 1)
    {
        return false;
    }
    while (normalized.size() % 4 != 0)
    {
        normalized.push_back('=');
    }

    try
    {
        out = drogon::utils::base64Decode(normalized);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool readUint64(const nlohmann::json &j,
                const char *key,
                std::uint64_t &out)
{
    const auto it = j.find(key);
    if (it == j.end())
    {
        return false;
    }

    try
    {
        if (it->is_number_unsigned())
        {
            out = it->get<std::uint64_t>();
            return true;
        }
        if (it->is_number_integer())
        {
            const auto value = it->get<std::int64_t>();
            if (value < 0)
            {
                return false;
            }
            out = static_cast<std::uint64_t>(value);
            return true;
        }
        if (it->is_string())
        {
            const auto text = it->get<std::string>();
            std::size_t pos = 0;
            const auto value = std::stoull(text, &pos);
            if (pos != text.size())
            {
                return false;
            }
            out = value;
            return true;
        }
    }
    catch (...)
    {
        return false;
    }
    return false;
}
}  // namespace

JwtCodec::JwtCodec(std::string secret, std::string issuer)
    : secret_(std::move(secret)), issuer_(std::move(issuer))
{
}

std::string JwtCodec::sign(const JwtClaims &claims) const
{
    nlohmann::json header{{"alg", "HS256"}, {"typ", "JWT"}};
    nlohmann::json payload{
        {"sub", std::to_string(claims.adminId)},
        {"username", claims.username},
        {"role", claims.role},
        {"iss", claims.issuer.empty() ? issuer_ : claims.issuer},
        {"iat", claims.issuedAt},
        {"exp", claims.expiresAt},
        {"jti", claims.jti}};

    const std::string signingInput =
        base64UrlEncode(header.dump()) + "." + base64UrlEncode(payload.dump());
    const std::string signature =
        CryptoUtil::hmacSha256(signingInput, secret_);
    return signingInput + "." + base64UrlEncode(signature);
}

JwtStatus JwtCodec::verify(const std::string &token, JwtClaims &out) const
{
    try
    {
        const auto firstDot = token.find('.');
        if (firstDot == std::string::npos)
        {
            return JwtStatus::Invalid;
        }
        const auto secondDot = token.find('.', firstDot + 1);
        if (secondDot == std::string::npos)
        {
            return JwtStatus::Invalid;
        }
        if (token.find('.', secondDot + 1) != std::string::npos)
        {
            return JwtStatus::Invalid;
        }

        const std::string headerPart = token.substr(0, firstDot);
        const std::string payloadPart =
            token.substr(firstDot + 1, secondDot - firstDot - 1);
        const std::string signaturePart = token.substr(secondDot + 1);

        std::string headerJson;
        std::string payloadJson;
        std::string signature;
        if (!base64UrlDecode(headerPart, headerJson) ||
            !base64UrlDecode(payloadPart, payloadJson) ||
            !base64UrlDecode(signaturePart, signature))
        {
            return JwtStatus::Invalid;
        }

        const auto header = nlohmann::json::parse(headerJson);
        if (!header.is_object() ||
            header.value("alg", std::string{}) != "HS256")
        {
            return JwtStatus::Invalid;
        }

        const std::string expectedSignature = CryptoUtil::hmacSha256(
            token.substr(0, secondDot), secret_);
        if (!CryptoUtil::constantTimeEquals(expectedSignature, signature))
        {
            return JwtStatus::Invalid;
        }

        const auto payload = nlohmann::json::parse(payloadJson);
        if (!payload.is_object())
        {
            return JwtStatus::Invalid;
        }

        if (payload.value("iss", std::string{}) != issuer_)
        {
            return JwtStatus::IssuerMismatch;
        }

        const auto expIt = payload.find("exp");
        if (expIt == payload.end() || !expIt->is_number_integer())
        {
            return JwtStatus::Invalid;
        }
        const auto expiresAt = expIt->get<std::int64_t>();
        if (expiresAt <= static_cast<std::int64_t>(std::time(nullptr)))
        {
            return JwtStatus::Expired;
        }

        JwtClaims claims;
        if (!readUint64(payload, "sub", claims.adminId))
        {
            return JwtStatus::Invalid;
        }
        claims.username = payload.value("username", std::string{});
        claims.role = payload.value("role", std::string{});
        if (claims.username.empty() || claims.role.empty())
        {
            return JwtStatus::Invalid;
        }
        claims.issuer = issuer_;
        claims.issuedAt = payload.value("iat", static_cast<std::int64_t>(0));
        claims.expiresAt = expiresAt;
        claims.jti = payload.value("jti", std::string{});

        out = std::move(claims);
        return JwtStatus::Ok;
    }
    catch (...)
    {
        return JwtStatus::Invalid;
    }
}
