#include <storq/format/format_inspector.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

#include <storq/core/runtime.hpp>
#include <storq/format/storq_format.hpp>

namespace storq {
namespace {

constexpr std::array<std::byte, 16> sqlite_signature{
    std::byte{'S'}, std::byte{'Q'}, std::byte{'L'}, std::byte{'i'},
    std::byte{'t'}, std::byte{'e'}, std::byte{' '}, std::byte{'f'},
    std::byte{'o'}, std::byte{'r'}, std::byte{'m'}, std::byte{'a'},
    std::byte{'t'}, std::byte{' '}, std::byte{'3'}, std::byte{0}};

StorqError format_error(ErrorCode code, std::string message) {
    return {ErrorCategory::Parsing, code, std::move(message), {}};
}

std::uint32_t read_be32(std::span<const std::byte> bytes, std::size_t offset) {
    return (std::to_integer<std::uint32_t>(bytes[offset]) << 24) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8) |
           std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

void add_validation(
    FormatInspectionResult& result, std::string name, ValidationSeverity severity,
    bool passed, std::string message, std::string warning_code = {}) {
    result.validations.push_back({name, severity, passed, message});
    if (!passed) {
        result.warnings.push_back({severity, std::move(warning_code), std::move(message)});
    }
}

bool has_failure(const FormatInspectionResult& result) {
    return std::any_of(result.validations.begin(), result.validations.end(),
        [](const FormatValidation& validation) {
            return !validation.passed &&
                   (validation.severity == ValidationSeverity::Error ||
                    validation.severity == ValidationSeverity::Critical);
        });
}

FormatStatus classify(const FormatInspectionResult& result) {
    if (std::any_of(result.validations.begin(), result.validations.end(),
            [](const FormatValidation& validation) {
                return !validation.passed &&
                       validation.severity == ValidationSeverity::Critical;
            })) {
        return FormatStatus::Corrupted;
    }
    if (has_failure(result)) {
        return FormatStatus::Malformed;
    }
    if (std::any_of(result.validations.begin(), result.validations.end(),
            [](const FormatValidation& validation) {
                return !validation.passed &&
                       validation.severity == ValidationSeverity::Warning;
            })) {
        return FormatStatus::ValidWithWarnings;
    }
    return FormatStatus::Valid;
}

} // namespace

FormatDescriptor StorqNativeParser::descriptor() const {
    return {
        DatabaseFormat::Storq,
        "storq",
        "Storq",
        "1.0",
        "Read-only inspection of Storq native database headers and metadata.",
        {"header-validation", "page-layout", "metadata-integrity", "crc32c"},
        true,
        true};
}

Result<bool> StorqNativeParser::matches(StorageFile& file) const {
    if (!file.is_open()) return format_error(ErrorCode::InvalidState, "Cannot inspect a closed file.");
    std::array<std::byte, 9> bytes{};
    auto read = file.read_at(0, bytes);
    if (!read) return read.error();
    return read.value() >= 8 &&
           bytes[0] == std::byte{'S'} && bytes[1] == std::byte{'T'} &&
           bytes[2] == std::byte{'O'} && bytes[3] == std::byte{'R'} &&
           bytes[4] == std::byte{'Q'} && bytes[5] == std::byte{'D'} &&
           bytes[6] == std::byte{'B'} && bytes[7] == std::byte{0};
}

Result<FormatInspectionResult> StorqNativeParser::inspect(StorageFile& file) const {
    FormatInspectionResult result;
    result.format = DatabaseFormat::Storq;
    result.descriptor = descriptor();
    auto metadata_path = file.metadata();
    if (!metadata_path || !metadata_path.value().path) {
        result.status = FormatStatus::Corrupted;
        result.warnings.push_back({ValidationSeverity::Critical, "storq.invalid", "Storq format has no inspectable path."});
        return result;
    }
    auto metadata = StorqFormatOpener::open(metadata_path.value().path.value(), true);
    if (!metadata) {
        result.status = FormatStatus::Corrupted;
        result.warnings.push_back({ValidationSeverity::Critical, "storq.invalid", metadata.error().message});
        return result;
    }
    result.file_size = metadata.value().physical_file_size;
    result.page_size = metadata.value().header.page_size;
    result.page_count = metadata.value().header.page_count;
    result.complete_pages = metadata.value().header.page_count;
    result.header_valid = true;
    result.structure_valid = true;
    result.format_metadata = std::make_shared<StorqFormatMetadata>(metadata.value());
    result.status = FormatStatus::Valid;
    return result;
}

SQLiteJournalMode SQLiteHeader::journal_mode() const noexcept {
    if (write_version == 1 && read_version == 1) {
        return SQLiteJournalMode::RollbackJournal;
    }
    if (write_version == 2 && read_version == 2) {
        return SQLiteJournalMode::WriteAheadLog;
    }
    return SQLiteJournalMode::Unknown;
}

bool SQLiteHeader::header_page_count_is_valid() const noexcept {
    return database_size_pages != 0 && file_change_counter == version_valid_for;
}

Result<void> FormatRegistry::register_parser(std::shared_ptr<IFormatParser> parser) {
    if (!parser || parser->format() == DatabaseFormat::Unknown) {
        return format_error(ErrorCode::InvalidArgument, "A parser must declare a supported format.");
    }
    const auto descriptor = parser->descriptor();
    if (descriptor.format != parser->format() || descriptor.identifier.empty()) {
        return format_error(
            ErrorCode::InvalidArgument, "Parser descriptor does not match its declared format.");
    }

    std::unique_lock lock(mutex_);
    const auto existing = std::find_if(parsers_.begin(), parsers_.end(),
        [&parser](const auto& registered) { return registered->format() == parser->format(); });
    if (existing != parsers_.end()) {
        return format_error(ErrorCode::DuplicateRegistration, "A parser for this format is already registered.");
    }
    parsers_.push_back(std::move(parser));
    return {};
}

Result<void> FormatRegistry::unregister_parser(DatabaseFormat format) {
    std::unique_lock lock(mutex_);
    const auto existing = std::find_if(parsers_.begin(), parsers_.end(),
        [format](const auto& parser) { return parser->format() == format; });
    if (existing == parsers_.end()) {
        return format_error(ErrorCode::NotFound, "No parser is registered for this format.");
    }
    parsers_.erase(existing);
    return {};
}

Result<std::shared_ptr<IFormatParser>> FormatRegistry::find_parser(
    DatabaseFormat format) const {
    std::shared_lock lock(mutex_);
    const auto existing = std::find_if(parsers_.begin(), parsers_.end(),
        [format](const auto& parser) { return parser->format() == format; });
    if (existing == parsers_.end()) {
        return format_error(ErrorCode::NotFound, "No parser is registered for this format.");
    }
    return *existing;
}

std::vector<FormatDescriptor> FormatRegistry::formats() const {
    std::shared_lock lock(mutex_);
    std::vector<FormatDescriptor> descriptors;
    descriptors.reserve(parsers_.size());
    for (const auto& parser : parsers_) {
        descriptors.push_back(parser->descriptor());
    }
    return descriptors;
}

Result<FormatDetectionResult> FormatRegistry::detect(StorageFile& file) const {
    if (!file.is_open()) {
        return format_error(ErrorCode::InvalidState, "Cannot detect a format on a closed file.");
    }
    std::vector<std::shared_ptr<IFormatParser>> parsers;
    {
        std::shared_lock lock(mutex_);
        parsers = parsers_;
    }

    for (const auto& parser : parsers) {
        auto match = parser->matches(file);
        if (!match) {
            return match.error();
        }
        if (match.value()) {
            auto descriptor = parser->descriptor();
            descriptor.parser_available = true;
            return FormatDetectionResult{
                parser->format(), std::move(descriptor), parser, "Recognized format."};
        }
    }
    return FormatDetectionResult{
        DatabaseFormat::Unknown, {}, nullptr, "No registered parser recognized this file."};
}

DatabaseFormat SQLiteFormatParser::format() const noexcept {
    return DatabaseFormat::SQLite;
}

FormatDescriptor SQLiteFormatParser::descriptor() const {
    return {
        DatabaseFormat::SQLite,
        "sqlite",
        "SQLite",
        "3",
        "Read-only inspection of SQLite database headers and physical page layout.",
        {"header-validation", "page-layout", "encoding", "journal-mode"},
        true,
        true};
}

Result<bool> SQLiteFormatParser::matches(StorageFile& file) const {
    if (!file.is_open()) {
        return format_error(ErrorCode::InvalidState, "Cannot inspect a closed file.");
    }
    std::array<std::byte, sqlite_signature.size()> bytes{};
    auto read = file.read_at(0, bytes);
    if (!read) {
        return read.error();
    }
    return read.value() == bytes.size() && bytes == sqlite_signature;
}

Result<SQLiteHeader> SQLiteFormatParser::parse_header(std::span<const std::byte> bytes) {
    if (bytes.size() < 100) {
        return format_error(ErrorCode::InvalidArgument, "SQLite header is truncated; 100 bytes are required.");
    }
    if (!std::equal(sqlite_signature.begin(), sqlite_signature.end(), bytes.begin())) {
        return format_error(ErrorCode::InvalidArgument, "SQLite signature is invalid.");
    }

    SQLiteHeader header;
    std::copy_n(bytes.begin(), header.signature.size(), header.signature.begin());
    header.raw_page_size =
        static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(bytes[16]) << 8) |
                                   std::to_integer<std::uint16_t>(bytes[17]));
    header.page_size = header.raw_page_size == 1 ? 65536U : header.raw_page_size;
    header.write_version = std::to_integer<std::uint8_t>(bytes[18]);
    header.read_version = std::to_integer<std::uint8_t>(bytes[19]);
    header.reserved_bytes = std::to_integer<std::uint8_t>(bytes[20]);
    header.maximum_embedded_payload_fraction = std::to_integer<std::uint8_t>(bytes[21]);
    header.minimum_embedded_payload_fraction = std::to_integer<std::uint8_t>(bytes[22]);
    header.leaf_payload_fraction = std::to_integer<std::uint8_t>(bytes[23]);
    header.file_change_counter = read_be32(bytes, 24);
    header.database_size_pages = read_be32(bytes, 28);
    header.first_freelist_trunk_page = read_be32(bytes, 32);
    header.total_freelist_pages = read_be32(bytes, 36);
    header.schema_cookie = read_be32(bytes, 40);
    header.schema_format = read_be32(bytes, 44);
    header.default_page_cache_size = read_be32(bytes, 48);
    header.largest_root_btree_page = read_be32(bytes, 52);
    header.database_encoding_raw = read_be32(bytes, 56);
    if (header.database_encoding_raw >= 1 && header.database_encoding_raw <= 3) {
        header.database_encoding = static_cast<SQLiteEncoding>(header.database_encoding_raw);
    }
    header.user_version = read_be32(bytes, 60);
    header.incremental_vacuum = read_be32(bytes, 64);
    header.application_id = read_be32(bytes, 68);
    std::copy_n(bytes.begin() + 72, header.reserved_expansion_area.size(),
                header.reserved_expansion_area.begin());
    header.version_valid_for = read_be32(bytes, 92);
    header.sqlite_library_version = read_be32(bytes, 96);
    return header;
}

