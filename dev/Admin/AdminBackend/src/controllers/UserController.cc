#include "controllers/UserController.h"

#include <nlohmann/json.hpp>
#include <drogon/drogon.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include "common/AppContext.h"
#include "common/AppError.h"
#include "common/ParamUtil.h"
#include "common/Responder.h"
#include "models/ModelJson.h"
#include "models/UserSummary.h"
#include "services/UserService.h"

void UserController::list(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const int page =
            ParamUtil::parseIntParam(req, "page", 1, 1, 1000000);
        const int pageSize =
            ParamUtil::parseIntParam(req, "page_size", 20, 1, 100);
        const std::string keyword = req->getParameter("keyword");
        std::string status = req->getParameter("status");
        if (status.empty())
        {
            status = "all";
        }
        const std::string requestId = ParamUtil::requestId(req);

        auto service = AppContext::instance().userService();
        service->listUsers(
            page,
            pageSize,
            keyword,
            status,
            [responder, requestId](Result<UserPage> result) mutable {
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

void UserController::detail(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
    std::string id)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const std::uint64_t userId = ParamUtil::parseUint64(id, "id");
        const std::string requestId = ParamUtil::requestId(req);

        auto service = AppContext::instance().userService();
        service->getUser(
            userId,
            [responder, requestId](Result<UserSummary> result) mutable {
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

void UserController::updateStatus(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
    std::string id)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const std::uint64_t userId = ParamUtil::parseUint64(id, "id");
        const auto json = ParamUtil::parseJsonBody(req);
        if (!json.contains("status") || !json["status"].is_string())
        {
            throw AppError::invalidParam("status", "status 不能为空");
        }
        const std::string status = json["status"].get<std::string>();
        const auto adminId = ParamUtil::adminId(req);
        const std::string adminName = ParamUtil::adminUsername(req);
        const std::string ip =
            ParamUtil::truncate(ParamUtil::clientIp(req), 64);
        const std::string userAgent = ParamUtil::userAgent(req);
        const std::string requestId = ParamUtil::requestId(req);

        auto service = AppContext::instance().userService();
        service->updateStatus(
            static_cast<std::uint64_t>(adminId),
            adminName,
            userId,
            status,
            ip,
            userAgent,
            requestId,
            [responder, requestId](
                Result<UserStatusResult> result) mutable {
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

void UserController::resetPassword(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
    std::string id)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const std::uint64_t userId = ParamUtil::parseUint64(id, "id");
        const auto adminId = ParamUtil::adminId(req);
        const std::string adminName = ParamUtil::adminUsername(req);
        const std::string ip =
            ParamUtil::truncate(ParamUtil::clientIp(req), 64);
        const std::string userAgent = ParamUtil::userAgent(req);
        const std::string requestId = ParamUtil::requestId(req);

        auto service = AppContext::instance().userService();
        service->resetPassword(
            static_cast<std::uint64_t>(adminId),
            adminName,
            userId,
            ip,
            userAgent,
            requestId,
            [responder, requestId](Result<bool> result) mutable {
                try
                {
                    if (!result.ok())
                    {
                        responder->fail(result.error());
                        return;
                    }
                    responder->ok(nlohmann::json{{"reset", true}},
                                  requestId);
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

void UserController::kick(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
    std::string id)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const std::uint64_t userId = ParamUtil::parseUint64(id, "id");
        const auto adminId = ParamUtil::adminId(req);
        const std::string adminName = ParamUtil::adminUsername(req);
        const std::string ip =
            ParamUtil::truncate(ParamUtil::clientIp(req), 64);
        const std::string userAgent = ParamUtil::userAgent(req);
        const std::string requestId = ParamUtil::requestId(req);

        auto service = AppContext::instance().userService();
        service->kick(
            static_cast<std::uint64_t>(adminId),
            adminName,
            userId,
            ip,
            userAgent,
            requestId,
            [responder, requestId](Result<bool> result) mutable {
                try
                {
                    if (!result.ok())
                    {
                        responder->fail(result.error());
                        return;
                    }
                    responder->ok(nlohmann::json{{"kicked", true}},
                                  requestId);
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
