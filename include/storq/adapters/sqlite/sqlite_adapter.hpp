#pragma once

#include <sqlite3.h>
#include <storq/adapters/database_adapter.hpp>

namespace storq {

class SQLiteAdapter;

class SQLiteConnection final : public IDatabaseConnection {
public:
    SQLiteConnection(SQLiteAdapter& adapter, DatabaseConnectionConfig config);
    ~SQLiteConnection() override;
    Result<void> open() override;
    Result<void> close() override;
    bool is_open() const noexcept override;
    DatabaseEngine engine() const noexcept override { return DatabaseEngine::SQLite; }
    Result<DatabaseMetadata> metadata() override;
    Result<void> synchronize() override;
    Result<void> execute(const std::string& sql) override;
    Result<std::shared_ptr<IDatabaseTransaction>> transaction() override;
    sqlite3* handle() noexcept { return handle_; }
private:
    SQLiteAdapter& adapter_;
    DatabaseConnectionConfig config_;
    sqlite3* handle_{nullptr};
    mutable std::mutex mutex_;
};

class SQLiteTransaction final : public IDatabaseTransaction {
public:
    explicit SQLiteTransaction(SQLiteConnection& connection);
    Result<void> begin() override;
    Result<void> commit() override;
    Result<void> rollback() override;
    bool is_active() const noexcept override;
private:
    SQLiteConnection& connection_;
    bool active_{false};
    mutable std::mutex mutex_;
};

class SQLiteSnapshot final : public IDatabaseSnapshot {
public:
    Result<void> create(const SnapshotRequest& request) override;
    Result<void> restore(const std::filesystem::path& destination) override;
    Result<void> validate() override;
    Result<void> remove() override;
    SnapshotMetadata metadata() const override;
private:
    SnapshotMetadata metadata_;
};

class SQLiteInspector final : public IDatabaseInspector, public IDatabaseInspection {
public:
    Result<DatabaseMetadata> inspect(const std::filesystem::path& database_path) override;
    Result<InspectionResult> inspect(const InspectionRequest& request) override;
};

class SQLiteRecovery final : public IDatabaseRecovery {
public:
    Result<AdapterRecoveryResult> recover(const AdapterRecoveryRequest& request) override;
};

class SQLiteAdapter final : public IDatabaseAdapter {
public:
    DatabaseEngine engine() const noexcept override { return DatabaseEngine::SQLite; }
    std::string name() const override { return "SQLite Adapter"; }
    std::string version() const override { return SQLITE_VERSION; }
    AdapterState state() const noexcept override;
    DatabaseCapabilities capabilities() const override;
    Result<void> initialize(const AdapterConfig& config) override;
    Result<void> shutdown() override;
    Result<bool> can_open(const std::filesystem::path& path) const override;
    Result<std::shared_ptr<IDatabaseConnection>> connect(const DatabaseConnectionConfig& config) override;
private:
    mutable std::mutex mutex_;
    AdapterState state_{AdapterState::Uninitialized};
    AdapterConfig config_;
};

}