bool SQLiteFormatParser::valid_page_size(std::uint32_t page_size) noexcept {
    return page_size == 65536 ||
           (page_size >= 512 && page_size <= 32768 &&
            (page_size & (page_size - 1)) == 0);
}

Result<std::uint64_t> SQLiteFormatParser::page_offset(
    std::uint32_t page_size, std::uint64_t page_number) {
    if (!valid_page_size(page_size) || page_number == 0) {
        return format_error(ErrorCode::InvalidArgument, "Page size or page number is invalid.");
    }
    const auto index = page_number - 1;
    if (index > std::numeric_limits<std::uint64_t>::max() / page_size) {
        return format_error(ErrorCode::InvalidArgument, "Page offset overflows the supported range.");
    }
    return index * page_size;
}

Result<std::uint64_t> SQLiteFormatParser::page_boundary(
    std::uint32_t page_size, std::uint64_t page_number) {
    if (!valid_page_size(page_size) || page_number == 0 ||
        page_number > std::numeric_limits<std::uint64_t>::max() / page_size) {
        return format_error(ErrorCode::InvalidArgument, "Page boundary is invalid or overflows.");
    }
    return page_number * page_size;
}

std::uint64_t SQLiteFormatParser::complete_pages(
    std::uint64_t file_size, std::uint32_t page_size) noexcept {
    return valid_page_size(page_size) ? file_size / page_size : 0;
}

