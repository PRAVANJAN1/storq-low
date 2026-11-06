#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <storq/storage/storage.hpp>

namespace storq {

enum class DatabaseFormat {
    Unknown,
    SQLite,
    Storq
};

enum class ValidationSeverity {
    Information,
    Warning,
    Error,
    Critical
};

enum class FormatStatus {
    Valid,
    ValidWithWarnings,
    Unsupported,
    Malformed,
    Incomplete,
    Corrupted
};

enum class SQLiteEncoding : std::uint32_t {
    Unknown = 0,
    UTF8 = 1,
    UTF16LE = 2,
    UTF16BE = 3
};

enum class SQLiteJournalMode {
    RollbackJournal,
    WriteAheadLog,
    Unknown
};

struct FormatWarning {
    ValidationSeverity severity{ValidationSeverity::Warning};
    std::string code;
    std::string message;
};

struct FormatValidation {
    std::string name;
    ValidationSeverity severity{ValidationSeverity::Information};
    bool passed{true};
    std::string message;
};

struct FormatDescriptor {
    DatabaseFormat format{DatabaseFormat::Unknown};
    std::string identifier;
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> supported_features;
    bool parser_available{false};
    bool read_only_inspection{true};
};

class IFormatMetadata {
public:
    virtual ~IFormatMetadata() = default;
};

struct SQLiteHeader {
    std::array<std::byte, 16> signature{};
    std::uint16_t raw_page_size{0};
    std::uint32_t page_size{0};
    std::uint8_t write_version{0};
    std::uint8_t read_version{0};
    std::uint8_t reserved_bytes{0};
    std::uint8_t maximum_embedded_payload_fraction{0};
    std::uint8_t minimum_embedded_payload_fraction{0};
    std::uint8_t leaf_payload_fraction{0};
    std::uint32_t file_change_counter{0};
    std::uint32_t database_size_pages{0};
    std::uint32_t first_freelist_trunk_page{0};
    std::uint32_t total_freelist_pages{0};
    std::uint32_t schema_cookie{0};
    std::uint32_t schema_format{0};
    std::uint32_t default_page_cache_size{0};
    std::uint32_t largest_root_btree_page{0};
    std::uint32_t database_encoding_raw{0};
    SQLiteEncoding database_encoding{SQLiteEncoding::Unknown};
    std::uint32_t user_version{0};
    std::uint32_t incremental_vacuum{0};
    std::uint32_t application_id{0};
    std::array<std::byte, 20> reserved_expansion_area{};
    std::uint32_t version_valid_for{0};
    std::uint32_t sqlite_library_version{0};

    [[nodiscard]] SQLiteJournalMode journal_mode() const noexcept;
    [[nodiscard]] bool header_page_count_is_valid() const noexcept;
};

struct SQLiteMetadata final : IFormatMetadata {
    SQLiteHeader header;
    std::uint32_t usable_page_size{0};
    bool header_page_count_reliable{false};
    std::uint64_t header_reported_page_count{0};
    std::uint64_t physically_available_complete_pages{0};
};

struct StorqInspectionMetadata final : IFormatMetadata {
    std::string detected_version;
};

struct FormatInspectionResult {
    DatabaseFormat format{DatabaseFormat::Unknown};
    FormatDescriptor descriptor;
    FormatStatus status{FormatStatus::Unsupported};
    std::uint64_t file_size{0};
    std::uint32_t page_size{0};
    std::uint64_t page_count{0};
    std::uint64_t complete_pages{0};
    std::uint64_t trailing_bytes{0};
    bool header_valid{false};
    bool structure_valid{false};
    std::vector<FormatValidation> validations;
    std::vector<FormatWarning> warnings;
    std::shared_ptr<const IFormatMetadata> format_metadata;
    std::chrono::milliseconds inspection_duration{0};

    template <typename Metadata>
    [[nodiscard]] const Metadata* metadata_as() const noexcept {
        return dynamic_cast<const Metadata*>(format_metadata.get());
    }
};

struct FormatDetectionResult {
    DatabaseFormat format{DatabaseFormat::Unknown};
    FormatDescriptor descriptor;
    std::shared_ptr<class IFormatParser> parser;
    std::string message;

