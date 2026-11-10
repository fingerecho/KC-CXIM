#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <nlohmann/json.hpp>
#include <drogon/drogon.h>
#include "common/ApiResponse.h"
#include "common/AppError.h"

class Responder final
{
  public:
    using Callback = std::function<void(const drogon::HttpResponsePtr &)>;

    explicit Responder(Callback cb) : cb_(std::move(cb)) {}

    void ok(nlohmann::json data, std::string requestId = {})
    {
        if (done_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        cb_(makeSuccess(std::move(data), std::move(requestId)));
    }

    void fail(const AppError &e)
    {
        if (done_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        cb_(makeError(e));
    }

    void failUnknown(const std::exception &e)
    {
        if (done_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        LOG_ERROR << "unhandled exception: " << e.what();
        cb_(makeError(AppError::internal()));
    }

    void failUnknown()
    {
        if (done_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        LOG_ERROR << "unhandled unknown exception";
        cb_(makeError(AppError::internal()));
    }

  private:
    std::atomic_bool done_{false};
    Callback cb_;
};