Result<FormatInspectionResult> SQLiteFormatParser::inspect(StorageFile& file) const {
    const auto started = std::chrono::steady_clock::now();
    if (!file.is_open()) {
        return format_error(ErrorCode::InvalidState, "Cannot inspect a closed file.");
    }
    if (file.access() != StorageAccess::ReadOnly) {
        return format_error(ErrorCode::InvalidArgument, "Format inspection requires a read-only storage handle.");
    }

    FormatInspectionResult result;
    result.format = DatabaseFormat::SQLite;
    result.descriptor = descriptor();
    auto size = file.size();
    if (!size) {
        return size.error();
    }
    result.file_size = size.value();
    result.header_valid = false;
    if (result.file_size < 100) {
        add_validation(result, "header-length", ValidationSeverity::Error, false,
                       "SQLite header is truncated; 100 bytes are required.", "sqlite.header_truncated");
        result.status = FormatStatus::Incomplete;
        result.inspection_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return result;
    }

    std::array<std::byte, 100> bytes{};
    auto read = file.read_at(0, bytes);
    if (!read) {
        return read.error();
    }
    if (read.value() != bytes.size()) {
        add_validation(result, "header-read", ValidationSeverity::Error, false,
                       "SQLite header could not be read completely.", "sqlite.header_read");
        result.status = FormatStatus::Incomplete;
        result.inspection_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return result;
    }
    auto parsed = parse_header(bytes);
    if (!parsed) {
        return parsed.error();
    }
    const auto& header = parsed.value();
    auto metadata = std::make_shared<SQLiteMetadata>();
    metadata->header = header;
    result.format_metadata = metadata;
    result.header_valid = true;
    add_validation(result, "signature", ValidationSeverity::Information, true,
                   "SQLite format 3 signature is valid.");

    if (!valid_page_size(header.page_size)) {
        add_validation(result, "page-size", ValidationSeverity::Error, false,
                       "Page size is not a supported SQLite page size.", "sqlite.invalid_page_size");
    } else {
        result.page_size = header.page_size;
        metadata->usable_page_size =
            header.page_size >= header.reserved_bytes
                ? header.page_size - header.reserved_bytes : 0;
        result.complete_pages = complete_pages(result.file_size, header.page_size);
        result.trailing_bytes = result.file_size % header.page_size;
        metadata->physically_available_complete_pages = result.complete_pages;

        const auto usable_valid = metadata->usable_page_size >= 480;
        add_validation(result, "usable-page-size", ValidationSeverity::Error, usable_valid,
                       usable_valid ? "Usable page size is at least 480 bytes."
                                    : "Reserved bytes leave fewer than 480 usable bytes per page.",
                       "sqlite.invalid_usable_size");
        if (result.trailing_bytes != 0) {
            add_validation(result, "file-page-alignment", ValidationSeverity::Warning, false,
                           "File size is not an exact multiple of the page size.",
                           "sqlite.trailing_bytes");
        } else {
            add_validation(result, "file-page-alignment", ValidationSeverity::Information, true,
                           "File size is aligned to complete pages.");
        }
    }

    const bool read_version_valid = header.read_version == 1 || header.read_version == 2;
    const bool write_version_valid = header.write_version >= 1;
    const bool version_valid = read_version_valid && write_version_valid;
    add_validation(result, "journal-format-version",
                   version_valid ? ValidationSeverity::Information : ValidationSeverity::Error,
                   version_valid,
                   version_valid ? "Read and write format versions are supported."
                                 : "SQLite read/write format version is unsupported.",
                   "sqlite.unsupported_format_version");
    if (read_version_valid && header.write_version > 2) {
        add_validation(result, "write-format-version", ValidationSeverity::Warning, false,
                       "Database requires a newer writer; inspection remains read-only.",
                       "sqlite.newer_writer_required");
    }

    const bool fractions_valid =
        header.maximum_embedded_payload_fraction == 64 &&
        header.minimum_embedded_payload_fraction == 32 &&
        header.leaf_payload_fraction == 32;
    add_validation(result, "payload-fractions", ValidationSeverity::Error, fractions_valid,
                   fractions_valid ? "SQLite payload fractions match the required 64/32/32 values."
                                   : "SQLite payload fractions must be 64/32/32.",
                   "sqlite.invalid_payload_fractions");

    const bool encoding_valid = header.database_encoding_raw <= 3;
    add_validation(result, "database-encoding",
                   encoding_valid ? ValidationSeverity::Information : ValidationSeverity::Error,
                   encoding_valid,
                   encoding_valid ? "Database text encoding value is recognized."
                                  : "Database text encoding value is invalid.",
                   "sqlite.invalid_encoding");
    if (header.database_encoding_raw == 0) {
        add_validation(result, "database-encoding-initialized", ValidationSeverity::Warning, false,
                       "Database encoding has not been initialized.",
                       "sqlite.encoding_uninitialized");
    }

    const bool schema_format_valid = header.schema_format <= 4;
    add_validation(result, "schema-format",
                   schema_format_valid ? ValidationSeverity::Information : ValidationSeverity::Warning,
                   schema_format_valid,
                   schema_format_valid ? "Schema format is supported or uninitialized."
                                       : "Schema format number is newer than the supported range.",
                   "sqlite.unsupported_schema_format");

    const bool reserved_area_zero = std::all_of(
        header.reserved_expansion_area.begin(), header.reserved_expansion_area.end(),
        [](std::byte value) { return value == std::byte{0}; });
    add_validation(result, "reserved-expansion-area",
                   reserved_area_zero ? ValidationSeverity::Information : ValidationSeverity::Warning,
                   reserved_area_zero,
                   reserved_area_zero ? "Reserved expansion bytes are zero."
                                      : "Reserved expansion bytes are nonzero.",
                   "sqlite.reserved_expansion_nonzero");

    metadata->header_page_count_reliable = header.header_page_count_is_valid();
    metadata->header_reported_page_count = header.database_size_pages;
    if (metadata->header_page_count_reliable) {
        result.page_count = header.database_size_pages;
        add_validation(result, "header-page-count", ValidationSeverity::Information, true,
                       "In-header database page count is reliable.");
        if (valid_page_size(header.page_size) &&
            header.database_size_pages > result.complete_pages) {
            add_validation(result, "header-page-count-bounds", ValidationSeverity::Critical, false,
                           "Header declares more pages than are physically present.",
                           "sqlite.header_page_count_exceeds_file");
        } else if (valid_page_size(header.page_size) &&
                   header.database_size_pages != result.complete_pages) {
            add_validation(result, "header-page-count-match", ValidationSeverity::Warning, false,
                           "Header page count differs from the number of complete physical pages.",
                           "sqlite.header_page_count_mismatch");
        }
    } else {
        result.page_count = result.complete_pages;
        add_validation(result, "header-page-count", ValidationSeverity::Warning, false,
                       header.database_size_pages == 0
                           ? "In-header page count is zero; using complete physical pages."
                           : "Change counter and version-valid-for differ; using complete physical pages.",
                       "sqlite.header_page_count_unreliable");
    }

    const bool freelist_shape_valid =
        (header.first_freelist_trunk_page == 0) ==
        (header.total_freelist_pages == 0);
    add_validation(result, "freelist-shape", ValidationSeverity::Critical,
                   freelist_shape_valid,
                   freelist_shape_valid ? "Freelist trunk and page count agree."
                                        : "Freelist trunk and page count are inconsistent.",
                   "sqlite.invalid_freelist_shape");
    if (header.first_freelist_trunk_page > result.page_count ||
        header.total_freelist_pages > result.page_count) {
        add_validation(result, "freelist-bounds", ValidationSeverity::Critical, false,
                       "Freelist metadata exceeds the selected database page count.",
                       "sqlite.freelist_out_of_bounds");
    } else {
        add_validation(result, "freelist-bounds", ValidationSeverity::Information, true,
                       "Freelist metadata is within the selected page count.");
    }
    result.structure_valid = !has_failure(result);
    result.status = classify(result);
    if (header.read_version > 2) {
        result.status = FormatStatus::Unsupported;
    }
    result.inspection_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

FormatInspector::FormatInspector(Runtime& runtime)
    : runtime_(runtime), storage_manager_(&owned_storage_manager_) {}

FormatInspector::FormatInspector(Runtime& runtime, StorageManager& storage_manager)
    : runtime_(runtime), storage_manager_(&storage_manager) {}

FormatInspector::~FormatInspector() {
    if (initialized_.load()) {
        (void)shutdown();
    }
}

std::string_view FormatInspector::name() const noexcept {
    return "format_inspector";
}

Result<void> FormatInspector::initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true)) {
        return format_error(ErrorCode::AlreadyInitialized, "Format inspector is already initialized.");
    }
    auto sqlite_parser = registry_.find_parser(DatabaseFormat::SQLite);
    if (!sqlite_parser) {
        auto registered = registry_.register_parser(std::make_shared<SQLiteFormatParser>());
        if (!registered) {
            initialized_.store(false);
            return registered.error();
        }
        if (!registry_.find_parser(DatabaseFormat::Storq)) {
            auto registered = registry_.register_parser(std::make_shared<StorqNativeParser>());
            if (!registered) {
                initialized_.store(false);
                return registered.error();
            }
        }
    }
    static_cast<void>(runtime_.capabilities().register_capability("format_inspection"));
    static_cast<void>(
        runtime_.capabilities().register_capability("format_inspection.sqlite"));
    static_cast<void>(
        runtime_.capabilities().register_capability("format_inspection.storq"));
    return {};
}

