#include <sqlite3.h>

#include <fstream>
#include <cstring>
#include <system_error>

#include <storq/adapters/sqlite/sqlite_adapter.hpp>

namespace storq {

namespace {
StorqError sqlite_error(ErrorCode code, const std::string& message) {
    return StorqError{ErrorCategory::Runtime, code, message, {}};
}

bool valid_sqlite_header(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    char header[16]{};
    if (!file || !file.read(header, sizeof(header))) return false;
    return std::memcmp(header, "SQLite format 3", 15) == 0 && header[15] == '\0';
}
}

AdapterState SQLiteAdapter::state() const noexcept {
    std::lock_guard lock(mutex_);
    return state_;
}

DatabaseCapabilities SQLiteAdapter::capabilities() const {
    DatabaseCapabilities result;
    result.add(DatabaseCapability::ReadOnlyInspection);
    result.add(DatabaseCapability::ReadWriteAccess);
    result.add(DatabaseCapability::Transactions);
    result.add(DatabaseCapability::WriteAheadLogging);
    result.add(DatabaseCapability::Checkpointing);
    result.add(DatabaseCapability::Snapshots);
    result.add(DatabaseCapability::Recovery);
    result.add(DatabaseCapability::PageInspection);
    result.add(DatabaseCapability::FormatInspection);
    result.add(DatabaseCapability::ConcurrentReaders);
    result.add(DatabaseCapability::AtomicTransactions);
    return result;
}

Result<void> SQLiteAdapter::initialize(const AdapterConfig&) {
    std::lock_guard lock(mutex_);
    if (state_ == AdapterState::Ready || state_ == AdapterState::Connected)
        return sqlite_error(ErrorCode::AlreadyInitialized, "SQLite adapter is already initialized.");
    state_ = AdapterState::Initializing;
    state_ = AdapterState::Ready;
    return {};
}

Result<void> SQLiteAdapter::shutdown() {
    std::lock_guard lock(mutex_);
    if (state_ == AdapterState::Stopped) return {};
    state_ = AdapterState::ShuttingDown;
    state_ = AdapterState::Stopped;
    return {};
}

Result<bool> SQLiteAdapter::can_open(const std::filesystem::path& path) const {
    if (!std::filesystem::exists(path) || std::filesystem::is_directory(path))
        return sqlite_error(ErrorCode::InvalidArgument, "SQLite database path must name an existing file.");
    return valid_sqlite_header(path);
}

Result<std::shared_ptr<IDatabaseConnection>> SQLiteAdapter::connect(const DatabaseConnectionConfig& config) {
    if (state() != AdapterState::Ready && state() != AdapterState::Connected)
        return sqlite_error(ErrorCode::InvalidState, "SQLite adapter is not ready.");
    if (config.database_path.empty())
        return sqlite_error(ErrorCode::InvalidArgument, "SQLite database path is required.");
    if (!config.read_only && !std::filesystem::exists(config.database_path))
        return sqlite_error(ErrorCode::InvalidArgument, "A read-write SQLite database path must already exist.");
    auto connection = std::make_shared<SQLiteConnection>(*this, config);
    auto opened = connection->open();
    if (!opened) return sqlite_error(opened.error().code, opened.error().message);
    return std::static_pointer_cast<IDatabaseConnection>(connection);
}

SQLiteConnection::SQLiteConnection(SQLiteAdapter& adapter, DatabaseConnectionConfig config)
    : adapter_(adapter), config_(std::move(config)) {}

SQLiteConnection::~SQLiteConnection() { static_cast<void>(close()); }

Result<void> SQLiteConnection::open() {
    std::lock_guard lock(mutex_);
    if (handle_) return {};
    unsigned flags = config_.read_only ? SQLITE_OPEN_READONLY
                                       : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int timeout_ms = config_.timeout.count() < 1 ? 1
                        : static_cast<int>(std::min<std::chrono::milliseconds::rep>(config_.timeout.count(), 300000));
    sqlite3* opened = nullptr;
    int result = sqlite3_open_v2(config_.database_path.string().c_str(), &opened, flags, nullptr);
    if (result != SQLITE_OK) {
        std::string message = opened ? sqlite3_errmsg(opened) : sqlite3_errstr(result);
        if (opened) sqlite3_close(opened);
        return sqlite_error(ErrorCode::IoError, "Unable to open SQLite database: " + message);
    }
    sqlite3_busy_timeout(opened, timeout_ms);
    handle_ = opened;
    return {};
}

Result<void> SQLiteConnection::close() {
    std::lock_guard lock(mutex_);
    if (!handle_) return {};
    sqlite3_close(handle_);
    handle_ = nullptr;
    return {};
}

bool SQLiteConnection::is_open() const noexcept {
    std::lock_guard lock(mutex_);
    return handle_ != nullptr;
}

Result<DatabaseMetadata> SQLiteConnection::metadata() {
    if (!is_open()) return sqlite_error(ErrorCode::InvalidState, "SQLite connection is closed.");
    SQLiteInspector inspector;
    return inspector.inspect(config_.database_path);
}

Result<void> SQLiteConnection::synchronize() {
    return execute("PRAGMA wal_checkpoint(PASSIVE)");
}

Result<void> SQLiteConnection::execute(const std::string& sql) {
    if (!is_open()) return sqlite_error(ErrorCode::InvalidState, "SQLite connection is closed.");
    if (config_.read_only && (sql.rfind("PRAGMA", 0) != 0 && sql.rfind("SELECT", 0) != 0))
        return sqlite_error(ErrorCode::InvalidState, "Write operation rejected on a read-only SQLite connection.");
    char* error = nullptr;
    int result = sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        std::string message = error ? error : sqlite3_errmsg(handle_);
        sqlite3_free(error);
        return sqlite_error(ErrorCode::IoError, message);
    }
    return {};
}

