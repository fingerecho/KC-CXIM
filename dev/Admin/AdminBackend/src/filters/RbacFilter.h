#pragma once

#include <drogon/HttpFilter.h>

class RbacFilter : public drogon::HttpFilter<RbacFilter, false>
{
  public:
    void doFilter(const drogon::HttpRequestPtr &req,
                  drogon::FilterCallback &&fcb,
                  drogon::FilterChainCallback &&fccb) override;
};