Result<void> FormatInspector::shutdown() {
    if (!initialized_.exchange(false)) {
        return {};
    }
    return {};
}

bool FormatInspector::is_initialized() const noexcept {
    return initialized_.load();
}

Result<FormatDetectionResult> FormatInspector::detect(const std::filesystem::path& path) const {
    if (!is_initialized()) {
        return format_error(ErrorCode::InvalidState, "Format inspector is not initialized.");
    }
    StorageOpenOptions options;
    options.access = StorageAccess::ReadOnly;
    auto file = storage_manager_->open_file(path, options);
    if (!file) {
        return file.error();
    }
    return detect(*file.value());
}

Result<FormatDetectionResult> FormatInspector::detect(StorageFile& file) const {
    if (!is_initialized()) {
        return format_error(ErrorCode::InvalidState, "Format inspector is not initialized.");
    }
    if (file.access() != StorageAccess::ReadOnly) {
        return format_error(ErrorCode::InvalidArgument, "Format inspection requires a read-only storage handle.");
    }
    return registry_.detect(file);
}

Result<FormatInspectionResult> FormatInspector::inspect(
    const std::filesystem::path& path) const {
    if (!is_initialized()) {
        return format_error(ErrorCode::InvalidState, "Format inspector is not initialized.");
    }
    StorageOpenOptions options;
    options.access = StorageAccess::ReadOnly;
    auto file = storage_manager_->open_file(path, options);
    if (!file) {
        return file.error();
    }
    return inspect(*file.value());
}

