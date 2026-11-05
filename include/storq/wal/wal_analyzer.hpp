#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <storq/format/format_inspector.hpp>

namespace storq {

enum class WALFormat {
    Unknown,
    SQLite,
    Storq
};

enum class WALChecksumByteOrder {
    Unknown,
    LittleEndian,
    BigEndian
};

struct SQLiteWALHeader {
    std::uint32_t magic{0};
    std::uint32_t format_version{0};
    std::uint32_t page_size{0};
    std::uint32_t checkpoint_sequence{0};
    std::uint32_t salt_1{0};
    std::uint32_t salt_2{0};
    std::uint32_t checksum_1{0};
    std::uint32_t checksum_2{0};
    WALChecksumByteOrder checksum_byte_order{WALChecksumByteOrder::Unknown};
};

struct SQLiteWALChecksumState {
    std::uint32_t first{0};
    std::uint32_t second{0};
};

struct SQLiteWALFrameHeader {
    std::uint32_t page_number{0};
    std::uint32_t database_size{0};
    std::uint32_t salt_1{0};
    std::uint32_t salt_2{0};
    std::uint32_t checksum_1{0};
    std::uint32_t checksum_2{0};
};

struct SQLiteWALFrame {
    std::uint64_t frame_number{0};
    std::uint64_t file_offset{0};
    SQLiteWALFrameHeader header;
    SQLiteWALChecksumState calculated_checksum;
    bool salts_valid{false};
    bool checksum_valid{false};

    [[nodiscard]] bool is_commit() const noexcept {
        return header.database_size != 0;
    }
};

enum class WALIssueType {
    InvalidMagic,
    UnsupportedVersion,
    InvalidPageSize,
    InvalidHeaderChecksum,
    InvalidFrameChecksum,
    InvalidSalt,
    InvalidPageNumber,
    TruncatedHeader,
    TruncatedFrame,
    InvalidFrameBoundary,
    InvalidTransactionBoundary,
    InvalidDatabaseSize,
    IncompleteTransaction,
    Unknown
};

enum class WALIssueSeverity {
    Information,
    Warning,
    Error,
    Critical
};

struct WALIssue {
    WALIssueType type{WALIssueType::Unknown};
    WALIssueSeverity severity{WALIssueSeverity::Warning};
    std::optional<std::uint64_t> frame_number;
    std::optional<std::uint64_t> byte_offset;
    std::string description;
    std::string expected;
    std::string observed;
    bool valid{false};
};

struct WALTransaction {
    std::uint64_t transaction_id{0};
    std::uint64_t first_frame{0};
    std::uint64_t last_frame{0};
    std::uint64_t frame_count{0};
    std::uint32_t committed_database_pages{0};
    bool committed{false};
    bool complete{false};
};

enum class WALEventType {
    HeaderValidated,
    FrameParsed,
    FrameChecksumVerified,
    TransactionCommitted,
    IncompleteTransaction,
    InvalidFrame,
    CheckpointMetadataObserved
};

struct WALEvent {
    std::uint64_t event_id{0};
    WALEventType type{WALEventType::HeaderValidated};
    std::optional<std::uint64_t> frame_number;
    std::uint64_t file_offset{0};
    std::chrono::system_clock::time_point analysis_time;
    std::optional<std::uint64_t> transaction_id;
    std::string description;
    bool valid{true};
};

struct WALCheckpointMetadata {
    std::uint32_t checkpoint_sequence{0};
    std::uint32_t salt_1{0};
    std::uint32_t salt_2{0};
    std::optional<std::uint64_t> last_commit_frame;
    std::optional<std::uint32_t> database_pages_at_last_commit;
    bool exact_checkpoint_state_known{false};
};

struct WALAnalysisLimits {
    std::uint64_t maximum_frames{1000000};
    std::uint64_t maximum_transactions{100000};
    std::uint64_t maximum_timeline_events{2000000};
    std::uint64_t maximum_issues{100000};
    std::uint64_t maximum_memory_bytes{128ULL * 1024ULL * 1024ULL};
    std::uint32_t frame_batch_size{32};
};

struct WALAnalysisOptions {
    WALAnalysisLimits limits;
    std::function<bool(std::uint64_t frames_scanned, std::uint64_t total_frames)> progress;
};

struct WALAnalysisReport {
    SQLiteWALHeader header;
    std::uint64_t file_size{0};
    std::uint64_t total_frames{0};
    std::uint64_t valid_frames{0};
    std::uint64_t invalid_frames{0};
    std::uint64_t committed_transactions{0};
    std::uint64_t incomplete_transactions{0};
    std::uint64_t last_commit_frame{0};
    bool header_valid{false};
    bool checksums_valid{false};
    bool structurally_valid{false};
    bool complete_analysis{true};
    WALCheckpointMetadata checkpoint;
    std::vector<SQLiteWALFrame> frames;
    std::vector<WALTransaction> transactions;
    std::vector<WALIssue> issues;
    std::vector<WALEvent> timeline;
    std::chrono::milliseconds analysis_duration{0};
};

struct WALFrameInspectionResult {
    SQLiteWALFrame frame;
    std::vector<std::byte> page_data;
};

struct WALParserDescriptor {
    WALFormat format{WALFormat::Unknown};
    std::string identifier;
    std::string name;
    std::string version;
    bool available{false};
};

class IWALParser {
public:
    virtual ~IWALParser() = default;
    [[nodiscard]] virtual WALFormat format() const noexcept = 0;
    [[nodiscard]] virtual WALParserDescriptor descriptor() const = 0;
    [[nodiscard]] virtual Result<bool> matches(StorageFile& file) const = 0;
    [[nodiscard]] virtual Result<WALAnalysisReport> analyze(
        StorageFile& file, const WALAnalysisOptions& options,
        const SQLiteMetadata* database_metadata) const = 0;
};

class WALRegistry {
public:
    [[nodiscard]] Result<void> register_parser(std::shared_ptr<IWALParser> parser);
    [[nodiscard]] Result<void> unregister_parser(WALFormat format);
    [[nodiscard]] Result<std::shared_ptr<IWALParser>> find_parser(WALFormat format) const;
    [[nodiscard]] std::vector<WALParserDescriptor> parsers() const;
    [[nodiscard]] Result<std::shared_ptr<IWALParser>> detect(StorageFile& file) const;

private:
    mutable std::shared_mutex mutex_;
    std::vector<std::shared_ptr<IWALParser>> parsers_;
};

class SQLiteWALChecksum {
public:
    [[nodiscard]] static Result<SQLiteWALChecksumState> calculate(
        std::span<const std::byte> bytes, WALChecksumByteOrder byte_order,
        SQLiteWALChecksumState initial = {});
};

class SQLiteWALParser final : public IWALParser {
public:
    [[nodiscard]] WALFormat format() const noexcept override;
    [[nodiscard]] WALParserDescriptor descriptor() const override;
    [[nodiscard]] Result<bool> matches(StorageFile& file) const override;
    [[nodiscard]] Result<WALAnalysisReport> analyze(
        StorageFile& file, const WALAnalysisOptions& options,
        const SQLiteMetadata* database_metadata) const override;
    [[nodiscard]] Result<SQLiteWALHeader> parse_header(
        std::span<const std::byte> bytes) const;
    [[nodiscard]] Result<SQLiteWALFrameHeader> parse_frame_header(
        std::span<const std::byte> bytes) const;
    [[nodiscard]] Result<WALFrameInspectionResult> inspect_frame(
        StorageFile& file, const SQLiteWALHeader& header,
        std::uint64_t frame_number, SQLiteWALChecksumState previous_checksum) const;
};

class WALDetector {
public:
    [[nodiscard]] static Result<WALFormat> detect(
        const WALRegistry& registry, StorageFile& file);
};

class FrameAnalyzer {
public:
    [[nodiscard]] static std::vector<std::uint64_t> page_history(
        const WALAnalysisReport& report, std::uint32_t page_number);
    [[nodiscard]] static std::vector<WALIssue> validate_sequence(
        const WALAnalysisReport& report, std::uint64_t maximum_issues);
};

class TransactionAnalyzer {
public:
    [[nodiscard]] static std::vector<WALTransaction> analyze(
        const std::vector<SQLiteWALFrame>& frames, std::uint64_t maximum_transactions,
        std::vector<WALIssue>& issues, std::uint64_t maximum_issues);
};

class CheckpointAnalyzer {
public:
    [[nodiscard]] static WALCheckpointMetadata analyze(
        const SQLiteWALHeader& header, const std::vector<WALTransaction>& transactions);
};

class WALTimeline {
public:
    [[nodiscard]] static std::vector<WALEvent> reconstruct(
        const WALAnalysisReport& report, std::uint64_t maximum_events);
};

class WALAnalyzer final : public IModule {
public:
    explicit WALAnalyzer(Runtime& runtime);
    WALAnalyzer(Runtime& runtime, StorageManager& storage_manager);
    ~WALAnalyzer() override;

