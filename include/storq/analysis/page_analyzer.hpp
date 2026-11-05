#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <storq/format/format_inspector.hpp>

namespace storq {

enum class SQLitePageType {
    InteriorIndex,
    InteriorTable,
    LeafIndex,
    LeafTable,
    Unknown
};

enum class CorruptionType {
    InvalidPageType,
    InvalidPageHeader,
    InvalidCellPointer,
    InvalidCellBoundary,
    OverlappingCells,
    InvalidFreeblock,
    FreeblockCycle,
    InvalidVarint,
    InvalidChildReference,
    InvalidOverflowReference,
    OverflowCycle,
    InvalidPageRelationship,
    InvalidSpaceAccounting,
    UnreachablePage,
    Unknown
};

enum class CorruptionSeverity {
    Low,
    Medium,
    High,
    Critical
};

struct CorruptionFinding {
    CorruptionType type{CorruptionType::Unknown};
    CorruptionSeverity severity{CorruptionSeverity::Low};
    std::uint32_t page_number{0};
    std::optional<std::uint32_t> byte_offset;
    std::string description;
    std::string expected;
    std::string observed;
    bool confirmed{true};
};

struct SQLitePageHeader {
    SQLitePageType page_type{SQLitePageType::Unknown};
    std::uint16_t first_freeblock_offset{0};
    std::uint16_t cell_count{0};
    std::uint16_t cell_content_area_offset{0};
    std::uint8_t fragmented_free_bytes{0};
    std::uint32_t rightmost_child_page{0};
    std::uint32_t header_offset{0};
    std::uint32_t header_size{0};

    [[nodiscard]] bool is_leaf() const noexcept;
    [[nodiscard]] bool is_interior() const noexcept;
    [[nodiscard]] bool contains_index_records() const noexcept;
    [[nodiscard]] bool contains_table_records() const noexcept;
};

struct CellPointer {
    std::uint32_t array_offset{0};
    std::uint16_t cell_offset{0};
};

struct CellStructure {
    std::uint32_t cell_offset{0};
    std::uint32_t cell_size{0};
    std::optional<std::uint32_t> left_child_page;
    std::optional<std::uint64_t> rowid;
    std::uint64_t payload_size{0};
    std::uint32_t local_payload_size{0};
    std::uint32_t local_payload_offset{0};
    std::optional<std::uint32_t> first_overflow_page;
    bool valid{false};
};

struct Freeblock {
    std::uint16_t offset{0};
    std::uint16_t size{0};
    std::uint16_t next_offset{0};
    bool valid{false};
};

struct OverflowReference {
    std::uint32_t source_page{0};
    std::uint32_t first_overflow_page{0};
    std::uint64_t expected_payload_bytes{0};
    std::uint64_t remaining_payload_bytes{0};
    std::uint32_t pages_traversed{0};
    bool complete{false};
};

struct PageSpaceAnalysis {
    std::uint32_t usable_size{0};
    std::uint32_t header_size{0};
    std::uint32_t pointer_array_size{0};
    std::uint32_t cell_content_size{0};
    std::uint32_t freeblock_bytes{0};
    std::uint32_t fragmented_bytes{0};
    std::uint32_t unallocated_bytes{0};
    std::uint32_t cell_bytes{0};
    std::uint32_t accounted_bytes{0};
    std::uint32_t unaccounted_bytes{0};
};

struct PageRelationship {
    enum class Type {
        BTreeChild,
        OverflowNext
    };