    [[nodiscard]] bool supported() const noexcept {
        return format != DatabaseFormat::Unknown && static_cast<bool>(parser);
    }
};

class IFormatParser {
public:
    virtual ~IFormatParser() = default;

    [[nodiscard]] virtual DatabaseFormat format() const noexcept = 0;
    [[nodiscard]] virtual FormatDescriptor descriptor() const = 0;
    [[nodiscard]] virtual Result<bool> matches(StorageFile& file) const = 0;
    [[nodiscard]] virtual Result<FormatInspectionResult> inspect(StorageFile& file) const = 0;
};

class StorqFormatParser : public IFormatParser {
public:
    [[nodiscard]] DatabaseFormat format() const noexcept final {
        return DatabaseFormat::Storq;
    }
};

class FormatRegistry {
public:
    [[nodiscard]] Result<void> register_parser(std::shared_ptr<IFormatParser> parser);
    [[nodiscard]] Result<void> unregister_parser(DatabaseFormat format);
    [[nodiscard]] Result<std::shared_ptr<IFormatParser>> find_parser(
        DatabaseFormat format) const;
    [[nodiscard]] std::vector<FormatDescriptor> formats() const;
    [[nodiscard]] Result<FormatDetectionResult> detect(StorageFile& file) const;

private:
    mutable std::shared_mutex mutex_;
    std::vector<std::shared_ptr<IFormatParser>> parsers_;
};

class SQLiteFormatParser final : public IFormatParser {
public:
    [[nodiscard]] DatabaseFormat format() const noexcept override;
    [[nodiscard]] FormatDescriptor descriptor() const override;
    [[nodiscard]] Result<bool> matches(StorageFile& file) const override;
    [[nodiscard]] Result<FormatInspectionResult> inspect(StorageFile& file) const override;

    [[nodiscard]] static Result<SQLiteHeader> parse_header(
        std::span<const std::byte> bytes);
    [[nodiscard]] static bool valid_page_size(std::uint32_t page_size) noexcept;
    [[nodiscard]] static Result<std::uint64_t> page_offset(
        std::uint32_t page_size, std::uint64_t page_number);
    [[nodiscard]] static Result<std::uint64_t> page_boundary(
        std::uint32_t page_size, std::uint64_t page_number);
    [[nodiscard]] static std::uint64_t complete_pages(
        std::uint64_t file_size, std::uint32_t page_size) noexcept;
};

class FormatInspector final : public IModule {
public:
    explicit FormatInspector(Runtime& runtime);
    FormatInspector(Runtime& runtime, StorageManager& storage_manager);
    ~FormatInspector() override;

    FormatInspector(const FormatInspector&) = delete;
    FormatInspector& operator=(const FormatInspector&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Result<void> shutdown() override;
    [[nodiscard]] bool is_initialized() const noexcept override;

    [[nodiscard]] Result<FormatDetectionResult> detect(
        const std::filesystem::path& path) const;
    [[nodiscard]] Result<FormatDetectionResult> detect(StorageFile& file) const;
    [[nodiscard]] Result<FormatInspectionResult> inspect(
        const std::filesystem::path& path) const;
    [[nodiscard]] Result<FormatInspectionResult> inspect(StorageFile& file) const;
    [[nodiscard]] Result<void> register_parser(std::shared_ptr<IFormatParser> parser);
    [[nodiscard]] Result<void> unregister_parser(DatabaseFormat format);
    [[nodiscard]] std::vector<FormatDescriptor> formats() const;

private:
    Runtime& runtime_;
    StorageManager owned_storage_manager_;
    StorageManager* storage_manager_;
    FormatRegistry registry_;
    std::atomic<bool> initialized_{false};
};

[[nodiscard]] std::string_view format_name(DatabaseFormat format) noexcept;
[[nodiscard]] std::string_view validation_severity_name(ValidationSeverity severity) noexcept;
[[nodiscard]] std::string_view format_status_name(FormatStatus status) noexcept;
[[nodiscard]] std::string_view sqlite_encoding_name(SQLiteEncoding encoding) noexcept;
[[nodiscard]] std::string_view sqlite_journal_mode_name(SQLiteJournalMode mode) noexcept;
[[nodiscard]] std::string format_inspection_report(const FormatInspectionResult& result);

} // namespace storq