Result<FormatInspectionResult> FormatInspector::inspect(StorageFile& file) const {
    auto detection = detect(file);
    if (!detection) {
        return detection.error();
    }
    if (!detection.value().supported()) {
        FormatInspectionResult result;
        result.format = DatabaseFormat::Unknown;
        result.status = FormatStatus::Unsupported;
        auto size = file.size();
        if (!size) {
            return size.error();
        }
        result.file_size = size.value();
        result.warnings.push_back(
            {ValidationSeverity::Warning, "format.unknown", detection.value().message});
        return result;
    }
    return detection.value().parser->inspect(file);
}

Result<void> FormatInspector::register_parser(std::shared_ptr<IFormatParser> parser) {
    if (parser && parser->format() == DatabaseFormat::SQLite) {
        return format_error(ErrorCode::InvalidArgument, "The built-in SQLite parser cannot be replaced.");
    }
    return registry_.register_parser(std::move(parser));
}

Result<void> FormatInspector::unregister_parser(DatabaseFormat format) {
    if (!is_initialized()) {
        return format_error(ErrorCode::InvalidState, "Format inspector is not initialized.");
    }
    if (format == DatabaseFormat::SQLite) {
        return format_error(ErrorCode::InvalidArgument, "The built-in SQLite parser cannot be removed.");
    }
    return registry_.unregister_parser(format);
}

