#pragma once

#include <drogon/HttpFilter.h>
#include <string>
#include "common/JwtCodec.h"

class AuthFilter : public drogon::HttpFilter<AuthFilter, false>
{
  public:
    AuthFilter(std::string secret, std::string issuer);

    void doFilter(const drogon::HttpRequestPtr &req,
                  drogon::FilterCallback &&fcb,
                  drogon::FilterChainCallback &&fccb) override;

  private:
    JwtCodec codec_;
};
