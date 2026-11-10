#include "filters/AuthFilter.h"

#include <drogon/drogon.h>

#include <cstdint>
#include <utility>
#include "common/ApiResponse.h"
#include "common/AppError.h"

AuthFilter::AuthFilter(std::string secret, std::string issuer)
    : codec_(std::move(secret), std::move(issuer))
{
}

void AuthFilter::doFilter(const drogon::HttpRequestPtr &req,
                          drogon::FilterCallback &&fcb,
                          drogon::FilterChainCallback &&fccb)
{
    try
    {
        const std::string &header = req->getHeader("authorization");
        const bool hasBearer =
            header.size() > 7 &&
            (header.compare(0, 7, "Bearer ") == 0 ||
             header.compare(0, 7, "bearer ") == 0);
        if (!hasBearer)
        {
            fcb(makeError(AppError::unauthorized()));
            return;
        }

        const std::string token = header.substr(7);
        JwtClaims claims;
        const auto status = codec_.verify(token, claims);
        if (status == JwtStatus::Expired)
        {
            fcb(makeError(AppError::tokenExpired()));
            return;
        }
        if (status != JwtStatus::Ok)
        {
            fcb(makeError(AppError::unauthorized()));
            return;
        }

        auto attributes = req->attributes();
        attributes->insert("admin_id",
                           static_cast<std::int64_t>(claims.adminId));
        attributes->insert("admin_username", claims.username);
        attributes->insert("admin_role", claims.role);

        fccb();
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "auth filter failed: " << e.what();
        fcb(makeError(AppError::internal()));
    }
    catch (...)
    {
        LOG_ERROR << "auth filter failed with unknown exception";
        fcb(makeError(AppError::internal()));
    }
}