Result<std::shared_ptr<IDatabaseTransaction>> SQLiteConnection::transaction() {
    if (!is_open()) return sqlite_error(ErrorCode::InvalidState, "SQLite connection is closed.");
    if (config_.read_only) return sqlite_error(ErrorCode::InvalidState, "Transactions are unavailable on a read-only connection.");
    return std::static_pointer_cast<IDatabaseTransaction>(std::make_shared<SQLiteTransaction>(*this));
}

SQLiteTransaction::SQLiteTransaction(SQLiteConnection& connection) : connection_(connection) {}

Result<void> SQLiteTransaction::begin() {
    std::lock_guard lock(mutex_);
    if (active_) return sqlite_error(ErrorCode::InvalidState, "SQLite transaction is already active.");
    auto result = connection_.execute("BEGIN");
    if (!result) return result;
    active_ = true;
    return {};
}

Result<void> SQLiteTransaction::commit() {
    std::lock_guard lock(mutex_);
    if (!active_) return sqlite_error(ErrorCode::InvalidState, "No active SQLite transaction to commit.");
    auto result = connection_.execute("COMMIT");
    if (!result) return result;
    active_ = false;
    return {};
}

Result<void> SQLiteTransaction::rollback() {
    std::lock_guard lock(mutex_);
    if (!active_) return sqlite_error(ErrorCode::InvalidState, "No active SQLite transaction to roll back.");
    auto result = connection_.execute("ROLLBACK");
    if (!result) return result;
    active_ = false;
    return {};
}

bool SQLiteTransaction::is_active() const noexcept {
    std::lock_guard lock(mutex_);
    return active_;
}

Result<DatabaseMetadata> SQLiteInspector::inspect(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path) || !valid_sqlite_header(path))
        return sqlite_error(ErrorCode::InvalidArgument, "Path is not a valid SQLite database.");
    sqlite3* database = nullptr;
    int flags = SQLITE_OPEN_READONLY;
    if (sqlite3_open_v2(path.string().c_str(), &database, flags, nullptr) != SQLITE_OK) {
        std::string message = database ? sqlite3_errmsg(database) : "Unable to open SQLite database.";
        if (database) sqlite3_close(database);
        return sqlite_error(ErrorCode::IoError, message);
    }
    DatabaseMetadata metadata;
    metadata.engine = DatabaseEngine::SQLite;
    metadata.engine_version = SQLITE_VERSION;
    metadata.file_size = std::filesystem::file_size(path);
    metadata.read_only = true;
    metadata.capabilities = SQLiteAdapter{}.capabilities();
    auto pragma = [&](const char* sql, auto&& visitor) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK && statement) {
            if (sqlite3_step(statement) == SQLITE_ROW) visitor(statement);
            sqlite3_finalize(statement);
        }
    };
    pragma("PRAGMA page_size", [&](sqlite3_stmt* statement) { metadata.page_size = sqlite3_column_int64(statement, 0); });
    pragma("PRAGMA page_count", [&](sqlite3_stmt* statement) { metadata.page_count = sqlite3_column_int64(statement, 0); });
    pragma("PRAGMA encoding", [&](sqlite3_stmt* statement) { metadata.encoding = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)); });
    pragma("PRAGMA journal_mode", [&](sqlite3_stmt* statement) { metadata.journal_mode = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)); });
    sqlite3_close(database);
    return metadata;
}

Result<InspectionResult> SQLiteInspector::inspect(const InspectionRequest& request) {
    InspectionResult result;
    auto metadata = inspect(request.database_path);
    if (!metadata) return sqlite_error(metadata.error().code, metadata.error().message);
    result.metadata = metadata.value();
    sqlite3* database = nullptr;
    result.integrity_check_passed =
        sqlite3_open_v2(request.database_path.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK;
    if (result.integrity_check_passed) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, "PRAGMA integrity_check", -1, &statement, nullptr) == SQLITE_OK && statement) {
            result.integrity_check_passed = sqlite3_step(statement) == SQLITE_ROW &&
                std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))) == "ok";
            sqlite3_finalize(statement);
        }
        sqlite3_close(database);
    }
    result.evidence_references.push_back(request.database_path.string());
    if (std::filesystem::exists(request.database_path.string() + "-wal"))
        result.evidence_references.push_back(request.database_path.string() + "-wal");
    result.diagnostics = result.integrity_check_passed ? "SQLite integrity check passed."
                                                       : "SQLite integrity check failed or database could not be opened.";
    return result;
}

