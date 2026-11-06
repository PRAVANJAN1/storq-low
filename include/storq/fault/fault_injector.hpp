#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <storq/storage/storage.hpp>

namespace storq {

class Runtime;

enum class FaultType {
    WriteFailure,
    PartialWrite,
    TruncatedWrite,
    DelayedWrite,
    RejectedWrite,
    SyncFailure,
    InterruptedSync,
    FileTruncation,
    FileCreationFailure,
    FileAccessFailure,
    DiskFull,
    RandomCorruption,
    TargetedCorruption,
    PageHeaderCorruption,
    ChecksumCorruption,
    WALRecordCorruption,
    InvalidOffset
};

enum class FaultStatus {
    Scheduled,
    Triggered,
    Executed,
    Failed,
    Skipped,
    Cancelled
};

enum class FaultSeverity {
    Low,
    Medium,
    High,
    Critical
};

enum class SQLitePageHeaderField {
    PageType,
    FirstFreeblockOffset,
    CellCount,
    CellContentOffset,
    FragmentedFreeBytes,
    RightmostChildPage
};

enum class WALCorruptionRegion {
    FrameHeader,
    PageContent,
    Salt,
    Checksum
};

struct FaultConfig {
    FaultType type{FaultType::WriteFailure};
    double probability{1.0};
    std::optional<StorageOperationType> target_operation;
    std::string target_resource;
    std::optional<std::uint64_t> target_page;
    std::optional<std::uint64_t> target_offset;
    std::optional<std::uint64_t> trigger_count;
    std::optional<std::uint64_t> random_seed;
    std::chrono::nanoseconds delay{0};
    std::optional<std::uint64_t> partial_write_bytes;
    double partial_write_fraction{0.5};
    std::optional<std::uint64_t> maximum_partial_write_bytes;
    std::optional<std::uint64_t> truncation_length;
    std::optional<std::uint64_t> target_size;
    std::optional<std::uint64_t> maximum_capacity;
    std::uint64_t corruption_length{1};
    std::vector<std::byte> corruption_pattern;
    std::optional<std::uint64_t> wal_frame_number;
    SQLitePageHeaderField page_header_field{SQLitePageHeaderField::PageType};
    WALCorruptionRegion wal_region{WALCorruptionRegion::PageContent};
    FaultSeverity severity{FaultSeverity::High};
    bool enabled{true};
};

struct FaultRule {
    std::string id;
    FaultConfig config;
    std::optional<StorageOperationType> target_operation;
    std::string target_resource;
    std::optional<std::uint64_t> target_offset;
    std::optional<std::uint64_t> target_page;
    std::optional<std::uint64_t> operation_sequence;
    std::optional<std::uint64_t> trigger_count;
    std::uint64_t maximum_triggers{std::numeric_limits<std::uint64_t>::max()};
    int priority{0};
    bool enabled{true};
};

struct FaultContext {
    StorageOperationType operation{StorageOperationType::Read};
    std::string resource_id;
    std::optional<std::uint64_t> offset;
    std::uint64_t requested_bytes{0};
    std::optional<std::uint64_t> page_number;
    std::uint64_t operation_sequence{0};
    bool creates_resource{false};
};

struct FaultExecution {
    std::string fault_id;
    FaultType type{FaultType::WriteFailure};
    FaultStatus status{FaultStatus::Scheduled};
    FaultSeverity severity{FaultSeverity::High};
    StorageOperationType operation{StorageOperationType::Read};
    std::string resource_id;
    std::uint64_t operation_sequence{0};
    std::uint64_t target_offset{0};
    std::uint64_t requested_bytes{0};
    std::uint64_t actual_bytes{0};
    std::chrono::system_clock::time_point triggered_at{};
    std::optional<std::chrono::system_clock::time_point> completed_at;
    std::chrono::system_clock::time_point recorded_at{};
    std::chrono::nanoseconds actual_delay{0};
    std::vector<std::uint64_t> corrupted_offsets;
    std::string description;
};

struct FaultOperationTrace {
    std::uint64_t operation_sequence{0};
    StorageOperationType operation{StorageOperationType::Read};
    std::string resource_id;
    std::optional<std::uint64_t> offset;
    std::uint64_t requested_bytes{0};
    std::optional<std::string> selected_fault_id;
    bool creates_resource{false};
    std::chrono::system_clock::time_point evaluated_at{};
};

struct FaultStatistics {
    std::uint64_t total_configured{0};
    std::uint64_t total_scheduled{0};
    std::uint64_t total_triggered{0};
    std::uint64_t total_executed{0};
    std::uint64_t total_failed{0};
    std::uint64_t total_skipped{0};
    std::uint64_t total_cancelled{0};
    std::uint64_t total_partial_bytes{0};
    std::uint64_t total_corrupted_bytes{0};
    std::chrono::nanoseconds total_injected_delay{0};
    std::map<FaultType, std::uint64_t> faults_by_type;
    std::map<StorageOperationType, std::uint64_t> faults_by_operation;
};

struct FaultCapacityStatus {
    std::uint64_t maximum_capacity{0};
    std::uint64_t current_usage{0};
    std::uint64_t remaining_capacity{0};
    bool full{false};
};

struct FaultDecision {
    FaultRule rule;
    FaultContext context;
};

struct TestEnvironment {
    std::filesystem::path root_directory;
    bool isolated{false};
    bool disposable{false};
    bool allow_destructive_operations{false};
};

class FaultPolicy {
public:
    [[nodiscard]] static Result<void> validate(const FaultConfig& config);
};

class FaultHistory {
public:
    void record(FaultExecution execution);
    void record_operation(FaultOperationTrace operation);
    [[nodiscard]] std::vector<FaultExecution> entries() const;
    [[nodiscard]] std::vector<FaultOperationTrace> operations() const;
    [[nodiscard]] FaultStatistics statistics() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<FaultExecution> entries_;
    std::vector<FaultOperationTrace> operations_;
};

class FaultDecisionEngine {
public:
    [[nodiscard]] static bool matches(
        const FaultRule& rule, const FaultContext& context,
        std::uint64_t matching_trigger_count);
};

class FaultScheduler {
public:
    explicit FaultScheduler(std::shared_ptr<FaultHistory> history, std::uint64_t seed = 0);

