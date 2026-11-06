#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <storq/common/result.hpp>
#include <storq/common/types.hpp>

namespace storq {

class Runtime;

enum class DatabaseEngine { Unknown, SQLite, PostgreSQL, RocksDB, LMDB, LevelDB, Storq };
enum class AdapterState { Uninitialized, Initializing, Ready, Connected, Failed, ShuttingDown, Stopped };
enum class DatabaseOperationType { Read, Write, Update, Delete, BeginTransaction, CommitTransaction,
    RollbackTransaction, Checkpoint, Synchronize, Snapshot, Recovery, Custom };
enum class DatabaseCapability { ReadOnlyInspection, ReadWriteAccess, Transactions, WriteAheadLogging,
    Checkpointing, Snapshots, Recovery, PageInspection, FormatInspection, FaultInjection,
    ConcurrentReaders, ConcurrentWriters, AtomicTransactions, CustomStorageFormat };

class DatabaseCapabilities {
public:
    void add(DatabaseCapability capability) { capabilities_.insert(capability); }
    void remove(DatabaseCapability capability) { capabilities_.erase(capability); }
    bool supports(DatabaseCapability capability) const { return capabilities_.count(capability) != 0; }
    bool require(const std::vector<DatabaseCapability>& required, std::string& missing) const;
    const std::unordered_set<DatabaseCapability>& all() const noexcept { return capabilities_; }
private:
    std::unordered_set<DatabaseCapability> capabilities_;
};

struct AdapterConfig {
    std::filesystem::path database_path;
    std::unordered_map<std::string, std::string> settings;
};

struct DatabaseConnectionConfig {
    std::filesystem::path database_path;
    bool read_only{false};
    std::chrono::milliseconds timeout{5000};
    std::unordered_map<std::string, std::string> settings;
};

struct DatabaseMetadata {
    DatabaseEngine engine{DatabaseEngine::Unknown};
    std::optional<std::string> engine_version;
    std::optional<std::uint64_t> format_version;
    std::uint64_t file_size{0};
    std::optional<std::uint64_t> page_size;
    std::optional<std::uint64_t> page_count;
    std::optional<std::string> encoding;
    std::optional<std::string> journal_mode;
    bool read_only{false};
    DatabaseCapabilities capabilities;
};

struct DatabaseOperation {
    DatabaseOperationType type{DatabaseOperationType::Custom};
    std::string operation_id;
    DatabaseEngine engine{DatabaseEngine::Unknown};
    std::chrono::system_clock::time_point timestamp{};
    std::chrono::milliseconds duration{0};
    bool success{false};
    std::string error_message;
    std::optional<std::string> transaction_id;
    std::optional<std::uint64_t> page_number;
    std::optional<std::uint64_t> wal_frame_number;
};

struct SnapshotRequest {
    std::filesystem::path source_database;
    std::filesystem::path destination;
    std::optional<std::string> crash_scenario_id;
};

struct SnapshotMetadata {
    std::string snapshot_id;
    std::filesystem::path source_database;
    std::filesystem::path destination;
    std::chrono::system_clock::time_point created_at{};
    DatabaseEngine engine{DatabaseEngine::Unknown};
    bool consistent{false};
    std::uint64_t size{0};
    std::optional<std::string> crash_scenario_id;
};

struct AdapterRecoveryRequest {
    std::filesystem::path source_database;
    std::filesystem::path workspace;
};

struct AdapterRecoveryResult {
    bool recovered{false};
    bool can_open{false};
    bool source_modified{false};
    std::string diagnostics;
    std::filesystem::path recovered_database;
};

struct InspectionRequest {
    std::filesystem::path database_path;
    bool inspect_pages{true};
    bool inspect_wal{true};
};

struct InspectionResult {
    DatabaseMetadata metadata;
    bool integrity_check_passed{false};
    std::string diagnostics;
    std::vector<std::string> evidence_references;
};

class IDatabaseConnection;
class IDatabaseTransaction;
class IDatabaseSnapshot;

class IDatabaseConnection {
public:
    virtual ~IDatabaseConnection() = default;
    virtual Result<void> open() = 0;
    virtual Result<void> close() = 0;
    virtual bool is_open() const noexcept = 0;
    virtual DatabaseEngine engine() const noexcept = 0;
    virtual Result<DatabaseMetadata> metadata() = 0;
    virtual Result<void> synchronize() = 0;
    virtual Result<void> execute(const std::string& sql) = 0;
    virtual Result<std::shared_ptr<IDatabaseTransaction>> transaction() = 0;
};

class IDatabaseTransaction {
public:
    virtual ~IDatabaseTransaction() = default;
    virtual Result<void> begin() = 0;
    virtual Result<void> commit() = 0;
    virtual Result<void> rollback() = 0;
    virtual bool is_active() const noexcept = 0;
};

class IDatabaseSnapshot {
public:
    virtual ~IDatabaseSnapshot() = default;
    virtual Result<void> create(const SnapshotRequest& request) = 0;
    virtual Result<void> restore(const std::filesystem::path& destination) = 0;
    virtual Result<void> validate() = 0;
    virtual Result<void> remove() = 0;
    virtual SnapshotMetadata metadata() const = 0;
};

class IDatabaseInspector {
public:
    virtual ~IDatabaseInspector() = default;
    virtual Result<DatabaseMetadata> inspect(const std::filesystem::path& database_path) = 0;
};

class IDatabaseRecovery {
public:
    virtual ~IDatabaseRecovery() = default;
    virtual Result<AdapterRecoveryResult> recover(const AdapterRecoveryRequest& request) = 0;
};

class IDatabaseInspection {
public:
    virtual ~IDatabaseInspection() = default;
    virtual Result<InspectionResult> inspect(const InspectionRequest& request) = 0;
};

class IDatabaseAdapter {
public:
    virtual ~IDatabaseAdapter() = default;
    virtual DatabaseEngine engine() const noexcept = 0;
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    virtual AdapterState state() const noexcept = 0;
    virtual DatabaseCapabilities capabilities() const = 0;
    virtual Result<void> initialize(const AdapterConfig& config) = 0;
    virtual Result<void> shutdown() = 0;
    virtual Result<bool> can_open(const std::filesystem::path& path) const = 0;
    virtual Result<std::shared_ptr<IDatabaseConnection>> connect(const DatabaseConnectionConfig& config) = 0;
};

class AdapterRegistry {
public:
    Result<void> register_adapter(std::shared_ptr<IDatabaseAdapter> adapter);
    Result<void> unregister_adapter(DatabaseEngine engine);
    std::shared_ptr<IDatabaseAdapter> get(DatabaseEngine engine) const;
    Result<std::shared_ptr<IDatabaseAdapter>> resolve(const std::filesystem::path& path) const;
    std::vector<DatabaseEngine> available_engines() const;
private:
    mutable std::mutex mutex_;
    struct EngineHash {
        std::size_t operator()(DatabaseEngine engine) const noexcept {
            return std::hash<int>{}(static_cast<int>(engine));
        }
    };
    std::unordered_map<DatabaseEngine, std::shared_ptr<IDatabaseAdapter>, EngineHash> adapters_;
};

class AdapterFactory {
public:
    static Result<std::shared_ptr<IDatabaseAdapter>> create(DatabaseEngine engine,
                                                            const AdapterConfig& config = {});
};

class AdapterManager {
public:
    explicit AdapterManager(Runtime* runtime = nullptr);
    Result<void> register_adapter(std::shared_ptr<IDatabaseAdapter> adapter);
    Result<void> initialize_adapter(DatabaseEngine engine, const AdapterConfig& config = {});
    Result<void> shutdown_all();
    Result<std::shared_ptr<IDatabaseAdapter>> resolve(const std::filesystem::path& path) const;
    DatabaseCapabilities capabilities(DatabaseEngine engine) const;
private:
    Runtime* runtime_;
    AdapterRegistry registry_;
};

[[nodiscard]] std::string_view database_engine_name(DatabaseEngine engine) noexcept;
[[nodiscard]] std::string_view adapter_state_name(AdapterState state) noexcept;

}
