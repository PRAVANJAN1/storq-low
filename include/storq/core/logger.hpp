#pragma once

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <storq/core/configuration.hpp>

namespace storq {

class Logger {
public:
    [[nodiscard]] static Result<std::shared_ptr<Logger>> create(const RuntimeConfig& config);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void set_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;

    [[nodiscard]] Result<void> log(
        LogLevel level, std::string_view message, std::string_view component = "Storq");
    [[nodiscard]] Result<void> trace(std::string_view message);
    [[nodiscard]] Result<void> debug(std::string_view message);
    [[nodiscard]] Result<void> info(std::string_view message);
    [[nodiscard]] Result<void> warning(std::string_view message);
    [[nodiscard]] Result<void> error(std::string_view message);
    [[nodiscard]] Result<void> critical(std::string_view message);

private:
    explicit Logger(const RuntimeConfig& config);

    std::atomic<LogLevel> level_;
    LogDestination destination_;
    std::mutex mutex_;
    std::ofstream file_;
};

[[nodiscard]] std::string_view log_level_name(LogLevel level) noexcept;

} // namespace storq
