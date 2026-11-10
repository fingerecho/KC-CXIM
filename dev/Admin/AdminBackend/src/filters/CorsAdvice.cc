#include "filters/CorsAdvice.h"

#include <drogon/drogon.h>

#include <utility>

void registerCorsAdvice(std::string origin)
{
    drogon::app().registerPreRoutingAdvice(
        [origin](const drogon::HttpRequestPtr &req,
                 drogon::AdviceCallback &&acb,
                 drogon::AdviceChainCallback &&accb) {
            if (req->method() == drogon::Options)
            {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k204NoContent);
                acb(resp);
                return;
            }
            accb();
        });

    drogon::app().registerPreSendingAdvice(
        [origin](const drogon::HttpRequestPtr &,
                 const drogon::HttpResponsePtr &resp) {
            if (!resp)
            {
                return;
            }
            resp->addHeader("Access-Control-Allow-Origin", origin);
            resp->addHeader("Access-Control-Allow-Methods",
                            "GET, POST, PATCH, PUT, DELETE, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers",
                            "Authorization, Content-Type, X-Request-Id");
            resp->addHeader("Access-Control-Expose-Headers",
                            "X-Request-Id");
            resp->addHeader("Access-Control-Max-Age", "600");
            resp->addHeader("Vary", "Origin");
        });
}
