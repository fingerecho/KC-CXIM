#include "controllers/PlaceholderController.h"

#include <drogon/drogon.h>

#include <memory>
#include <utility>
#include "common/AppError.h"
#include "common/Responder.h"

void PlaceholderController::notImplemented(
    const drogon::HttpRequestPtr &,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        responder->fail(AppError::notImplemented());
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
