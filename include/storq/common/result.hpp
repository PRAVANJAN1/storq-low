#pragma once

#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include <storq/core/error.hpp>

namespace storq {

template <typename T>
class Result {
    static_assert(!std::is_reference_v<T>);

public:
    Result(T value) : value_(std::in_place_index<0>, std::move(value)) {}
    Result(StorqError error) : value_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] static Result success(T value) {
        return Result(std::move(value));
    }

    [[nodiscard]] static Result failure(StorqError error) {
        return Result(std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(value_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    T& value() & {
        if (!has_value()) {
            throw StorqException(std::get<StorqError>(value_));
        }
        return std::get<T>(value_);
    }

    const T& value() const& {
        if (!has_value()) {
            throw StorqException(std::get<StorqError>(value_));
        }
        return std::get<T>(value_);
    }

    T&& value() && {
        if (!has_value()) {
            throw StorqException(std::get<StorqError>(value_));
        }
        return std::get<T>(std::move(value_));
    }

    [[nodiscard]] StorqError& error() & {
        if (has_value()) {
            throw std::logic_error("A successful result has no error.");
        }
        return std::get<StorqError>(value_);
    }

    [[nodiscard]] const StorqError& error() const& {
        if (has_value()) {
            throw std::logic_error("A successful result has no error.");
        }
        return std::get<StorqError>(value_);
    }

    [[nodiscard]] StorqError&& error() && {
        if (has_value()) {
            throw std::logic_error("A successful result has no error.");
        }
        return std::get<StorqError>(std::move(value_));
    }

private:
    std::variant<T, StorqError> value_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(StorqError error) : error_(std::move(error)) {}

    [[nodiscard]] static Result success() {
        return Result();
    }

    [[nodiscard]] static Result failure(StorqError error) {
        return Result(std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    void value() const {
        if (error_) {
            throw StorqException(*error_);
        }
    }

    [[nodiscard]] StorqError& error() & {
        if (!error_) {
            throw std::logic_error("A successful result has no error.");
        }
        return *error_;
    }

    [[nodiscard]] const StorqError& error() const& {
        if (!error_) {
            throw std::logic_error("A successful result has no error.");
        }
        return *error_;
    }

private:
    std::optional<StorqError> error_;
};

} // namespace storq
