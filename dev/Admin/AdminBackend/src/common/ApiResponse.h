#pragma once

#include <nlohmann/json.hpp>
#include <drogon/HttpResponse.h>
#include <utility>
#include "common/AppError.h"

inline drogon::HttpResponsePtr makeJsonResponse(
    drogon::HttpStatusCode status,
    const nlohmann::json &body)
{
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(status);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    resp->addHeader("X-Content-Type-Options", "nosniff");
    resp->addHeader("X-Frame-Options", "DENY");
    resp->addHeader("Referrer-Policy", "no-referrer");
    return resp;
}

inline drogon::HttpResponsePtr makeSuccess(
    nlohmann::json data,
    std::string requestId = {})
{
    nlohmann::json body{
        {"code", 0},
        {"message", "ok"},
        {"data", std::move(data)}};
    if (!requestId.empty())
    {
        body["request_id"] = std::move(requestId);
    }
    return makeJsonResponse(drogon::k200OK, body);
}

inline drogon::HttpResponsePtr makeError(const AppError &e)
{
    nlohmann::json body{
        {"code", e.code()},
        {"field", e.field()},
        {"message", e.message()}};
    return makeJsonResponse(e.status(), body);
}