std::vector<FormatDescriptor> FormatInspector::formats() const {
    return registry_.formats();
}

std::string_view format_name(DatabaseFormat format) noexcept {
    switch (format) {
    case DatabaseFormat::Unknown: return "Unknown";
    case DatabaseFormat::SQLite: return "SQLite";
    case DatabaseFormat::Storq: return "Storq";
    }
    return "Unknown";
}

std::string_view validation_severity_name(ValidationSeverity severity) noexcept {
    switch (severity) {
    case ValidationSeverity::Information: return "Information";
    case ValidationSeverity::Warning: return "Warning";
    case ValidationSeverity::Error: return "Error";
    case ValidationSeverity::Critical: return "Critical";
    }
    return "Unknown";
}

std::string_view format_status_name(FormatStatus status) noexcept {
    switch (status) {
    case FormatStatus::Valid: return "Valid";
    case FormatStatus::ValidWithWarnings: return "Valid with warnings";
    case FormatStatus::Unsupported: return "Unsupported";
    case FormatStatus::Malformed: return "Malformed";
    case FormatStatus::Incomplete: return "Incomplete";
    case FormatStatus::Corrupted: return "Corrupted";
    }
    return "Unknown";
}

std::string_view sqlite_encoding_name(SQLiteEncoding encoding) noexcept {
    switch (encoding) {
    case SQLiteEncoding::Unknown: return "Unknown";
    case SQLiteEncoding::UTF8: return "UTF-8";
    case SQLiteEncoding::UTF16LE: return "UTF-16LE";
    case SQLiteEncoding::UTF16BE: return "UTF-16BE";
    }
    return "Unknown";
}

