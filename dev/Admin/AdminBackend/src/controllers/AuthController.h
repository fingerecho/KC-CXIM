#pragma once

#include <drogon/HttpController.h>
#include <functional>

class AuthController : public drogon::HttpController<AuthController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthController::login,
                  "/admin/api/auth/login",
                  drogon::Post);
    ADD_METHOD_TO(AuthController::refresh,
                  "/admin/api/auth/refresh",
                  drogon::Post);
    ADD_METHOD_TO(AuthController::logout,
                  "/admin/api/auth/logout",
                  drogon::Post,
                  "AuthFilter",
                  "RbacFilter");
    ADD_METHOD_TO(AuthController::me,
                  "/admin/api/auth/me",
                  drogon::Get,
                  "AuthFilter",
                  "RbacFilter");
    METHOD_LIST_END

    void login(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void refresh(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void logout(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void me(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};
