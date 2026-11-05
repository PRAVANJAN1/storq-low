#pragma once

#include <cstddef>
#include <filesystem>
#include <shared_mutex>
#include <string>

#include <storq/common/result.hpp>

namespace storq {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off
};

enum class LogDestination {
    Console,
    File,
    ConsoleAndFile
};

struct ThreadConfig {
    std::size_t worker_threads{0};
};

struct RuntimeConfig {
    std::string application_name{"Storq"};
    std::string environment{"development"};
    LogLevel log_level{LogLevel::Info};
    LogDestination log_destination{LogDestination::Console};
    std::filesystem::path log_file_path;
    ThreadConfig threads;
    bool debug_mode{false};
    Metadata metadata;
};

[[nodiscard]] Result<void> validate_config(const RuntimeConfig& config);

class ConfigurationManager {
public:
    ConfigurationManager() = default;
    explicit ConfigurationManager(RuntimeConfig config);

    [[nodiscard]] RuntimeConfig get() const;
    [[nodiscard]] Result<void> update(RuntimeConfig config);

private:
    mutable std::shared_mutex mutex_;
    RuntimeConfig config_;
};

} // namespace storq
