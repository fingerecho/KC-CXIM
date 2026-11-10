#pragma once

#include <utility>
#include <variant>
#include "common/AppError.h"

template <typename T>
class Result
{
  public:
    Result(T value) : data_(std::move(value)) {}
    Result(AppError error) : data_(std::move(error)) {}

    bool ok() const noexcept { return std::holds_alternative<T>(data_); }

    const T &value() const { return std::get<T>(data_); }
    T &value() { return std::get<T>(data_); }

    const AppError &error() const { return std::get<AppError>(data_); }

  private:
    std::variant<T, AppError> data_;
};