    std::uint32_t source_page{0};
    std::uint32_t target_page{0};
    Type type{Type::BTreeChild};
};

struct PageAnalysisLimits {
    std::uint64_t maximum_pages{100000};
    std::uint32_t maximum_cells_per_page{20000};
    std::uint32_t maximum_overflow_chain_depth{10000};
    std::uint32_t maximum_btree_depth{1000};
    std::uint64_t maximum_relationship_nodes{100000};
    std::uint64_t maximum_findings{100000};
    std::uint64_t maximum_memory_bytes{64ULL * 1024ULL * 1024ULL};
};

struct PageAnalysisOptions {
    PageAnalysisLimits limits;
    bool follow_overflow_chains{true};
    bool validate_relationships{true};
};

struct PageAnalysisResult {
    std::uint32_t page_number{0};
    SQLitePageType page_type{SQLitePageType::Unknown};
    SQLitePageHeader header;
    std::vector<CellPointer> cell_pointers;
    std::vector<CellStructure> cells;
    std::vector<Freeblock> freeblocks;
    std::vector<OverflowReference> overflow_references;
    PageSpaceAnalysis space;
    std::vector<PageRelationship> relationships;
    std::vector<CorruptionFinding> findings;
    bool structurally_valid{false};
    bool supported{true};
    bool complete_analysis{true};
    std::string message;
};

struct DatabasePageAnalysisResult {
    bool complete_analysis{true};
    std::uint64_t total_pages_analyzed{0};
    std::uint64_t valid_pages{0};
    std::uint64_t pages_with_warnings{0};
    std::uint64_t corrupted_pages{0};
    std::uint64_t unsupported_pages{0};
    std::uint64_t total_findings{0};
    std::vector<PageRelationship> relationships;
    std::vector<CorruptionFinding> findings;
    std::chrono::milliseconds analysis_duration{0};
};

struct BTreeAnalysisResult {
    std::uint32_t root_page{0};
    std::uint32_t maximum_depth{0};
    std::uint64_t interior_pages{0};
    std::uint64_t leaf_pages{0};
    std::uint64_t pages_visited{0};
    std::vector<std::uint32_t> visited_pages;
    std::vector<CorruptionFinding> findings;
};

class PageReader {
public:
    explicit PageReader(StorageFile& file);
    [[nodiscard]] Result<std::vector<std::byte>> read_page(
        std::uint32_t page_number, std::uint32_t page_size,
        std::uint64_t page_count) const;

private:
    StorageFile& file_;
};

class SQLitePageHeaderParser {
public:
    [[nodiscard]] static Result<SQLitePageHeader> parse(
        std::uint32_t page_number, std::span<const std::byte> page,
        std::uint32_t page_size);
};

struct SQLiteVarint {
    std::uint64_t value{0};
    std::uint8_t bytes{0};
};

class SQLiteCellParser {
public:
    [[nodiscard]] static Result<SQLiteVarint> decode_varint(
        std::span<const std::byte> bytes, std::size_t offset);
    [[nodiscard]] static std::uint32_t maximum_local_payload(
        SQLitePageType type, std::uint32_t usable_size) noexcept;
    [[nodiscard]] static std::uint32_t minimum_local_payload(
        std::uint32_t usable_size) noexcept;
    [[nodiscard]] static Result<std::uint32_t> local_payload_size(
        SQLitePageType type, std::uint32_t usable_size, std::uint64_t payload_size);
};

struct OverflowAnalysisResult {
    OverflowReference reference;
    std::vector<PageRelationship> relationships;
    std::vector<CorruptionFinding> findings;
};

class SQLiteOverflowParser {
public:
    [[nodiscard]] static Result<OverflowAnalysisResult> analyze(
        StorageFile& file, std::uint32_t source_page,
        std::uint32_t first_overflow_page, std::uint64_t expected_payload_bytes,
        std::uint32_t page_size, std::uint32_t usable_size,
        std::uint64_t page_count, const PageAnalysisOptions& options);
};

class SQLiteBTreePageParser {
public:
    [[nodiscard]] static Result<PageAnalysisResult> parse(
        std::uint32_t page_number, std::span<const std::byte> page,
        std::uint32_t page_size, std::uint32_t usable_size,
        std::uint64_t page_count, const PageAnalysisOptions& options);
};

class SQLitePageValidator {
public:
    static void validate(PageAnalysisResult& result,
                         const PageAnalysisOptions& options);
};

class CorruptionDetector {
public:
    [[nodiscard]] static CorruptionSeverity severity(CorruptionType type) noexcept;
    static void add(PageAnalysisResult& result, CorruptionType type,
                    std::uint32_t page_number, std::optional<std::uint32_t> byte_offset,
                    std::string description, std::string expected,
                    std::string observed, bool confirmed = true,
                    std::uint64_t maximum_findings = 100000);
};

class PageAnalyzer;

class PageRelationshipAnalyzer {
public:
    [[nodiscard]] static std::vector<CorruptionFinding> validate(
        const std::vector<PageAnalysisResult>& pages, std::uint64_t page_count,
        std::uint64_t maximum_findings);
    [[nodiscard]] static std::vector<CorruptionFinding> validate(
        const std::vector<PageRelationship>& relationships,
        const std::vector<std::uint32_t>& parsed_btree_pages,
        std::uint64_t page_count, std::uint64_t maximum_findings,
        bool complete_page_set);
};

class BTreeAnalyzer {
public:
    [[nodiscard]] static Result<BTreeAnalysisResult> analyze(
        PageAnalyzer& analyzer, StorageFile& file, std::uint32_t root_page,
        const PageAnalysisOptions& options);
};

class PageAnalyzer final : public IModule {
public:
    explicit PageAnalyzer(Runtime& runtime);
    PageAnalyzer(Runtime& runtime, StorageManager& storage_manager,
                 FormatInspector& format_inspector);
    ~PageAnalyzer() override;

