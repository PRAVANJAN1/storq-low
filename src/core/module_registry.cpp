#include <storq/core/module_registry.hpp>

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace storq {
namespace {

StorqError module_error(ErrorCode code, std::string message) {
    return {ErrorCategory::Module, code, std::move(message), {}};
}

Result<void> initialize_module(const std::shared_ptr<IModule>& module) {
    try {
        return module->initialize();
    } catch (const std::exception& exception) {
        return module_error(ErrorCode::InitializationFailed,
                            "Module initialization threw an exception: " +
                                std::string(exception.what()));
    } catch (...) {
        return module_error(ErrorCode::InitializationFailed,
                            "Module initialization threw an unknown exception.");
    }
}

Result<void> shutdown_module(const std::shared_ptr<IModule>& module) {
    try {
        return module->shutdown();
    } catch (const std::exception& exception) {
        return module_error(ErrorCode::ShutdownFailed,
                            "Module shutdown threw an exception: " +
                                std::string(exception.what()));
    } catch (...) {
        return module_error(ErrorCode::ShutdownFailed,
                            "Module shutdown threw an unknown exception.");
    }
}

} // namespace

Result<void> ModuleRegistry::register_module(std::shared_ptr<IModule> module) {
    if (!module || module->name().empty()) {
        return module_error(ErrorCode::InvalidArgument,
                            "A registered module must have a non-empty name.");
    }

    std::lock_guard operation_lock(operation_mutex_);
    std::unique_lock lock(mutex_);
    if (initialized_) {
        return module_error(ErrorCode::InvalidState,
                            "Modules cannot be registered while the registry is initialized.");
    }
    const auto duplicate = std::find_if(modules_.begin(), modules_.end(),
        [&module](const auto& existing) { return existing->name() == module->name(); });
    if (duplicate != modules_.end()) {
        return module_error(ErrorCode::DuplicateRegistration,
                            "A module with that name is already registered.");
    }
    modules_.push_back(std::move(module));
    return {};
}

Result<void> ModuleRegistry::unregister_module(std::string_view name) {
    std::lock_guard operation_lock(operation_mutex_);
    std::unique_lock lock(mutex_);
    if (initialized_) {
        return module_error(ErrorCode::InvalidState,
                            "Modules cannot be removed while the registry is initialized.");
    }
    const auto module = std::find_if(modules_.begin(), modules_.end(),
        [name](const auto& item) { return item->name() == name; });
    if (module == modules_.end()) {
        return module_error(ErrorCode::NotFound, "The requested module is not registered.");
    }
    modules_.erase(module);
    return {};
}

Result<std::shared_ptr<IModule>> ModuleRegistry::find(std::string_view name) const {
    std::shared_lock lock(mutex_);
    const auto module = std::find_if(modules_.begin(), modules_.end(),
        [name](const auto& item) { return item->name() == name; });
    if (module == modules_.end()) {
        return module_error(ErrorCode::NotFound, "The requested module is not registered.");
    }
    return *module;
}

std::vector<std::shared_ptr<IModule>> ModuleRegistry::modules() const {
    std::shared_lock lock(mutex_);
    return modules_;
}

Result<void> ModuleRegistry::initialize_all() {
    std::lock_guard operation_lock(operation_mutex_);
    std::vector<std::shared_ptr<IModule>> snapshot;
    {
        std::shared_lock lock(mutex_);
        if (initialized_) {
            return module_error(ErrorCode::AlreadyInitialized,
                                "The module registry is already initialized.");
        }
        snapshot = modules_;
    }

    std::vector<std::shared_ptr<IModule>> initialized;
    for (const auto& module : snapshot) {
        auto result = initialize_module(module);
        if (!result) {
            std::string rollback_failure;
            std::vector<std::shared_ptr<IModule>> remaining;
            for (auto iterator = initialized.rbegin(); iterator != initialized.rend(); ++iterator) {
                auto rollback = shutdown_module(*iterator);
                if (!rollback) {
                    if (rollback_failure.empty()) {
                        rollback_failure = std::string((*iterator)->name()) + ": " +
                                           rollback.error().message;
                    }
                    remaining.push_back(*iterator);
                }
            }
            std::reverse(remaining.begin(), remaining.end());
            if (!remaining.empty()) {
                std::unique_lock lock(mutex_);
                initialized_modules_ = std::move(remaining);
                initialized_ = true;
            }
            auto failure = result.error();
            failure.metadata.emplace("module", std::string(module->name()));
            if (!rollback_failure.empty()) {
                failure.metadata.emplace("rollback_error", std::move(rollback_failure));
            }
            return failure;
        }
        initialized.push_back(module);
    }

    {
        std::unique_lock lock(mutex_);
        initialized_modules_ = std::move(initialized);
        initialized_ = true;
    }
    return {};
}

Result<void> ModuleRegistry::shutdown_all() {
    std::lock_guard operation_lock(operation_mutex_);
    std::vector<std::shared_ptr<IModule>> initialized;
    {
        std::shared_lock lock(mutex_);
        initialized = initialized_modules_;
    }

    StorqError first_error;
    bool has_error = false;
    std::vector<std::shared_ptr<IModule>> remaining;
    for (auto iterator = initialized.rbegin(); iterator != initialized.rend(); ++iterator) {
        auto result = shutdown_module(*iterator);
        if (!result) {
            if (!has_error) {
                first_error = result.error();
                first_error.metadata.emplace("module", std::string((*iterator)->name()));
                has_error = true;
            }
            remaining.push_back(*iterator);
        }
    }
    std::reverse(remaining.begin(), remaining.end());

    {
        std::unique_lock lock(mutex_);
        initialized_modules_ = std::move(remaining);
        initialized_ = !initialized_modules_.empty();
    }
    if (has_error) {
        return first_error;
    }
    return {};
}

bool ModuleRegistry::is_initialized() const noexcept {
    std::shared_lock lock(mutex_);
    return initialized_;
}

} // namespace storq
