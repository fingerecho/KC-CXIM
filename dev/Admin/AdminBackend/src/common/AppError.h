#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <drogon/HttpTypes.h>

class AppError final : public std::runtime_error
{
  public:
    AppError(std::string code,
             std::string field,
             std::string message,
             drogon::HttpStatusCode status)
        : std::runtime_error(message),
          code_(std::move(code)),
          field_(std::move(field)),
          message_(std::move(message)),
          status_(status)
    {
    }

    const std::string &code() const noexcept { return code_; }
    const std::string &field() const noexcept { return field_; }
    const std::string &message() const noexcept { return message_; }
    drogon::HttpStatusCode status() const noexcept { return status_; }

    static AppError invalidParam(std::string field, std::string message)
    {
        return {"INVALID_PARAM",
                std::move(field),
                std::move(message),
                drogon::k400BadRequest};
    }

    static AppError invalidBody()
    {
        return {"INVALID_BODY",
                "",
                "请求体必须是合法的 JSON 对象",
                drogon::k400BadRequest};
    }

    static AppError unauthorized(std::string message = "未登录或登录已过期")
    {
        return {"UNAUTHORIZED",
                "",
                std::move(message),
                drogon::k401Unauthorized};
    }

    static AppError tokenExpired()
    {
        return {"TOKEN_EXPIRED",
                "",
                "登录已过期，请重新登录",
                drogon::k401Unauthorized};
    }

    static AppError invalidCredentials()
    {
        return {"INVALID_CREDENTIALS",
                "password",
                "账号或密码错误",
                drogon::k401Unauthorized};
    }

    static AppError forbidden(std::string message = "无权限执行该操作")
    {
        return {"FORBIDDEN", "", std::move(message), drogon::k403Forbidden};
    }

    static AppError notFound(std::string field, std::string message)
    {
        return {"NOT_FOUND",
                std::move(field),
                std::move(message),
                drogon::k404NotFound};
    }

    static AppError conflict(std::string field, std::string message)
    {
        return {"CONFLICT",
                std::move(field),
                std::move(message),
                drogon::k409Conflict};
    }

    static AppError internal()
    {
        return {"INTERNAL_ERROR",
                "",
                "服务器内部错误，请稍后重试",
                drogon::k500InternalServerError};
    }

    static AppError notImplemented()
    {
        return {"NOT_IMPLEMENTED",
                "",
                "该功能尚未实现",
                drogon::k501NotImplemented};
    }

  private:
    std::string code_;
    std::string field_;
    std::string message_;
    drogon::HttpStatusCode status_;
};