    PageAnalyzer(const PageAnalyzer&) = delete;
    PageAnalyzer& operator=(const PageAnalyzer&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Result<void> shutdown() override;
    [[nodiscard]] bool is_initialized() const noexcept override;

    [[nodiscard]] Result<PageAnalysisResult> inspect_page(
        const std::filesystem::path& path, std::uint32_t page_number,
        const PageAnalysisOptions& options = {}) const;
    [[nodiscard]] Result<PageAnalysisResult> inspect_page(
        StorageFile& file, std::uint32_t page_number,
        const PageAnalysisOptions& options = {}) const;
    [[nodiscard]] Result<std::vector<PageAnalysisResult>> inspect_pages(
        StorageFile& file, std::span<const std::uint32_t> page_numbers,
        const PageAnalysisOptions& options = {}) const;
    [[nodiscard]] Result<DatabasePageAnalysisResult> scan_database(
        const std::filesystem::path& path,
        const PageAnalysisOptions& options = {}) const;
    [[nodiscard]] Result<DatabasePageAnalysisResult> scan_database(
        StorageFile& file, const PageAnalysisOptions& options = {}) const;
    [[nodiscard]] Result<BTreeAnalysisResult> analyze_btree(
        StorageFile& file, std::uint32_t root_page,
        const PageAnalysisOptions& options = {}) const;
    [[nodiscard]] Result<std::vector<CorruptionFinding>> analyze_relationships(
        StorageFile& file, const PageAnalysisOptions& options = {}) const;

private:
    friend class BTreeAnalyzer;
    [[nodiscard]] Result<FormatInspectionResult> inspect_format(StorageFile& file) const;
    [[nodiscard]] Result<PageAnalysisResult> inspect_page(
        StorageFile& file, std::uint32_t page_number,
        const SQLiteMetadata& metadata, std::uint64_t page_count,
        const PageAnalysisOptions& options) const;

    Runtime& runtime_;
    StorageManager owned_storage_manager_;
    FormatInspector owned_format_inspector_;
    StorageManager* storage_manager_;
    FormatInspector* format_inspector_;
    bool initialized_owned_inspector_{false};
    std::atomic<bool> initialized_{false};
};

[[nodiscard]] std::string_view sqlite_page_type_name(SQLitePageType type) noexcept;
[[nodiscard]] std::string_view corruption_type_name(CorruptionType type) noexcept;
[[nodiscard]] std::string_view corruption_severity_name(CorruptionSeverity severity) noexcept;
[[nodiscard]] std::string format_page_analysis_report(const PageAnalysisResult& result);
[[nodiscard]] std::string format_database_analysis_report(
    const DatabasePageAnalysisResult& result);

} // namespace storq
