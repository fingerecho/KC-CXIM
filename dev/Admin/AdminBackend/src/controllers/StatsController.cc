#include "controllers/StatsController.h"

#include <drogon/drogon.h>

#include <memory>
#include <utility>
#include "common/AppContext.h"
#include "common/AppError.h"
#include "common/ParamUtil.h"
#include "common/Responder.h"
#include "models/ModelJson.h"
#include "services/StatsService.h"

void StatsController::overview(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const std::string requestId = ParamUtil::requestId(req);
        auto service = AppContext::instance().statsService();
        service->overview(
            [responder, requestId](Result<StatsOverview> result) mutable {
                try
                {
                    if (!result.ok())
                    {
                        responder->fail(result.error());
                        return;
                    }
                    responder->ok(toJson(result.value()), requestId);
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
            });
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

void StatsController::trend(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const int days =
            ParamUtil::parseIntParam(req, "days", 7, 1, 365);
        const std::string requestId = ParamUtil::requestId(req);

        auto service = AppContext::instance().statsService();
        service->trend(
            days,
            [responder, requestId](Result<TrendData> result) mutable {
                try
                {
                    if (!result.ok())
                    {
                        responder->fail(result.error());
                        return;
                    }
                    responder->ok(toJson(result.value()), requestId);
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
            });
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
