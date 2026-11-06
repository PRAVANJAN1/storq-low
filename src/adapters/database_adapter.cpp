#include <storq/adapters/database_adapter.hpp>
#include <storq/adapters/sqlite/sqlite_adapter.hpp>
#include <storq/core/runtime.hpp>

namespace storq {

namespace {
StorqError adapter_error(ErrorCode code, std::string message) {
    return StorqError{ErrorCategory::Runtime, code, std::move(message), {}};
}
}

bool DatabaseCapabilities::require(const std::vector<DatabaseCapability>& required, std::string& missing) const {
    for (auto capability : required) {
        if (!supports(capability)) {
            missing = std::to_string(static_cast<int>(capability));
            return false;
        }
    }
    return true;
}

Result<void> AdapterRegistry::register_adapter(std::shared_ptr<IDatabaseAdapter> adapter) {
    if (!adapter) return adapter_error(ErrorCode::InvalidArgument, "Adapter must not be null.");
    std::lock_guard lock(mutex_);
    if (adapters_.count(adapter->engine()) != 0)
        return adapter_error(ErrorCode::DuplicateRegistration, "An adapter for this database engine is already registered.");
    if (adapter->state() == AdapterState::Failed || adapter->state() == AdapterState::Stopped)
        return adapter_error(ErrorCode::InvalidState, "Adapter is not eligible for registration.");
    adapters_[adapter->engine()] = std::move(adapter);
    return {};
}

Result<void> AdapterRegistry::unregister_adapter(DatabaseEngine engine) {
    std::lock_guard lock(mutex_);
    if (adapters_.erase(engine) == 0)
        return adapter_error(ErrorCode::NotFound, "No adapter is registered for this database engine.");
    return {};
}

std::shared_ptr<IDatabaseAdapter> AdapterRegistry::get(DatabaseEngine engine) const {
    std::lock_guard lock(mutex_);
    auto it = adapters_.find(engine);
    return it == adapters_.end() ? nullptr : it->second;
}

Result<std::shared_ptr<IDatabaseAdapter>> AdapterRegistry::resolve(const std::filesystem::path& path) const {
    std::lock_guard lock(mutex_);
    std::shared_ptr<IDatabaseAdapter> match;
    for (const auto& [engine, adapter] : adapters_) {
        static_cast<void>(engine);
        auto can_open = adapter->can_open(path);
        if (can_open && can_open.value()) {
            if (match) return adapter_error(ErrorCode::InvalidArgument, "Multiple adapters claim this database path.");
            match = adapter;
        } else if (!can_open) {
            return adapter_error(can_open.error().code, can_open.error().message);
        }
    }
    if (!match) return adapter_error(ErrorCode::NotFound, "No registered adapter can open this database.");
    return match;
}

std::vector<DatabaseEngine> AdapterRegistry::available_engines() const {
    std::lock_guard lock(mutex_);
    std::vector<DatabaseEngine> engines;
    engines.reserve(adapters_.size());
    for (const auto& [engine, adapter] : adapters_) {
        static_cast<void>(adapter);
        engines.push_back(engine);
    }
    return engines;
}

Result<std::shared_ptr<IDatabaseAdapter>> AdapterFactory::create(DatabaseEngine engine,
                                                                  const AdapterConfig& config) {
    std::shared_ptr<IDatabaseAdapter> adapter;
    if (engine == DatabaseEngine::SQLite) adapter = std::make_shared<SQLiteAdapter>();
    else return adapter_error(ErrorCode::NotFound, "Database adapter is not implemented for this engine.");
    auto initialized = adapter->initialize(config);
    if (!initialized) return adapter_error(initialized.error().code, initialized.error().message);
    return adapter;
}

AdapterManager::AdapterManager(Runtime* runtime) : runtime_(runtime) {}

Result<void> AdapterManager::register_adapter(std::shared_ptr<IDatabaseAdapter> adapter) {
    return registry_.register_adapter(std::move(adapter));
}

Result<void> AdapterManager::initialize_adapter(DatabaseEngine engine, const AdapterConfig& config) {
    auto existing = registry_.get(engine);
    if (!existing) {
        auto created = AdapterFactory::create(engine, config);
        if (!created) return adapter_error(created.error().code, created.error().message);
        existing = created.value();
        auto registered = registry_.register_adapter(existing);
        if (!registered) return registered;
    } else {
        auto initialized = existing->initialize(config);
        if (!initialized) return initialized;
    }
    if (runtime_) static_cast<void>(runtime_->capabilities().register_capability(
        "database-adapter-" + std::string(database_engine_name(engine))));
    return {};
}

Result<void> AdapterManager::shutdown_all() {
    for (auto engine : registry_.available_engines()) {
        auto adapter = registry_.get(engine);
        if (adapter) {
            auto result = adapter->shutdown();
            if (!result) return result;
        }
    }
    return {};
}

Result<std::shared_ptr<IDatabaseAdapter>> AdapterManager::resolve(const std::filesystem::path& path) const {
    return registry_.resolve(path);
}

DatabaseCapabilities AdapterManager::capabilities(DatabaseEngine engine) const {
    auto adapter = registry_.get(engine);
    return adapter ? adapter->capabilities() : DatabaseCapabilities{};
}

std::string_view database_engine_name(DatabaseEngine engine) noexcept {
    switch (engine) {
        case DatabaseEngine::Unknown: return "Unknown";
        case DatabaseEngine::SQLite: return "SQLite";
        case DatabaseEngine::PostgreSQL: return "PostgreSQL";
        case DatabaseEngine::RocksDB: return "RocksDB";
        case DatabaseEngine::LMDB: return "LMDB";
        case DatabaseEngine::LevelDB: return "LevelDB";
        case DatabaseEngine::Storq: return "Storq";
    }
    return "Unknown";
}

std::string_view adapter_state_name(AdapterState state) noexcept {
    switch (state) {
        case AdapterState::Uninitialized: return "Uninitialized";
        case AdapterState::Initializing: return "Initializing";
        case AdapterState::Ready: return "Ready";
        case AdapterState::Connected: return "Connected";
        case AdapterState::Failed: return "Failed";
        case AdapterState::ShuttingDown: return "ShuttingDown";
        case AdapterState::Stopped: return "Stopped";
    }
    return "Unknown";
}

}
