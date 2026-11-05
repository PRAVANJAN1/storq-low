#include <storq/core/logger.hpp>

#include <iostream>
#include <exception>
#include <utility>

#include <storq/common/utilities.hpp>

namespace storq {
namespace {

StorqError logger_error(ErrorCode code, std::string message) {
    return {ErrorCategory::Filesystem, code, std::move(message), {}};
}

} // namespace

Logger::Logger(const RuntimeConfig& config)
    : level_(config.log_level), destination_(config.log_destination) {
    if (destination_ == LogDestination::File ||
        destination_ == LogDestination::ConsoleAndFile) {
        file_.open(config.log_file_path, std::ios::out | std::ios::app);
    }
}

Result<std::shared_ptr<Logger>> Logger::create(const RuntimeConfig& config) {
    auto validation = validate_config(config);
    if (!validation) {
        return validation.error();
    }

    auto logger = std::shared_ptr<Logger>(new Logger(config));
    if ((config.log_destination == LogDestination::File ||
         config.log_destination == LogDestination::ConsoleAndFile) &&
        !logger->file_.is_open()) {
        return logger_error(ErrorCode::IoError,
                            "Unable to open log file: " + config.log_file_path.string());
    }
    return logger;
}

void Logger::set_level(LogLevel level) noexcept {
    level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept {
    return level_.load(std::memory_order_relaxed);
}

Result<void> Logger::log(
    LogLevel message_level, std::string_view message, std::string_view component) {
    const auto minimum_level = level();
    if (minimum_level == LogLevel::Off || message_level == LogLevel::Off ||
        static_cast<int>(message_level) < static_cast<int>(minimum_level)) {
        return {};
    }

    std::lock_guard lock(mutex_);
    try {
        const std::string entry = "[" + format_timestamp() + "]\n[" +
            std::string(log_level_name(message_level)) + "]\n[" + std::string(component) +
            "]\n[Thread: " + current_thread_id() + "]\n" + std::string(message) + '\n';

        if (destination_ == LogDestination::Console ||
            destination_ == LogDestination::ConsoleAndFile) {
            std::clog << entry;
            if (!std::clog) {
                return logger_error(ErrorCode::IoError, "Unable to write to the console log.");
            }
        }
        if (destination_ == LogDestination::File ||
            destination_ == LogDestination::ConsoleAndFile) {
            file_ << entry;
            file_.flush();
            if (!file_) {
                return logger_error(ErrorCode::IoError, "Unable to write to the log file.");
            }
        }
    } catch (const std::exception& exception) {
        return logger_error(ErrorCode::IoError,
                            "Log output failed: " + std::string(exception.what()));
    } catch (...) {
        return logger_error(ErrorCode::IoError, "Log output failed with an unknown exception.");
    }
    return {};
}

Result<void> Logger::trace(std::string_view message) {
    return log(LogLevel::Trace, message);
}

Result<void> Logger::debug(std::string_view message) {
    return log(LogLevel::Debug, message);
}

Result<void> Logger::info(std::string_view message) {
    return log(LogLevel::Info, message);
}

Result<void> Logger::warning(std::string_view message) {
    return log(LogLevel::Warning, message);
}

Result<void> Logger::error(std::string_view message) {
    return log(LogLevel::Error, message);
}

Result<void> Logger::critical(std::string_view message) {
    return log(LogLevel::Critical, message);
}

std::string_view log_level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Critical: return "CRITICAL";
    case LogLevel::Off: return "OFF";
    }
    return "UNKNOWN";
}

} // namespace storq