    WALAnalyzer(const WALAnalyzer&) = delete;
    WALAnalyzer& operator=(const WALAnalyzer&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Result<void> shutdown() override;
    [[nodiscard]] bool is_initialized() const noexcept override;
    [[nodiscard]] Result<void> register_parser(std::shared_ptr<IWALParser> parser);
    [[nodiscard]] Result<WALFormat> detect(StorageFile& file) const;
    [[nodiscard]] Result<WALAnalysisReport> inspect(
        const std::filesystem::path& path,
        const WALAnalysisOptions& options = {},
        const SQLiteMetadata* database_metadata = nullptr) const;
    [[nodiscard]] Result<WALAnalysisReport> inspect(
        StorageFile& file, const WALAnalysisOptions& options = {},
        const SQLiteMetadata* database_metadata = nullptr) const;
    [[nodiscard]] Result<WALFrameInspectionResult> inspect_frame(
        StorageFile& file, std::uint64_t frame_number,
        const SQLiteMetadata* database_metadata = nullptr) const;
    [[nodiscard]] Result<std::vector<std::uint64_t>> page_history(
        StorageFile& file, std::uint32_t page_number,
        const WALAnalysisOptions& options = {},
        const SQLiteMetadata* database_metadata = nullptr) const;
    [[nodiscard]] std::vector<WALParserDescriptor> parsers() const;

private:
    Runtime& runtime_;
    StorageManager owned_storage_manager_;
    StorageManager* storage_manager_;
    WALRegistry registry_;
    std::atomic<bool> initialized_{false};
};

[[nodiscard]] std::string_view wal_format_name(WALFormat format) noexcept;
[[nodiscard]] std::string_view wal_checksum_byte_order_name(
    WALChecksumByteOrder order) noexcept;
[[nodiscard]] std::string_view wal_issue_type_name(WALIssueType type) noexcept;
[[nodiscard]] std::string_view wal_issue_severity_name(WALIssueSeverity severity) noexcept;
[[nodiscard]] std::string_view wal_event_type_name(WALEventType type) noexcept;
[[nodiscard]] std::string format_wal_report(const WALAnalysisReport& report);
[[nodiscard]] std::string export_wal_report_json(const WALAnalysisReport& report);

} // namespace storq