    [[nodiscard]] Result<void> add_rule(FaultRule rule);
    [[nodiscard]] bool cancel(std::string_view rule_id);
    [[nodiscard]] Result<std::optional<FaultDecision>> evaluate(FaultContext context);
    [[nodiscard]] std::vector<FaultRule> rules() const;
    [[nodiscard]] std::vector<std::byte> random_bytes(
        std::size_t count, std::string_view rule_id = {});
    [[nodiscard]] std::uint64_t random_uint64(std::string_view rule_id = {});
    [[nodiscard]] std::uint64_t next_operation_sequence() noexcept;
    void reset(std::uint64_t seed);
    [[nodiscard]] bool has_destructive_rules() const;

private:
    mutable std::mutex mutex_;
    std::vector<FaultRule> rules_;
    std::map<std::string, std::uint64_t, std::less<>> matching_counts_;
    std::map<std::string, std::uint64_t, std::less<>> trigger_counts_;
    std::map<std::string, std::mt19937_64, std::less<>> rule_random_;
    std::mt19937_64 random_;
    std::uint64_t seed_{0};
    std::atomic<std::uint64_t> operation_sequence_{0};
    std::shared_ptr<FaultHistory> history_;
};

class FaultInjectedBackend final : public IStorageBackend {
public:
    explicit FaultInjectedBackend(std::shared_ptr<IStorageBackend> backend);
    ~FaultInjectedBackend() override;

    FaultInjectedBackend(const FaultInjectedBackend&) = delete;
    FaultInjectedBackend& operator=(const FaultInjectedBackend&) = delete;

    [[nodiscard]] Result<std::size_t> read_at(
        std::uint64_t offset, std::span<std::byte> buffer) override;
    [[nodiscard]] Result<std::size_t> write_at(
        std::uint64_t offset, std::span<const std::byte> buffer) override;
    [[nodiscard]] Result<std::size_t> append(
        std::span<const std::byte> buffer) override;
    [[nodiscard]] Result<void> truncate(std::uint64_t size) override;
    [[nodiscard]] Result<void> sync() override;
    [[nodiscard]] Result<std::uint64_t> size() const override;
    [[nodiscard]] Result<StorageMetadata> metadata() const override;
    [[nodiscard]] Result<void> close() override;
    [[nodiscard]] bool is_open() const noexcept override;
    [[nodiscard]] StorageAccess access() const noexcept override;
    void cancel_delays() noexcept;

private:
    friend class FaultInjector;
    FaultInjectedBackend(std::shared_ptr<IStorageBackend> backend,
                         std::shared_ptr<FaultScheduler> scheduler,
                         std::shared_ptr<FaultHistory> history,
                         bool destructive_authorized);

