#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include <storq/core/module.hpp>

namespace storq {

class ModuleRegistry {
public:
    [[nodiscard]] Result<void> register_module(std::shared_ptr<IModule> module);
    [[nodiscard]] Result<void> unregister_module(std::string_view name);
    [[nodiscard]] Result<std::shared_ptr<IModule>> find(std::string_view name) const;
    [[nodiscard]] std::vector<std::shared_ptr<IModule>> modules() const;

    [[nodiscard]] Result<void> initialize_all();
    [[nodiscard]] Result<void> shutdown_all();
    [[nodiscard]] bool is_initialized() const noexcept;

private:
    mutable std::shared_mutex mutex_;
    std::mutex operation_mutex_;
    std::vector<std::shared_ptr<IModule>> modules_;
    std::vector<std::shared_ptr<IModule>> initialized_modules_;
    bool initialized_{false};
};

} // namespace storq
