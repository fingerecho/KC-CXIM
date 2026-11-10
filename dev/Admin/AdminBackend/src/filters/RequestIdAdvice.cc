#include "filters/RequestIdAdvice.h"

#include <drogon/drogon.h>

void registerRequestIdAdvice()
{
    drogon::app().registerPreRoutingAdvice(
        [](const drogon::HttpRequestPtr &req) {
            if (!req->attributes()->find("request_id"))
            {
                req->attributes()->insert("request_id",
                                          drogon::utils::getUuid());
            }
        });

    drogon::app().registerPreSendingAdvice(
        [](const drogon::HttpRequestPtr &req,
           const drogon::HttpResponsePtr &resp) {
            if (!resp)
            {
                return;
            }
            const auto &requestId =
                req->attributes()->get<std::string>("request_id");
            if (!requestId.empty())
            {
                resp->addHeader("X-Request-Id", requestId);
            }
        });
}
