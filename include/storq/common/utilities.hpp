#pragma once

#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace storq {

[[nodiscard]] std::string generate_uuid();
[[nodiscard]] std::string format_timestamp(
    std::chrono::system_clock::time_point time = std::chrono::system_clock::now());
[[nodiscard]] std::string current_thread_id();
[[nodiscard]] std::string trim(std::string_view value);
[[nodiscard]] std::string to_lower(std::string_view value);

template <typename Function>
class ScopeExit {
    static_assert(std::is_invocable_v<Function&>);

public:
    explicit ScopeExit(Function function) noexcept(std::is_nothrow_move_constructible_v<Function>)
        : function_(std::move(function)) {}

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    ScopeExit(ScopeExit&& other) noexcept(std::is_nothrow_move_constructible_v<Function>)
        : function_(std::move(other.function_)), active_(std::exchange(other.active_, false)) {}

    ~ScopeExit() noexcept {
        if (active_) {
            try {
                function_();
            } catch (...) {
                std::terminate();
            }
        }
    }

    void release() noexcept {
        active_ = false;
    }

private:
    Function function_;
    bool active_{true};
};

template <typename Function>
ScopeExit(Function) -> ScopeExit<Function>;

template <typename Function>
[[nodiscard]] auto make_scope_exit(Function&& function) {
    return ScopeExit<std::decay_t<Function>>(std::forward<Function>(function));
}

} // namespace storq
