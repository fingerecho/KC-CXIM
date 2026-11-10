#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

template <typename... Args>
class OnceCallback
{
  public:
    using Callback = std::function<void(Args...)>;

    explicit OnceCallback(Callback cb)
        : cb_(std::move(cb)), done_(std::make_shared<std::atomic_bool>(false))
    {
    }

    void operator()(Args... args)
    {
        if (done_->exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        if (cb_)
        {
            cb_(std::forward<Args>(args)...);
        }
    }

    bool called() const noexcept
    {
        return done_->load(std::memory_order_acquire);
    }

  private:
    Callback cb_;
    std::shared_ptr<std::atomic_bool> done_;
};
