#pragma once

#include <drogon/HttpController.h>
#include <functional>

class StatsController : public drogon::HttpController<StatsController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(StatsController::overview,
                  "/admin/api/stats/overview",
                  drogon::Get,
                  "AuthFilter",
                  "RbacFilter");
    ADD_METHOD_TO(StatsController::trend,
                  "/admin/api/stats/trend",
                  drogon::Get,
                  "AuthFilter",
                  "RbacFilter");
    METHOD_LIST_END

    void overview(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void trend(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};
