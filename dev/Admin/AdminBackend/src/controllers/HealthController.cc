#include "controllers/HealthController.h"

#include <nlohmann/json.hpp>
#include <drogon/drogon.h>

#include <memory>
#include <utility>
#include "common/AppError.h"
#include "common/ParamUtil.h"
#include "common/Responder.h"

void HealthController::health(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        responder->ok(nlohmann::json{{"status", "up"}},
                      ParamUtil::requestId(req));
    }
    catch (const AppError &e)
    {
        responder->fail(e);
    }
    catch (const std::exception &e)
    {
        responder->failUnknown(e);
    }
    catch (...)
    {
        responder->failUnknown();
    }
}