    [[nodiscard]] Result<FaultContext> context(
        StorageOperationType operation, std::optional<std::uint64_t> offset,
        std::uint64_t requested_bytes) const;
    [[nodiscard]] Result<std::optional<FaultDecision>> decide(
        StorageOperationType operation, std::optional<std::uint64_t> offset,
        std::uint64_t requested_bytes) const;
    [[nodiscard]] Result<void> wait_for_delay(
        const FaultDecision& decision, FaultExecution& execution, bool& cancelled);
    [[nodiscard]] Result<std::vector<std::uint64_t>> corrupt_write(
        const FaultDecision& decision, std::uint64_t offset,
        std::vector<std::byte>& bytes);
    void record(const FaultDecision& decision, FaultStatus status,
                std::uint64_t actual_bytes, std::string description,
                std::vector<std::uint64_t> offsets = {},
                std::chrono::nanoseconds delay = {}) const;
    [[nodiscard]] Result<void> require_destructive_authorization(
        const FaultDecision& decision) const;

    std::shared_ptr<IStorageBackend> backend_;
    std::shared_ptr<FaultScheduler> scheduler_;
    std::shared_ptr<FaultHistory> history_;
    bool destructive_authorized_{false};
    mutable std::mutex delay_mutex_;
    std::condition_variable_any delay_condition_;
    std::stop_source delay_stop_source_;
};

class FaultManager {
public:
    explicit FaultManager(std::shared_ptr<FaultScheduler> scheduler);
    [[nodiscard]] Result<void> add_rule(FaultRule rule);
    [[nodiscard]] bool remove_rule(std::string_view rule_id);
    [[nodiscard]] std::vector<FaultRule> rules() const;

private:
    std::shared_ptr<FaultScheduler> scheduler_;
};

class FaultInjector final : public IModule {
public:
    explicit FaultInjector(Runtime& runtime);
    ~FaultInjector() override;

    FaultInjector(const FaultInjector&) = delete;
    FaultInjector& operator=(const FaultInjector&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Result<void> shutdown() override;
    [[nodiscard]] bool is_initialized() const noexcept override;

    [[nodiscard]] Result<void> configure_environment(TestEnvironment environment);
    [[nodiscard]] Result<void> set_random_seed(std::uint64_t seed);
    [[nodiscard]] Result<void> add_rule(FaultRule rule);
    [[nodiscard]] bool remove_rule(std::string_view rule_id);
    [[nodiscard]] Result<void> reset_scenario(std::uint64_t seed);
    [[nodiscard]] Result<std::shared_ptr<StorageFile>> open_file(
        StorageManager& manager, const std::filesystem::path& path,
        const StorageOpenOptions& options = {});
    [[nodiscard]] Result<std::shared_ptr<StorageFile>> create_memory_storage(
        StorageManager& manager, std::size_t capacity = static_cast<std::size_t>(-1));
    [[nodiscard]] Result<std::shared_ptr<StorageFile>> wrap_storage(
        StorageManager& manager, std::shared_ptr<IStorageBackend> backend);
    [[nodiscard]] Result<std::filesystem::path> create_test_copy(
        StorageManager& manager, const std::filesystem::path& source,
        const std::filesystem::path& destination);
    [[nodiscard]] Result<void> cleanup_file(const std::filesystem::path& path);

    [[nodiscard]] std::vector<FaultExecution> history() const;
    [[nodiscard]] std::vector<FaultOperationTrace> operation_trace() const;
    [[nodiscard]] FaultStatistics statistics() const;
    [[nodiscard]] Result<FaultCapacityStatus> capacity_status(
        const StorageFile& file, std::string_view rule_id) const;
    void cancel_pending_delays() noexcept;
    [[nodiscard]] std::vector<FaultRule> rules() const;

private:
    [[nodiscard]] Result<std::filesystem::path> validate_environment_path(
        const std::filesystem::path& path, bool require_existing) const;
    [[nodiscard]] bool destructive_authorized_for(
        const StorageMetadata& metadata) const;
    [[nodiscard]] bool environment_allows_destructive() const;
    void track_backend(const std::shared_ptr<FaultInjectedBackend>& backend);

    Runtime& runtime_;
    mutable std::mutex mutex_;
    TestEnvironment environment_;
    std::filesystem::path canonical_root_;
    bool environment_configured_{false};
    bool initialized_{false};
    std::shared_ptr<FaultHistory> history_;
    std::shared_ptr<FaultScheduler> scheduler_;
    FaultManager manager_;
    std::set<std::filesystem::path> created_files_;
    std::vector<std::weak_ptr<FaultInjectedBackend>> backends_;
};

[[nodiscard]] std::string_view fault_type_name(FaultType type) noexcept;
[[nodiscard]] std::string_view fault_status_name(FaultStatus status) noexcept;

} // namespace storq
