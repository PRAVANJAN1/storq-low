#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include <storq/core/capabilities.hpp>
#include <storq/core/configuration.hpp>
#include <storq/core/logger.hpp>
#include <storq/core/module_registry.hpp>

namespace storq {

enum class RuntimeState {
    Uninitialized,
    Initializing,
    Running,
    ShuttingDown,
    Stopped,
    Failed
};

class Runtime {
public:
    Runtime() = default;
    ~Runtime() noexcept;

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] Result<void> initialize(const RuntimeConfig& config = {});
    [[nodiscard]] Result<void> shutdown();

    [[nodiscard]] RuntimeState state() const noexcept;
    [[nodiscard]] RuntimeConfig configuration() const;
    [[nodiscard]] std::shared_ptr<Logger> logger() const;
    [[nodiscard]] ModuleRegistry& modules() noexcept;
    [[nodiscard]] const ModuleRegistry& modules() const noexcept;
    [[nodiscard]] CapabilityRegistry& capabilities() noexcept;
    [[nodiscard]] const CapabilityRegistry& capabilities() const noexcept;
    [[nodiscard]] Result<void> set_log_level(LogLevel level);

private:
    mutable std::mutex lifecycle_mutex_;
    std::atomic<RuntimeState> state_{RuntimeState::Uninitialized};
    ConfigurationManager configuration_;
    std::shared_ptr<Logger> logger_;
    ModuleRegistry modules_;
    CapabilityRegistry capabilities_;
};

[[nodiscard]] std::string_view runtime_state_name(RuntimeState state) noexcept;

} // namespace storq
