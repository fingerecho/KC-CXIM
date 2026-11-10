#include "controllers/AuthController.h"

#include <nlohmann/json.hpp>
#include <drogon/drogon.h>

#include <memory>
#include <string>
#include <utility>
#include "common/AppContext.h"
#include "common/AppError.h"
#include "common/ParamUtil.h"
#include "common/Responder.h"
#include "models/AdminUser.h"
#include "models/ModelJson.h"
#include "services/AuthService.h"

void AuthController::login(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const auto json = ParamUtil::parseJsonBody(req);
        if (!json.contains("username") || !json["username"].is_string())
        {
            throw AppError::invalidParam("username", "用户名不能为空");
        }
        if (!json.contains("password") || !json["password"].is_string())
        {
            throw AppError::invalidParam("password", "密码不能为空");
        }

        const std::string username =
            json["username"].get<std::string>();
        const std::string password =
            json["password"].get<std::string>();
        const std::string ip =
            ParamUtil::truncate(ParamUtil::clientIp(req), 64);
        const std::string userAgent = ParamUtil::userAgent(req);
        const std::string requestId = ParamUtil::requestId(req);

        auto service = AppContext::instance().authService();
        service->login(
            username,
            password,
            ip,
            userAgent,
            [responder, requestId](Result<LoginResult> result) mutable {
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

void AuthController::refresh(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const auto json = ParamUtil::parseJsonBody(req);
        if (!json.contains("refreshToken") ||
            !json["refreshToken"].is_string())
        {
            throw AppError::invalidParam("refreshToken",
                                         "refreshToken 不能为空");
        }

        const std::string refreshToken =
            json["refreshToken"].get<std::string>();
        const std::string ip =
            ParamUtil::truncate(ParamUtil::clientIp(req), 64);
        const std::string userAgent = ParamUtil::userAgent(req);
        const std::string requestId = ParamUtil::requestId(req);

        auto service = AppContext::instance().authService();
        service->refresh(
            refreshToken,
            ip,
            userAgent,
            [responder, requestId](Result<LoginResult> result) mutable {
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

void AuthController::logout(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const auto json = ParamUtil::parseOptionalJsonBody(req);
        std::string refreshToken;
        if (json.contains("refreshToken") &&
            json["refreshToken"].is_string())
        {
            refreshToken = json["refreshToken"].get<std::string>();
        }

        const auto adminId = ParamUtil::adminId(req);
        const std::string requestId = ParamUtil::requestId(req);

        auto service = AppContext::instance().authService();
        service->logout(
            refreshToken,
            static_cast<std::uint64_t>(adminId),
            [responder, requestId](Result<bool> result) mutable {
                try
                {
                    if (!result.ok())
                    {
                        responder->fail(result.error());
                        return;
                    }
                    responder->ok(nlohmann::json{{"loggedOut", true}},
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

void AuthController::me(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback)
{
    auto responder = std::make_shared<Responder>(std::move(callback));

    try
    {
        const auto adminId = ParamUtil::adminId(req);
        const std::string requestId = ParamUtil::requestId(req);

        auto service = AppContext::instance().authService();
        service->me(
            static_cast<std::uint64_t>(adminId),
            [responder, requestId](Result<AdminProfile> result) mutable {
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
