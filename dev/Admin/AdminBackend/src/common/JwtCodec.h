#pragma once

#include <cstdint>
#include <string>

enum class JwtStatus
{
    Ok,
    Invalid,
    Expired,
    IssuerMismatch
};

struct JwtClaims
{
    std::uint64_t adminId{0};
    std::string username;
    std::string role;
    std::string issuer;
    std::int64_t issuedAt{0};
    std::int64_t expiresAt{0};
    std::string jti;
};

class JwtCodec
{
  public:
    JwtCodec(std::string secret, std::string issuer);

    std::string sign(const JwtClaims &claims) const;
    JwtStatus verify(const std::string &token, JwtClaims &out) const;

  private:
    std::string secret_;
    std::string issuer_;
};