std::string_view sqlite_journal_mode_name(SQLiteJournalMode mode) noexcept {
    switch (mode) {
    case SQLiteJournalMode::RollbackJournal: return "Rollback journal";
    case SQLiteJournalMode::WriteAheadLog: return "WAL";
    case SQLiteJournalMode::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::string format_inspection_report(const FormatInspectionResult& result) {
    std::ostringstream report;
    report << "Format: " << format_name(result.format) << '\n'
           << "Status: " << format_status_name(result.status) << '\n'
           << "File size: " << result.file_size << " bytes\n";
    if (result.format == DatabaseFormat::SQLite) {
        report << "Page size: " << result.page_size << " bytes\n"
               << "Pages: " << result.page_count << " ("
               << result.complete_pages << " complete physical pages)\n"
               << "Trailing bytes: " << result.trailing_bytes << '\n';
        if (const auto* sqlite = result.metadata_as<SQLiteMetadata>()) {
            report << "Encoding: " << sqlite_encoding_name(sqlite->header.database_encoding) << '\n'
                   << "Journal mode: " << sqlite_journal_mode_name(sqlite->header.journal_mode()) << '\n'
                   << "Usable page size: " << sqlite->usable_page_size << " bytes\n";
        }
    }
    for (const auto& validation : result.validations) {
        report << (validation.passed ? "[PASS] " : "[FAIL] ")
               << validation.name << ": " << validation.message << '\n';
    }
    for (const auto& warning : result.warnings) {
        report << '[' << validation_severity_name(warning.severity) << "] "
               << warning.message << '\n';
    }
    return report.str();
}

} // namespace storq