Result<void> SQLiteSnapshot::create(const SnapshotRequest& request) {
    if (request.source_database == request.destination)
        return sqlite_error(ErrorCode::InvalidArgument, "Snapshot destination must differ from the source database.");
    if (!valid_sqlite_header(request.source_database))
        return sqlite_error(ErrorCode::InvalidArgument, "Snapshot source is not a valid SQLite database.");
    if (request.destination.has_parent_path())
        std::filesystem::create_directories(request.destination.parent_path());
    std::error_code ec;
    std::filesystem::remove(request.destination, ec);
    sqlite3* source = nullptr;
    sqlite3* destination = nullptr;
    if (sqlite3_open_v2(request.source_database.string().c_str(), &source, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK ||
        sqlite3_open(request.destination.string().c_str(), &destination) != SQLITE_OK) {
        std::string message = source ? sqlite3_errmsg(source) : "Unable to open snapshot destination.";
        if (source) sqlite3_close(source);
        if (destination) sqlite3_close(destination);
        return sqlite_error(ErrorCode::IoError, message);
    }
    sqlite3_backup* backup = sqlite3_backup_init(destination, "main", source, "main");
    if (!backup || sqlite3_backup_step(backup, -1) != SQLITE_DONE || sqlite3_backup_finish(backup) != SQLITE_OK) {
        std::string message = sqlite3_errmsg(destination);
        sqlite3_close(destination);
        sqlite3_close(source);
        return sqlite_error(ErrorCode::IoError, "SQLite backup failed: " + message);
    }
    sqlite3_close(destination);
    sqlite3_close(source);
    metadata_ = {std::filesystem::path("snapshot").filename().string(),
                 request.source_database, request.destination, std::chrono::system_clock::now(),
                 DatabaseEngine::SQLite, true, std::filesystem::file_size(request.destination),
                 request.crash_scenario_id};
    metadata_.snapshot_id = request.destination.filename().string();
    return {};
}

Result<void> SQLiteSnapshot::restore(const std::filesystem::path& destination) {
    if (!metadata_.consistent) return sqlite_error(ErrorCode::InvalidState, "Snapshot metadata is not marked consistent.");
    if (destination == metadata_.source_database)
        return sqlite_error(ErrorCode::InvalidArgument, "Snapshot restore must not overwrite the source database.");
    if (std::filesystem::exists(destination))
        return sqlite_error(ErrorCode::InvalidArgument, "Snapshot restore destination already exists.");
    std::error_code ec;
    std::filesystem::copy_file(metadata_.destination, destination, ec);
    if (ec) return sqlite_error(ErrorCode::IoError, "Unable to restore snapshot: " + ec.message());
    return {};
}

Result<void> SQLiteSnapshot::validate() {
    if (metadata_.destination.empty() || !valid_sqlite_header(metadata_.destination))
        return sqlite_error(ErrorCode::IoError, "Snapshot is missing or is not a valid SQLite database.");
    return {};
}

Result<void> SQLiteSnapshot::remove() {
    if (metadata_.destination.empty()) return {};
    std::error_code ec;
    std::filesystem::remove(metadata_.destination, ec);
    return ec ? Result<void>(sqlite_error(ErrorCode::IoError, ec.message())) : Result<void>{};
}

SnapshotMetadata SQLiteSnapshot::metadata() const { return metadata_; }

Result<AdapterRecoveryResult> SQLiteRecovery::recover(const AdapterRecoveryRequest& request) {
    AdapterRecoveryResult result;
    result.source_modified = false;
    if (!valid_sqlite_header(request.source_database))
        return sqlite_error(ErrorCode::InvalidArgument, "Recovery source is not a valid SQLite database.");
    std::filesystem::create_directories(request.workspace);
    result.recovered_database = request.workspace / request.source_database.filename();
    if (std::filesystem::exists(result.recovered_database))
        return sqlite_error(ErrorCode::InvalidArgument, "Recovery target already exists; refusing to overwrite.");
    std::error_code ec;
    std::filesystem::copy_file(request.source_database, result.recovered_database, ec);
    if (ec) return sqlite_error(ErrorCode::IoError, "Unable to copy recovery evidence: " + ec.message());
    sqlite3* database = nullptr;
    int flags = SQLITE_OPEN_READWRITE;
    if (sqlite3_open_v2(result.recovered_database.string().c_str(), &database, flags, nullptr) != SQLITE_OK) {
        result.diagnostics = database ? sqlite3_errmsg(database) : "SQLite could not open recovery copy.";
        if (database) sqlite3_close(database);
        return result;
    }
    char* integrity_error = nullptr;
    int integrity = sqlite3_exec(database, "PRAGMA integrity_check", nullptr, nullptr, &integrity_error);
    result.can_open = integrity == SQLITE_OK;
    result.recovered = result.can_open;
    sqlite3_close(database);
    sqlite3_free(integrity_error);
    result.diagnostics = result.can_open ? "Recovery copy opened successfully; logical correctness is not verified."
                                         : "Recovery copy failed SQLite integrity opening.";
    return result;
}

}
