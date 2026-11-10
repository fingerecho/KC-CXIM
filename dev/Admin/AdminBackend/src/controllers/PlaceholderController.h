#pragma once

#include <drogon/HttpController.h>
#include <functional>

#define CX_PLACEHOLDER_ROUTES(path)                                      \
    ADD_METHOD_TO(PlaceholderController::notImplemented,                 \
                  path,                                                  \
                  drogon::Get,                                           \
                  "AuthFilter",                                          \
                  "RbacFilter");                                         \
    ADD_METHOD_TO(PlaceholderController::notImplemented,                 \
                  path,                                                  \
                  drogon::Post,                                          \
                  "AuthFilter",                                          \
                  "RbacFilter");                                         \
    ADD_METHOD_TO(PlaceholderController::notImplemented,                 \
                  path,                                                  \
                  drogon::Patch,                                         \
                  "AuthFilter",                                          \
                  "RbacFilter");                                         \
    ADD_METHOD_TO(PlaceholderController::notImplemented,                 \
                  path,                                                  \
                  drogon::Put,                                           \
                  "AuthFilter",                                          \
                  "RbacFilter");                                         \
    ADD_METHOD_TO(PlaceholderController::notImplemented,                 \
                  path,                                                  \
                  drogon::Delete,                                        \
                  "AuthFilter",                                          \
                  "RbacFilter");

class PlaceholderController
    : public drogon::HttpController<PlaceholderController>
{
  public:
    METHOD_LIST_BEGIN
    CX_PLACEHOLDER_ROUTES("/admin/api/friends")
    CX_PLACEHOLDER_ROUTES("/admin/api/groups")
    CX_PLACEHOLDER_ROUTES("/admin/api/messages")
    CX_PLACEHOLDER_ROUTES("/admin/api/moderation")
    CX_PLACEHOLDER_ROUTES("/admin/api/push")
    CX_PLACEHOLDER_ROUTES("/admin/api/files")
    CX_PLACEHOLDER_ROUTES("/admin/api/configs")
    CX_PLACEHOLDER_ROUTES("/admin/api/audit-logs")
    CX_PLACEHOLDER_ROUTES("/admin/api/admins")
    CX_PLACEHOLDER_ROUTES("/admin/api/roles")
    METHOD_LIST_END

    void notImplemented(
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};

#undef CX_PLACEHOLDER_ROUTES
