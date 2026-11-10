#pragma once

#include <drogon/HttpController.h>
#include <functional>
#include <string>

class UserController : public drogon::HttpController<UserController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UserController::list,
                  "/admin/api/users",
                  drogon::Get,
                  "AuthFilter",
                  "RbacFilter");
    ADD_METHOD_TO(UserController::detail,
                  "/admin/api/users/{id}",
                  drogon::Get,
                  "AuthFilter",
                  "RbacFilter");
    ADD_METHOD_TO(UserController::updateStatus,
                  "/admin/api/users/{id}/status",
                  drogon::Patch,
                  "AuthFilter",
                  "RbacFilter");
    ADD_METHOD_TO(UserController::resetPassword,
                  "/admin/api/users/{id}/reset-password",
                  drogon::Post,
                  "AuthFilter",
                  "RbacFilter");
    ADD_METHOD_TO(UserController::kick,
                  "/admin/api/users/{id}/kick",
                  drogon::Post,
                  "AuthFilter",
                  "RbacFilter");
    METHOD_LIST_END

    void list(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void detail(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback,
        std::string id);

    void updateStatus(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback,
        std::string id);

    void resetPassword(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback,
        std::string id);

    void kick(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback,
        std::string id);
};
