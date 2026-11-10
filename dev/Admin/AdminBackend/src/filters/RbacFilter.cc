#include "filters/RbacFilter.h"

#include <drogon/drogon.h>

#include <string>
#include "common/ApiResponse.h"
#include "common/AppError.h"

namespace
{
bool startsWith(const std::string &value, const char *prefix)
{
    return value.rfind(prefix, 0) == 0;
}
}  // namespace

void RbacFilter::doFilter(const drogon::HttpRequestPtr &req,
                          drogon::FilterCallback &&fcb,
                          drogon::FilterChainCallback &&fccb)
{
    try
    {
        const std::string role =
            req->attributes()->get<std::string>("admin_role");
        if (role.empty())
        {
            fcb(makeError(AppError::unauthorized()));
            return;
        }

        const std::string &path = req->path();
        const auto method = req->method();
        bool allowed = true;

        if (startsWith(path, "/admin/api/admins") ||
            startsWith(path, "/admin/api/roles"))
        {
            allowed = role == "super_admin";
        }
        else if (startsWith(path, "/admin/api/audit-logs"))
        {
            allowed = role == "super_admin" || role == "auditor";
        }
        else if (startsWith(path, "/admin/api/configs"))
        {
            allowed = role == "super_admin" || role == "sys_admin";
        }
        else if (startsWith(path, "/admin/api/push"))
        {
            allowed = role == "super_admin" || role == "sys_admin" ||
                      role == "operator";
        }
        else if (startsWith(path, "/admin/api/moderation"))
        {
            allowed = role == "super_admin" || role == "sys_admin" ||
                      role == "moderator";
        }
        else if (startsWith(path, "/admin/api/users") &&
                 method != drogon::Get)
        {
            allowed = role == "super_admin" || role == "sys_admin" ||
                      role == "support";
        }

        if (!allowed)
        {
            fcb(makeError(AppError::forbidden()));
            return;
        }

        fccb();
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "rbac filter failed: " << e.what();
        fcb(makeError(AppError::internal()));
    }
    catch (...)
    {
        LOG_ERROR << "rbac filter failed with unknown exception";
        fcb(makeError(AppError::internal()));
    }
}
