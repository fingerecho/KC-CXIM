#include "filters/AccessLogAdvice.h"

#include <drogon/drogon.h>

#include <chrono>

namespace
{
using StartTime = std::chrono::steady_clock::time_point;
}  // namespace

void registerAccessLogAdvice()
{
    drogon::app().registerPreRoutingAdvice(
        [](const drogon::HttpRequestPtr &req) {
            req->attributes()->insert(
                "start_time", std::chrono::steady_clock::now());
        });

    drogon::app().registerPreSendingAdvice(
        [](const drogon::HttpRequestPtr &req,
           const drogon::HttpResponsePtr &resp) {
            if (!resp)
            {
                return;
            }
            const auto start =
                req->attributes()->get<StartTime>("start_time");
            const auto now = std::chrono::steady_clock::now();
            const auto duration = std::chrono::duration_cast<
                std::chrono::milliseconds>(now - start);
            const auto &requestId =
                req->attributes()->get<std::string>("request_id");
            LOG_INFO << "request_id=" << requestId
                     << " method=" << req->methodString()
                     << " path=" << req->path()
                     << " status=" << static_cast<int>(resp->statusCode())
                     << " duration_ms=" << duration.count();
        });
}
