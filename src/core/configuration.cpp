#include <storq/core/configuration.hpp>

#include <mutex>
#include <shared_mutex>

namespace storq {
namespace {

StorqError configuration_error(ErrorCode code, std::string message) {
    return {ErrorCategory::Configuration, code, std::move(message), {}};
}

bool valid_log_level(LogLevel level) {
    return static_cast<int>(level) >= static_cast<int>(LogLevel::Trace) &&
           static_cast<int>(level) <= static_cast<int>(LogLevel::Off);
}

bool valid_log_destination(LogDestination destination) {
    return destination == LogDestination::Console || destination == LogDestination::File ||
           destination == LogDestination::ConsoleAndFile;
}

} // namespace

Result<void> validate_config(const RuntimeConfig& config) {
    if (config.application_name.empty()) {
        return configuration_error(ErrorCode::InvalidArgument,
                                   "Application name must not be empty.");
    }
    if (config.environment.empty()) {
        return configuration_error(ErrorCode::InvalidArgument,
                                   "Runtime environment must not be empty.");
    }
    if (!valid_log_level(config.log_level)) {
        return configuration_error(ErrorCode::InvalidArgument, "Log level is invalid.");
    }
    if (!valid_log_destination(config.log_destination)) {
        return configuration_error(ErrorCode::InvalidArgument,
                                   "Log output destination is invalid.");
    }
    if (config.threads.worker_threads > 1024) {
        return configuration_error(ErrorCode::InvalidArgument,
                                   "Worker thread count must not exceed 1024.");
    }
    if ((config.log_destination == LogDestination::File ||
         config.log_destination == LogDestination::ConsoleAndFile) &&
        config.log_file_path.empty()) {
        return configuration_error(ErrorCode::InvalidArgument,
                                   "A log file path is required for file logging.");
    }
    return {};
}

ConfigurationManager::ConfigurationManager(RuntimeConfig config) {
    auto validation = validate_config(config);
    if (!validation) {
        throw StorqException(validation.error());
    }
    config_ = std::move(config);
}

RuntimeConfig ConfigurationManager::get() const {
    std::shared_lock lock(mutex_);
    return config_;
}

Result<void> ConfigurationManager::update(RuntimeConfig config) {
    auto validation = validate_config(config);
    if (!validation) {
        return validation.error();
    }
    std::unique_lock lock(mutex_);
    config_ = std::move(config);
    return {};
}

} // namespace storq
