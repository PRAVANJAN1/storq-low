#include <storq/core/runtime.hpp>

#include <utility>

namespace storq {
namespace {

StorqError runtime_error(ErrorCode code, std::string message) {
    return {ErrorCategory::Runtime, code, std::move(message), {}};
}

} // namespace

Runtime::~Runtime() noexcept {
    static_cast<void>(shutdown());
}

Result<void> Runtime::initialize(const RuntimeConfig& config) {
    auto validation = validate_config(config);
    if (!validation) {
        return validation.error();
    }

    std::lock_guard lock(lifecycle_mutex_);
    if (state_.load(std::memory_order_acquire) != RuntimeState::Uninitialized) {
        return runtime_error(ErrorCode::InvalidState,
                             "Runtime initialization is only allowed once per Runtime instance.");
    }
    state_.store(RuntimeState::Initializing, std::memory_order_release);

    try {
        auto config_update = configuration_.update(config);
        if (!config_update) {
            state_.store(RuntimeState::Failed, std::memory_order_release);
            return config_update.error();
        }

        auto logger_result = Logger::create(config);
        if (!logger_result) {
            state_.store(RuntimeState::Failed, std::memory_order_release);
            return logger_result.error();
        }
        logger_ = std::move(logger_result).value();

        auto modules_result = modules_.initialize_all();
        if (!modules_result) {
            auto failure = modules_result.error();
            auto log_result =
                logger_->error("Runtime initialization failed while starting modules.");
            if (!log_result) {
                failure.metadata.emplace("logging_error", log_result.error().message);
            }
            logger_.reset();
            state_.store(RuntimeState::Failed, std::memory_order_release);
            return failure;
        }

        for (const auto capability : {"core_runtime", "structured_logging", "module_registry",
                                      "configuration"}) {
            static_cast<void>(capabilities_.register_capability(capability));
        }

        auto log_result = logger_->info("Storq runtime initialized successfully.");
        if (!log_result) {
            auto failure = log_result.error();
            auto rollback = modules_.shutdown_all();
            if (!rollback) {
                failure.metadata.emplace("rollback_error", rollback.error().message);
            }
            logger_.reset();
            state_.store(RuntimeState::Failed, std::memory_order_release);
            return failure;
        }
        state_.store(RuntimeState::Running, std::memory_order_release);
        return {};
    } catch (const std::exception& exception) {
        auto failure = runtime_error(
            ErrorCode::InitializationFailed,
            "Runtime initialization threw an exception: " + std::string(exception.what()));
        auto rollback = modules_.shutdown_all();
        if (!rollback) {
            failure.metadata.emplace("rollback_error", rollback.error().message);
        }
        logger_.reset();
        state_.store(RuntimeState::Failed, std::memory_order_release);
        return failure;
    } catch (...) {
        auto failure = runtime_error(
            ErrorCode::InitializationFailed, "Runtime initialization threw an unknown exception.");
        auto rollback = modules_.shutdown_all();
        if (!rollback) {
            failure.metadata.emplace("rollback_error", rollback.error().message);
        }
        logger_.reset();
        state_.store(RuntimeState::Failed, std::memory_order_release);
        return failure;
    }
}

Result<void> Runtime::shutdown() {
    std::lock_guard lock(lifecycle_mutex_);
    const auto current_state = state_.load(std::memory_order_acquire);
    if (current_state == RuntimeState::Uninitialized || current_state == RuntimeState::Stopped) {
        return {};
    }
    if (current_state == RuntimeState::Initializing ||
        current_state == RuntimeState::ShuttingDown) {
        return runtime_error(ErrorCode::InvalidState,
                             "Runtime lifecycle transition is already in progress.");
    }

    state_.store(RuntimeState::ShuttingDown, std::memory_order_release);
    try {
        auto result = modules_.shutdown_all();
        if (logger_) {
            auto log_result = logger_->info("Storq runtime shut down.");
            if (!log_result && result) {
                result = log_result.error();
            }
        }
        logger_.reset();
        state_.store(result ? RuntimeState::Stopped : RuntimeState::Failed,
                     std::memory_order_release);
        return result;
    } catch (const std::exception& exception) {
        logger_.reset();
        state_.store(RuntimeState::Failed, std::memory_order_release);
        return runtime_error(ErrorCode::ShutdownFailed,
                             "Runtime shutdown threw an exception: " +
                                 std::string(exception.what()));
    } catch (...) {
        logger_.reset();
        state_.store(RuntimeState::Failed, std::memory_order_release);
        return runtime_error(ErrorCode::ShutdownFailed,
                             "Runtime shutdown threw an unknown exception.");
    }
}

RuntimeState Runtime::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

RuntimeConfig Runtime::configuration() const {
    return configuration_.get();
}

std::shared_ptr<Logger> Runtime::logger() const {
    std::lock_guard lock(lifecycle_mutex_);
    return logger_;
}

ModuleRegistry& Runtime::modules() noexcept {
    return modules_;
}

const ModuleRegistry& Runtime::modules() const noexcept {
    return modules_;
}

CapabilityRegistry& Runtime::capabilities() noexcept {
    return capabilities_;
}

const CapabilityRegistry& Runtime::capabilities() const noexcept {
    return capabilities_;
}

Result<void> Runtime::set_log_level(LogLevel level) {
    std::lock_guard lock(lifecycle_mutex_);
    if (state_.load(std::memory_order_acquire) != RuntimeState::Running || !logger_) {
        return runtime_error(ErrorCode::InvalidState,
                             "The log level can only be changed while the runtime is running.");
    }
    auto config = configuration_.get();
    config.log_level = level;
    auto result = configuration_.update(std::move(config));
    if (!result) {
        return result.error();
    }
    logger_->set_level(level);
    return {};
}

std::string_view runtime_state_name(RuntimeState state) noexcept {
    switch (state) {
    case RuntimeState::Uninitialized: return "Uninitialized";
    case RuntimeState::Initializing: return "Initializing";
    case RuntimeState::Running: return "Running";
    case RuntimeState::ShuttingDown: return "ShuttingDown";
    case RuntimeState::Stopped: return "Stopped";
    case RuntimeState::Failed: return "Failed";
    }
    return "Unknown";
}

} // namespace storq
