#include <storq/wal/wal_analyzer.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

#include <storq/core/runtime.hpp>

namespace storq {
namespace {

constexpr std::uint32_t wal_magic_little_checksum = 0x377f0682U;
constexpr std::uint32_t wal_magic_big_checksum = 0x377f0683U;
constexpr std::uint32_t sqlite_wal_version = 3007000U;
constexpr std::uint64_t wal_header_size = 32;
constexpr std::uint64_t wal_frame_header_size = 24;

StorqError wal_error(ErrorCode code, std::string message) {
    return {ErrorCategory::Parsing, code, std::move(message), {}};
}

std::uint32_t read_be32(std::span<const std::byte> bytes, std::size_t offset) {
    return (std::to_integer<std::uint32_t>(bytes[offset]) << 24) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8) |
           std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

std::uint32_t read_word(
    std::span<const std::byte> bytes, std::size_t offset,
    WALChecksumByteOrder byte_order) {
    if (byte_order == WALChecksumByteOrder::BigEndian) {
        return read_be32(bytes, offset);
    }
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24);
}

void add_issue(
    WALAnalysisReport& report, WALIssueType type, WALIssueSeverity severity,
    std::optional<std::uint64_t> frame_number, std::optional<std::uint64_t> offset,
    std::string description, std::string expected, std::string observed,
    std::uint64_t maximum_issues) {
    if (report.issues.size() >= maximum_issues) {
        report.complete_analysis = false;
        return;
    }
    report.issues.push_back({
        type, severity, frame_number, offset, std::move(description),
        std::move(expected), std::move(observed), false});
}

void add_event(
    WALAnalysisReport& report, WALEventType type,
    std::optional<std::uint64_t> frame_number, std::uint64_t offset,
    std::optional<std::uint64_t> transaction_id, std::string description,
    bool valid, std::uint64_t maximum_events) {
    if (report.timeline.size() >= maximum_events) {
        report.complete_analysis = false;
        return;
    }
    report.timeline.push_back({
        report.timeline.size() + 1, type, frame_number, offset,
        std::chrono::system_clock::now(), transaction_id,
        std::move(description), valid});
}

std::string json_escape(std::string_view value) {
    std::ostringstream result;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result << "\\\""; break;
        case '\\': result << "\\\\"; break;
        case '\b': result << "\\b"; break;
        case '\f': result << "\\f"; break;
        case '\n': result << "\\n"; break;
        case '\r': result << "\\r"; break;
        case '\t': result << "\\t"; break;
        default:
            if (character < 0x20) {
                constexpr char digits[] = "0123456789abcdef";
                result << "\\u00" << digits[(character >> 4) & 0x0f]
                       << digits[character & 0x0f];
            } else {
                result << static_cast<char>(character);
            }
        }
    }
    return result.str();
}

} // namespace

Result<SQLiteWALChecksumState> SQLiteWALChecksum::calculate(
    std::span<const std::byte> bytes, WALChecksumByteOrder byte_order,
    SQLiteWALChecksumState initial) {
    if (byte_order == WALChecksumByteOrder::Unknown) {
        return wal_error(ErrorCode::InvalidArgument, "WAL checksum byte order is unknown.");
    }
    if (bytes.size() % 8 != 0) {
        return wal_error(
            ErrorCode::InvalidArgument,
            "SQLite WAL checksum input length must be divisible by eight.");
    }
    auto state = initial;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 8) {
        const auto first = read_word(bytes, offset, byte_order);
        const auto second = read_word(bytes, offset + 4, byte_order);
        state.first += first + state.second;
        state.second += second + state.first;
    }
    return state;
}

WALFormat SQLiteWALParser::format() const noexcept {
    return WALFormat::SQLite;
}

WALParserDescriptor SQLiteWALParser::descriptor() const {
    return {WALFormat::SQLite, "sqlite-wal", "SQLite WAL", "3007000", true};
}

Result<bool> SQLiteWALParser::matches(StorageFile& file) const {
    if (!file.is_open()) {
        return wal_error(ErrorCode::InvalidState, "Cannot detect WAL format on a closed file.");
    }
    std::array<std::byte, 4> bytes{};
    auto read = file.read_at(0, bytes);
    if (!read) {
        return read.error();
    }
    if (read.value() != bytes.size()) {
        return false;
    }
    const auto magic = read_be32(bytes, 0);
    return magic == wal_magic_little_checksum || magic == wal_magic_big_checksum;
}

Result<SQLiteWALHeader> SQLiteWALParser::parse_header(
    std::span<const std::byte> bytes) const {
    if (bytes.size() < wal_header_size) {
        return wal_error(ErrorCode::InvalidArgument, "SQLite WAL header is truncated.");
    }
    SQLiteWALHeader header;
    header.magic = read_be32(bytes, 0);
    header.format_version = read_be32(bytes, 4);
    header.page_size = read_be32(bytes, 8);
    header.checkpoint_sequence = read_be32(bytes, 12);
    header.salt_1 = read_be32(bytes, 16);
    header.salt_2 = read_be32(bytes, 20);
    header.checksum_1 = read_be32(bytes, 24);
    header.checksum_2 = read_be32(bytes, 28);
    if (header.magic == wal_magic_little_checksum) {
        header.checksum_byte_order = WALChecksumByteOrder::LittleEndian;
    } else if (header.magic == wal_magic_big_checksum) {
        header.checksum_byte_order = WALChecksumByteOrder::BigEndian;
    }
    return header;
}

Result<SQLiteWALFrameHeader> SQLiteWALParser::parse_frame_header(
    std::span<const std::byte> bytes) const {
    if (bytes.size() < wal_frame_header_size) {
        return wal_error(ErrorCode::InvalidArgument, "SQLite WAL frame header is truncated.");
    }
    return SQLiteWALFrameHeader{
        read_be32(bytes, 0), read_be32(bytes, 4), read_be32(bytes, 8),
        read_be32(bytes, 12), read_be32(bytes, 16), read_be32(bytes, 20)};
}

Result<WALFrameInspectionResult> SQLiteWALParser::inspect_frame(
    StorageFile& file, const SQLiteWALHeader& header,
    std::uint64_t frame_number, SQLiteWALChecksumState previous_checksum) const {
    if (!file.is_open() || file.access() != StorageAccess::ReadOnly) {
        return wal_error(ErrorCode::InvalidArgument, "WAL frame inspection requires an open read-only file.");
    }
    if (frame_number == 0 || !SQLiteFormatParser::valid_page_size(header.page_size)) {
        return wal_error(ErrorCode::InvalidArgument, "WAL frame number or page size is invalid.");
    }
    const auto frame_size = wal_frame_header_size +
                            static_cast<std::uint64_t>(header.page_size);
    if (frame_number - 1 >
        (std::numeric_limits<std::uint64_t>::max() - wal_header_size) / frame_size) {
        return wal_error(ErrorCode::InvalidArgument, "WAL frame offset overflows.");
    }
    const auto frame_offset = wal_header_size + (frame_number - 1) * frame_size;
    const auto page_offset = frame_offset + wal_frame_header_size;
    if (page_offset < frame_offset || page_offset > std::numeric_limits<std::uint64_t>::max() -
                                                       header.page_size) {
        return wal_error(ErrorCode::InvalidArgument, "WAL page-data offset overflows.");
    }
    auto size = file.size();
    if (!size) {
        return size.error();
    }
    if (frame_offset > size.value() ||
        frame_size > size.value() - frame_offset) {
        return wal_error(ErrorCode::InvalidArgument, "WAL frame is incomplete or truncated.");
    }
    std::array<std::byte, wal_frame_header_size> header_bytes{};
    auto header_read = file.read_at(frame_offset, header_bytes);
    if (!header_read) {
        return header_read.error();
    }
    if (header_read.value() != header_bytes.size()) {
        return wal_error(ErrorCode::IoError, "WAL frame header could not be read completely.");
    }
    auto frame_header = parse_frame_header(header_bytes);
    if (!frame_header) {
        return frame_header.error();
    }

    std::vector<std::byte> page_data(header.page_size);
    auto page_read = file.read_at(page_offset, page_data);
    if (!page_read) {
        return page_read.error();
    }
    if (page_read.value() != page_data.size()) {
        return wal_error(ErrorCode::IoError, "WAL frame page data could not be read completely.");
    }
    std::vector<std::byte> checksum_input;
    checksum_input.reserve(8 + page_data.size());
    checksum_input.insert(checksum_input.end(), header_bytes.begin(), header_bytes.begin() + 8);
    checksum_input.insert(checksum_input.end(), page_data.begin(), page_data.end());
    auto checksum = SQLiteWALChecksum::calculate(
        checksum_input, header.checksum_byte_order, previous_checksum);
    if (!checksum) {
        return checksum.error();
    }

    WALFrameInspectionResult result;
    result.frame.frame_number = frame_number;
    result.frame.file_offset = frame_offset;
    result.frame.header = frame_header.value();
    result.frame.calculated_checksum = checksum.value();
    result.frame.salts_valid =
        result.frame.header.salt_1 == header.salt_1 &&
        result.frame.header.salt_2 == header.salt_2;
    result.frame.checksum_valid =
        checksum.value().first == result.frame.header.checksum_1 &&
        checksum.value().second == result.frame.header.checksum_2;
    result.page_data = std::move(page_data);
    return result;
}

Result<WALAnalysisReport> SQLiteWALParser::analyze(
    StorageFile& file, const WALAnalysisOptions& options,
    const SQLiteMetadata* database_metadata) const {
    const auto started = std::chrono::steady_clock::now();
    if (!file.is_open() || file.access() != StorageAccess::ReadOnly) {
        return wal_error(ErrorCode::InvalidArgument, "WAL analysis requires an open read-only file.");
    }
    auto file_size = file.size();
    if (!file_size) {
        return file_size.error();
    }

    WALAnalysisReport report;
    report.file_size = file_size.value();
    if (report.file_size < wal_header_size) {
        add_issue(report, WALIssueType::TruncatedHeader, WALIssueSeverity::Error,
                  std::nullopt, 0, "WAL header is shorter than 32 bytes.",
                  "32 bytes", std::to_string(report.file_size),
                  options.limits.maximum_issues);
        report.complete_analysis = false;
        report.analysis_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return report;
    }

    std::array<std::byte, wal_header_size> header_bytes{};
    auto header_read = file.read_at(0, header_bytes);
    if (!header_read) {
        return header_read.error();
    }
    if (header_read.value() != header_bytes.size()) {
        add_issue(report, WALIssueType::TruncatedHeader, WALIssueSeverity::Error,
                  std::nullopt, 0, "WAL header could not be read completely.",
                  "32 readable bytes", std::to_string(header_read.value()),
                  options.limits.maximum_issues);
        report.complete_analysis = false;
        return report;
    }
    auto parsed_header = parse_header(header_bytes);
    if (!parsed_header) {
        return parsed_header.error();
    }
    report.header = parsed_header.value();
    const bool valid_magic =
        report.header.magic == wal_magic_little_checksum ||
        report.header.magic == wal_magic_big_checksum;
    if (!valid_magic) {
        add_issue(report, WALIssueType::InvalidMagic, WALIssueSeverity::Critical,
                  std::nullopt, 0, "WAL magic number is unsupported.",
                  "0x377f0682 or 0x377f0683",
                  std::to_string(report.header.magic), options.limits.maximum_issues);
    }
    if (report.header.format_version != sqlite_wal_version) {
        add_issue(report, WALIssueType::UnsupportedVersion, WALIssueSeverity::Error,
                  std::nullopt, 4, "WAL format version is unsupported.",
                  std::to_string(sqlite_wal_version),
                  std::to_string(report.header.format_version), options.limits.maximum_issues);
    }
    if (!SQLiteFormatParser::valid_page_size(report.header.page_size)) {
        add_issue(report, WALIssueType::InvalidPageSize, WALIssueSeverity::Critical,
                  std::nullopt, 8, "WAL page size is not a supported SQLite page size.",
                  "512..65536, power of two",
                  std::to_string(report.header.page_size), options.limits.maximum_issues);
    }
    if (database_metadata &&
        database_metadata->header.page_size != report.header.page_size) {
        add_issue(report, WALIssueType::InvalidPageSize, WALIssueSeverity::Error,
                  std::nullopt, 8, "WAL page size disagrees with the database header.",
                  std::to_string(database_metadata->header.page_size),
                  std::to_string(report.header.page_size), options.limits.maximum_issues);
    }
    bool checksum_header_valid = false;
    if (valid_magic && report.header.format_version == sqlite_wal_version &&
        SQLiteFormatParser::valid_page_size(report.header.page_size)) {
        auto checksum = SQLiteWALChecksum::calculate(
            std::span<const std::byte>(header_bytes).first(24),
            report.header.checksum_byte_order);
        if (!checksum) {
            return checksum.error();
        }
        checksum_header_valid =
            checksum.value().first == report.header.checksum_1 &&
            checksum.value().second == report.header.checksum_2;
        if (!checksum_header_valid) {
            add_issue(report, WALIssueType::InvalidHeaderChecksum, WALIssueSeverity::Critical,
                      std::nullopt, 24, "WAL header checksum does not match.",
                      std::to_string(checksum.value().first) + "," +
                          std::to_string(checksum.value().second),
                      std::to_string(report.header.checksum_1) + "," +
                          std::to_string(report.header.checksum_2),
                      options.limits.maximum_issues);
        }
    }
    report.header_valid = checksum_header_valid &&
                          valid_magic &&
                          report.header.format_version == sqlite_wal_version &&
                          SQLiteFormatParser::valid_page_size(report.header.page_size) &&
                          (!database_metadata ||
                           database_metadata->header.page_size == report.header.page_size);
    report.checksums_valid = report.header_valid;
    report.checkpoint = CheckpointAnalyzer::analyze(report.header, {});
    add_event(report, WALEventType::HeaderValidated, std::nullopt, 0,
              std::nullopt,
              report.header_valid ? "WAL header checksum and fields validated."
                                  : "WAL header validation failed or was unsupported.",
              report.header_valid, options.limits.maximum_timeline_events);
    add_event(report, WALEventType::CheckpointMetadataObserved, std::nullopt, 12,
              std::nullopt,
              "Checkpoint sequence and WAL salts observed; exact checkpoint state is unknown.",
              true, options.limits.maximum_timeline_events);

    if (!valid_magic || !SQLiteFormatParser::valid_page_size(report.header.page_size)) {
        report.structurally_valid = false;
        report.analysis_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return report;
    }
    if (report.header.format_version != sqlite_wal_version) {
        report.structurally_valid = false;
        report.complete_analysis = false;
        report.analysis_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        return report;
    }

    const auto frame_size = wal_frame_header_size +
                            static_cast<std::uint64_t>(report.header.page_size);
    const auto trailing_size = report.file_size > wal_header_size
        ? (report.file_size - wal_header_size) % frame_size : 0;
    const auto physically_present_frames =
        (report.file_size - wal_header_size) / frame_size;
    report.total_frames = physically_present_frames;
    if (trailing_size != 0) {
        const auto remaining = report.file_size - wal_header_size -
                               physically_present_frames * frame_size;
        add_issue(report, WALIssueType::TruncatedFrame, WALIssueSeverity::Warning,
                  physically_present_frames + 1,
                  wal_header_size + physically_present_frames * frame_size,
                  "Trailing WAL data does not contain a complete frame.",
                  std::to_string(frame_size) + " bytes",
                  std::to_string(remaining) + " bytes",
                  options.limits.maximum_issues);
        report.complete_analysis = false;
        add_event(report, WALEventType::InvalidFrame,
                  physically_present_frames + 1,
                  wal_header_size + physically_present_frames * frame_size,
                  std::nullopt, "Incomplete trailing frame observed.", false,
                  options.limits.maximum_timeline_events);
    }

    const auto max_frames_by_memory =
        options.limits.maximum_memory_bytes / sizeof(SQLiteWALFrame);
    const auto per_retained_frame =
        sizeof(SQLiteWALFrame) + sizeof(WALTransaction) +
        3 * sizeof(WALEvent) + 4 * sizeof(WALIssue);
    const auto scan_count = std::min({
        physically_present_frames,
        options.limits.maximum_frames,
        std::min(max_frames_by_memory,
                 options.limits.maximum_memory_bytes / per_retained_frame),
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())});
    report.frames.reserve(static_cast<std::size_t>(scan_count));
    auto checksum_state = SQLiteWALChecksumState{0, 0};
    auto initial_checksum = SQLiteWALChecksum::calculate(
        std::span<const std::byte>(header_bytes).first(24),
        report.header.checksum_byte_order);
    if (initial_checksum) {
        checksum_state = initial_checksum.value();
    }
    bool any_frame_checksum_invalid = false;
    for (std::uint64_t number = 1; number <= scan_count; ++number) {
        const auto frame_offset = wal_header_size + (number - 1) * frame_size;
        std::array<std::byte, wal_frame_header_size> frame_header_bytes{};
        auto frame_header_read = file.read_at(frame_offset, frame_header_bytes);
        if (!frame_header_read) {
            return frame_header_read.error();
        }
        if (frame_header_read.value() != frame_header_bytes.size()) {
            add_issue(report, WALIssueType::TruncatedFrame, WALIssueSeverity::Warning,
                      number, frame_offset, "WAL frame header is truncated.",
                      "24 bytes", std::to_string(frame_header_read.value()),
                      options.limits.maximum_issues);
            report.complete_analysis = false;
            break;
        }
        auto parsed_frame_header = parse_frame_header(frame_header_bytes);
        if (!parsed_frame_header) {
            return parsed_frame_header.error();
        }
        std::vector<std::byte> page_data(report.header.page_size);
        auto page_read = file.read_at(frame_offset + wal_frame_header_size, page_data);
        if (!page_read) {
            return page_read.error();
        }
        if (page_read.value() != page_data.size()) {
            add_issue(report, WALIssueType::TruncatedFrame, WALIssueSeverity::Warning,
                      number, frame_offset, "WAL frame page data is truncated.",
                      std::to_string(page_data.size()) + " bytes",
                      std::to_string(page_read.value()),
                      options.limits.maximum_issues);
            report.complete_analysis = false;
            break;
        }
        std::vector<std::byte> checksum_input;
        checksum_input.reserve(8 + page_data.size());
        checksum_input.insert(
            checksum_input.end(), frame_header_bytes.begin(), frame_header_bytes.begin() + 8);
        checksum_input.insert(checksum_input.end(), page_data.begin(), page_data.end());
        auto calculated = SQLiteWALChecksum::calculate(
            checksum_input, report.header.checksum_byte_order, checksum_state);
        if (!calculated) {
            return calculated.error();
        }
        SQLiteWALFrame frame;
        frame.frame_number = number;
        frame.file_offset = frame_offset;
        frame.header = parsed_frame_header.value();
        frame.calculated_checksum = calculated.value();
        frame.salts_valid =
            frame.header.salt_1 == report.header.salt_1 &&
            frame.header.salt_2 == report.header.salt_2;
        frame.checksum_valid =
            frame.calculated_checksum.first == frame.header.checksum_1 &&
            frame.calculated_checksum.second == frame.header.checksum_2;
        if (frame.header.page_number == 0) {
            add_issue(report, WALIssueType::InvalidPageNumber, WALIssueSeverity::Critical,
                      number, frame_offset, "WAL frame page number is zero.",
                      "nonzero page number", "0", options.limits.maximum_issues);
        }
        if (!frame.salts_valid) {
            add_issue(report, WALIssueType::InvalidSalt, WALIssueSeverity::Critical,
                      number, frame_offset + 8, "WAL frame salts differ from the header.",
                      std::to_string(report.header.salt_1) + "," +
                          std::to_string(report.header.salt_2),
                      std::to_string(frame.header.salt_1) + "," +
                          std::to_string(frame.header.salt_2),
                      options.limits.maximum_issues);
        }
        if (!frame.checksum_valid) {
            any_frame_checksum_invalid = true;
            add_issue(report, WALIssueType::InvalidFrameChecksum, WALIssueSeverity::Critical,
                      number, frame_offset + 16, "WAL frame checksum does not match.",
                      std::to_string(frame.calculated_checksum.first) + "," +
                          std::to_string(frame.calculated_checksum.second),
                      std::to_string(frame.header.checksum_1) + "," +
                          std::to_string(frame.header.checksum_2),
                      options.limits.maximum_issues);
        }
        if (database_metadata &&
            frame.header.page_number > database_metadata->physically_available_complete_pages &&
            frame.header.database_size == 0) {
            add_issue(report, WALIssueType::InvalidPageNumber, WALIssueSeverity::Warning,
                      number, frame_offset, "WAL page number exceeds the current database file size.",
                      "page within database file or committed growth",
                      std::to_string(frame.header.page_number),
                      options.limits.maximum_issues);
        }
        if (frame.header.database_size != 0 &&
            database_metadata &&
            frame.header.database_size >
                database_metadata->physically_available_complete_pages +
                    physically_present_frames) {
            add_issue(report, WALIssueType::InvalidDatabaseSize, WALIssueSeverity::Warning,
                      number, frame_offset + 4,
                      "Commit database size is unusually larger than the database and WAL frame count.",
                      "plausible committed page count",
                      std::to_string(frame.header.database_size),
                      options.limits.maximum_issues);
        }
        if (frame.salts_valid && frame.checksum_valid && frame.header.page_number != 0) {
            ++report.valid_frames;
        } else {
            ++report.invalid_frames;
        }
        add_event(report, WALEventType::FrameParsed, number, frame_offset,
                  std::nullopt, "WAL frame parsed.", true,
                  options.limits.maximum_timeline_events);
        add_event(report, WALEventType::FrameChecksumVerified, number,
                  frame_offset + 16, std::nullopt,
                  frame.checksum_valid ? "Frame rolling checksum verified."
                                       : "Frame rolling checksum failed.",
                  frame.checksum_valid, options.limits.maximum_timeline_events);
        if (!frame.checksum_valid || !frame.salts_valid ||
            frame.header.page_number == 0) {
            add_event(report, WALEventType::InvalidFrame, number, frame_offset,
                      std::nullopt, "WAL frame contains a validation failure.",
                      false, options.limits.maximum_timeline_events);
        }
        checksum_state = frame.calculated_checksum;
        report.frames.push_back(frame);
        if (frame.is_commit()) {
            report.last_commit_frame = number;
        }
        if (options.progress && !options.progress(number, physically_present_frames)) {
            report.complete_analysis = false;
            add_issue(report, WALIssueType::IncompleteTransaction,
                      WALIssueSeverity::Warning, number, frame_offset,
                      "WAL scan was cancelled by the progress callback.",
                      "complete scan", "cancelled",
                      options.limits.maximum_issues);
            break;
        }
    }
    if (scan_count < physically_present_frames) {
        report.complete_analysis = false;
        add_issue(report, WALIssueType::InvalidFrameBoundary, WALIssueSeverity::Warning,
                  scan_count + 1, wal_header_size + scan_count * frame_size,
                  "WAL scan stopped at the configured frame or memory limit.",
                  std::to_string(physically_present_frames) + " frames",
                  std::to_string(scan_count) + " scanned",
                  options.limits.maximum_issues);
    }

    report.transactions = TransactionAnalyzer::analyze(
        report.frames, options.limits.maximum_transactions, report.issues,
        options.limits.maximum_issues);
    report.committed_transactions = static_cast<std::uint64_t>(std::count_if(
        report.frames.begin(), report.frames.end(),
        [](const SQLiteWALFrame& frame) { return frame.is_commit(); }));
    report.incomplete_transactions =
        !report.frames.empty() && !report.frames.back().is_commit() ? 1 : 0;
    std::uint64_t timeline_transaction_id = 0;
    for (const auto& frame : report.frames) {
        if (frame.is_commit()) {
            ++timeline_transaction_id;
            add_event(report, WALEventType::TransactionCommitted,
                      frame.frame_number, frame.file_offset,
                      timeline_transaction_id,
                      "Physical WAL transaction committed with database size " +
                          std::to_string(frame.header.database_size) + ".",
                      frame.checksum_valid && frame.salts_valid,
                      options.limits.maximum_timeline_events);
        }
    }
    if (!report.frames.empty() && !report.frames.back().is_commit()) {
        add_event(report, WALEventType::IncompleteTransaction,
                  report.frames.back().frame_number, report.frames.back().file_offset,
                  timeline_transaction_id + 1,
                  "Frames remain after the final commit boundary.", false,
                  options.limits.maximum_timeline_events);
    }
    report.checkpoint = CheckpointAnalyzer::analyze(report.header, report.transactions);
    report.checksums_valid = checksum_header_valid && !any_frame_checksum_invalid;
    report.structurally_valid = report.header_valid &&
        report.invalid_frames == 0 && trailing_size == 0 &&
        report.incomplete_transactions == 0;
    report.analysis_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return report;
}

Result<void> WALRegistry::register_parser(std::shared_ptr<IWALParser> parser) {
    if (!parser || parser->format() == WALFormat::Unknown) {
        return wal_error(ErrorCode::InvalidArgument, "WAL parser must declare a supported format.");
    }
    auto descriptor = parser->descriptor();
    if (descriptor.format != parser->format() || descriptor.identifier.empty()) {
        return wal_error(ErrorCode::InvalidArgument, "WAL parser descriptor is invalid.");
    }
    std::unique_lock lock(mutex_);
    const auto exists = std::find_if(parsers_.begin(), parsers_.end(),
        [&parser](const auto& registered) {
            return registered->format() == parser->format();
        });
    if (exists != parsers_.end()) {
        return wal_error(ErrorCode::DuplicateRegistration, "WAL parser is already registered.");
    }
    parsers_.push_back(std::move(parser));
    return {};
}

Result<void> WALRegistry::unregister_parser(WALFormat format) {
    std::unique_lock lock(mutex_);
    const auto existing = std::find_if(parsers_.begin(), parsers_.end(),
        [format](const auto& parser) { return parser->format() == format; });
    if (existing == parsers_.end()) {
        return wal_error(ErrorCode::NotFound, "WAL parser is not registered.");
    }
    parsers_.erase(existing);
    return {};
}

Result<std::shared_ptr<IWALParser>> WALRegistry::find_parser(WALFormat format) const {
    std::shared_lock lock(mutex_);
    const auto existing = std::find_if(parsers_.begin(), parsers_.end(),
        [format](const auto& parser) { return parser->format() == format; });
    if (existing == parsers_.end()) {
        return wal_error(ErrorCode::NotFound, "No parser is registered for this WAL format.");
    }
    return *existing;
}

std::vector<WALParserDescriptor> WALRegistry::parsers() const {
    std::shared_lock lock(mutex_);
    std::vector<WALParserDescriptor> descriptors;
    descriptors.reserve(parsers_.size());
    for (const auto& parser : parsers_) {
        descriptors.push_back(parser->descriptor());
    }
    return descriptors;
}

Result<std::shared_ptr<IWALParser>> WALRegistry::detect(StorageFile& file) const {
    if (!file.is_open()) {
        return wal_error(ErrorCode::InvalidState, "Cannot detect a WAL format on a closed file.");
    }
    std::vector<std::shared_ptr<IWALParser>> parsers;
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
            return parser;
        }
    }
    return wal_error(ErrorCode::NotFound, "No registered WAL parser recognized this file.");
}

Result<WALFormat> WALDetector::detect(const WALRegistry& registry, StorageFile& file) {
    auto parser = registry.detect(file);
    if (!parser) {
        if (parser.error().code == ErrorCode::NotFound) {
            return WALFormat::Unknown;
        }
        return parser.error();
    }
    return parser.value()->format();
}

std::vector<std::uint64_t> FrameAnalyzer::page_history(
    const WALAnalysisReport& report, std::uint32_t page_number) {
    std::vector<std::uint64_t> history;
    for (const auto& frame : report.frames) {
        if (frame.header.page_number == page_number) {
            history.push_back(frame.frame_number);
        }
    }
    return history;
}

std::vector<WALIssue> FrameAnalyzer::validate_sequence(
    const WALAnalysisReport& report, std::uint64_t maximum_issues) {
    std::vector<WALIssue> issues;
    for (std::size_t index = 0; index < report.frames.size(); ++index) {
        const auto& frame = report.frames[index];
        if (frame.frame_number != index + 1) {
            if (issues.size() < maximum_issues) {
                issues.push_back({
                    WALIssueType::InvalidFrameBoundary, WALIssueSeverity::Error,
                    frame.frame_number, frame.file_offset,
                    "Frame sequence number is not contiguous.",
                    std::to_string(index + 1), std::to_string(frame.frame_number), false});
            }
        }
    }
    return issues;
}

std::vector<WALTransaction> TransactionAnalyzer::analyze(
    const std::vector<SQLiteWALFrame>& frames, std::uint64_t maximum_transactions,
    std::vector<WALIssue>& issues, std::uint64_t maximum_issues) {
    std::vector<WALTransaction> transactions;
    std::optional<WALTransaction> current;
    const auto add_incomplete = [&issues, maximum_issues](const WALTransaction& transaction) {
        if (issues.size() < maximum_issues) {
            issues.push_back({
                WALIssueType::IncompleteTransaction, WALIssueSeverity::Warning,
                transaction.last_frame, std::nullopt,
                "WAL frame group is not terminated by a commit frame.",
                "nonzero database-size commit frame", "no commit frame", false});
        }
    };
    for (const auto& frame : frames) {
        if (!current) {
            current = WALTransaction{};
            current->transaction_id = transactions.size() + 1;
            current->first_frame = frame.frame_number;
        }
        current->last_frame = frame.frame_number;
        ++current->frame_count;
        current->complete = current->complete || frame.frame_number == current->first_frame;
        if (!frame.salts_valid || !frame.checksum_valid || frame.header.page_number == 0) {
            current->complete = false;
        }
        if (frame.is_commit()) {
            current->committed = true;
            current->committed_database_pages = frame.header.database_size;
            if (transactions.size() < maximum_transactions) {
                transactions.push_back(*current);
            } else if (issues.size() < maximum_issues) {
                issues.push_back({
                    WALIssueType::InvalidTransactionBoundary, WALIssueSeverity::Warning,
                    frame.frame_number, frame.file_offset,
                    "Transaction retention limit was reached.",
                    std::to_string(maximum_transactions) + " transactions",
                    "additional transaction", false});
            }
            current.reset();
        }
    }
    if (current) {
        current->committed = false;
        current->complete = false;
        add_incomplete(*current);
        if (transactions.size() < maximum_transactions) {
            transactions.push_back(*current);
        }
    }
    return transactions;
}

WALCheckpointMetadata CheckpointAnalyzer::analyze(
    const SQLiteWALHeader& header, const std::vector<WALTransaction>& transactions) {
    WALCheckpointMetadata metadata;
    metadata.checkpoint_sequence = header.checkpoint_sequence;
    metadata.salt_1 = header.salt_1;
    metadata.salt_2 = header.salt_2;
    metadata.exact_checkpoint_state_known = false;
    for (const auto& transaction : transactions) {
        if (transaction.committed) {
            metadata.last_commit_frame = transaction.last_frame;
            metadata.database_pages_at_last_commit =
                transaction.committed_database_pages;
        }
    }
    return metadata;
}

std::vector<WALEvent> WALTimeline::reconstruct(
    const WALAnalysisReport& report, std::uint64_t maximum_events) {
    if (report.timeline.size() <= maximum_events) {
        return report.timeline;
    }
    return {report.timeline.begin(),
            report.timeline.begin() + static_cast<std::ptrdiff_t>(maximum_events)};
}

WALAnalyzer::WALAnalyzer(Runtime& runtime)
    : runtime_(runtime), storage_manager_(&owned_storage_manager_) {}

WALAnalyzer::WALAnalyzer(Runtime& runtime, StorageManager& storage_manager)
    : runtime_(runtime), storage_manager_(&storage_manager) {}

WALAnalyzer::~WALAnalyzer() {
    if (initialized_.load()) {
        (void)shutdown();
    }
}

std::string_view WALAnalyzer::name() const noexcept {
    return "WALAnalyzer";
}

Result<void> WALAnalyzer::initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true)) {
        return wal_error(ErrorCode::AlreadyInitialized, "WAL analyzer is already initialized.");
    }
    auto registered = registry_.find_parser(WALFormat::SQLite);
    if (!registered) {
        auto result = registry_.register_parser(std::make_shared<SQLiteWALParser>());
        if (!result) {
            initialized_.store(false);
            return result.error();
        }
    }
    static_cast<void>(runtime_.capabilities().register_capability("wal_analysis"));
    static_cast<void>(runtime_.capabilities().register_capability("wal_analysis.sqlite"));
    return {};
}

Result<void> WALAnalyzer::shutdown() {
    initialized_.store(false);
    return {};
}

bool WALAnalyzer::is_initialized() const noexcept {
    return initialized_.load();
}

Result<void> WALAnalyzer::register_parser(std::shared_ptr<IWALParser> parser) {
    if (parser && parser->format() == WALFormat::SQLite) {
        return wal_error(ErrorCode::InvalidArgument, "Built-in SQLite WAL parser cannot be replaced.");
    }
    return registry_.register_parser(std::move(parser));
}

Result<WALFormat> WALAnalyzer::detect(StorageFile& file) const {
    if (!is_initialized()) {
        return wal_error(ErrorCode::InvalidState, "WAL analyzer is not initialized.");
    }
    if (file.access() != StorageAccess::ReadOnly) {
        return wal_error(ErrorCode::InvalidArgument, "WAL analysis requires a read-only storage handle.");
    }
    return WALDetector::detect(registry_, file);
}

Result<WALAnalysisReport> WALAnalyzer::inspect(
    const std::filesystem::path& path, const WALAnalysisOptions& options,
    const SQLiteMetadata* database_metadata) const {
    if (!is_initialized()) {
        return wal_error(ErrorCode::InvalidState, "WAL analyzer is not initialized.");
    }
    StorageOpenOptions open_options;
    open_options.access = StorageAccess::ReadOnly;
    auto file = storage_manager_->open_file(path, open_options);
    if (!file) {
        return file.error();
    }
    return inspect(*file.value(), options, database_metadata);
}

Result<WALAnalysisReport> WALAnalyzer::inspect(
    StorageFile& file, const WALAnalysisOptions& options,
    const SQLiteMetadata* database_metadata) const {
    if (!is_initialized()) {
        return wal_error(ErrorCode::InvalidState, "WAL analyzer is not initialized.");
    }
    if (file.access() != StorageAccess::ReadOnly) {
        return wal_error(ErrorCode::InvalidArgument, "WAL analysis requires a read-only storage handle.");
    }
    auto parser = registry_.detect(file);
    if (!parser) {
        if (parser.error().code != ErrorCode::NotFound) {
            return parser.error();
        }
        SQLiteWALParser sqlite_parser;
        return sqlite_parser.analyze(file, options, database_metadata);
    }
    return parser.value()->analyze(file, options, database_metadata);
}

Result<WALFrameInspectionResult> WALAnalyzer::inspect_frame(
    StorageFile& file, std::uint64_t frame_number,
    const SQLiteMetadata* database_metadata) const {
    auto report = inspect(file, {}, database_metadata);
    if (!report) {
        return report.error();
    }
    if (frame_number == 0 || frame_number > report.value().frames.size()) {
        return wal_error(ErrorCode::NotFound, "Requested frame was not analyzed.");
    }
    const auto parser = registry_.find_parser(WALFormat::SQLite);
    if (!parser) {
        return parser.error();
    }
    if (parser.value()->format() != WALFormat::SQLite) {
        return wal_error(ErrorCode::InvalidState, "SQLite WAL parser is not available.");
    }
    SQLiteWALParser sqlite;
    std::array<std::byte, 32> full_header{};
    auto full_header_read = file.read_at(0, full_header);
    if (!full_header_read) {
        return full_header_read.error();
    }
    auto parsed = sqlite.parse_header(full_header);
    if (!parsed) {
        return parsed.error();
    }
    auto initial = SQLiteWALChecksum::calculate(
        std::span<const std::byte>(full_header).first(24),
        parsed.value().checksum_byte_order);
    if (!initial) {
        return initial.error();
    }
    auto previous = initial.value();
    if (frame_number > 1) {
        previous = report.value().frames[static_cast<std::size_t>(frame_number - 2)]
                       .calculated_checksum;
    }
    return sqlite.inspect_frame(file, parsed.value(), frame_number, previous);
}

Result<std::vector<std::uint64_t>> WALAnalyzer::page_history(
    StorageFile& file, std::uint32_t page_number,
    const WALAnalysisOptions& options,
    const SQLiteMetadata* database_metadata) const {
    if (page_number == 0) {
        return wal_error(ErrorCode::InvalidArgument, "Page number must be nonzero.");
    }
    auto report = inspect(file, options, database_metadata);
    if (!report) {
        return report.error();
    }
    return FrameAnalyzer::page_history(report.value(), page_number);
}

std::vector<WALParserDescriptor> WALAnalyzer::parsers() const {
    return registry_.parsers();
}

std::string_view wal_format_name(WALFormat format) noexcept {
    switch (format) {
    case WALFormat::Unknown: return "Unknown";
    case WALFormat::SQLite: return "SQLite WAL";
    case WALFormat::Storq: return "Storq WAL";
    }
    return "Unknown";
}

std::string_view wal_checksum_byte_order_name(WALChecksumByteOrder order) noexcept {
    switch (order) {
    case WALChecksumByteOrder::Unknown: return "Unknown";
    case WALChecksumByteOrder::LittleEndian: return "Little-endian checksum words";
    case WALChecksumByteOrder::BigEndian: return "Big-endian checksum words";
    }
    return "Unknown";
}

std::string_view wal_issue_type_name(WALIssueType type) noexcept {
    switch (type) {
    case WALIssueType::InvalidMagic: return "Invalid magic";
    case WALIssueType::UnsupportedVersion: return "Unsupported version";
    case WALIssueType::InvalidPageSize: return "Invalid page size";
    case WALIssueType::InvalidHeaderChecksum: return "Invalid header checksum";
    case WALIssueType::InvalidFrameChecksum: return "Invalid frame checksum";
    case WALIssueType::InvalidSalt: return "Invalid salt";
    case WALIssueType::InvalidPageNumber: return "Invalid page number";
    case WALIssueType::TruncatedHeader: return "Truncated header";
    case WALIssueType::TruncatedFrame: return "Truncated frame";
    case WALIssueType::InvalidFrameBoundary: return "Invalid frame boundary";
    case WALIssueType::InvalidTransactionBoundary: return "Invalid transaction boundary";
    case WALIssueType::InvalidDatabaseSize: return "Invalid database size";
    case WALIssueType::IncompleteTransaction: return "Incomplete transaction";
    case WALIssueType::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::string_view wal_issue_severity_name(WALIssueSeverity severity) noexcept {
    switch (severity) {
    case WALIssueSeverity::Information: return "Information";
    case WALIssueSeverity::Warning: return "Warning";
    case WALIssueSeverity::Error: return "Error";
    case WALIssueSeverity::Critical: return "Critical";
    }
    return "Unknown";
}

std::string_view wal_event_type_name(WALEventType type) noexcept {
    switch (type) {
    case WALEventType::HeaderValidated: return "Header validated";
    case WALEventType::FrameParsed: return "Frame parsed";
    case WALEventType::FrameChecksumVerified: return "Frame checksum verified";
    case WALEventType::TransactionCommitted: return "Transaction committed";
    case WALEventType::IncompleteTransaction: return "Incomplete transaction";
    case WALEventType::InvalidFrame: return "Invalid frame";
    case WALEventType::CheckpointMetadataObserved: return "Checkpoint metadata observed";
    }
    return "Unknown";
}

std::string format_wal_report(const WALAnalysisReport& report) {
    std::ostringstream output;
    output << "Format: SQLite WAL\n"
           << "Magic: 0x" << std::hex << report.header.magic << std::dec << '\n'
           << "Format version: " << report.header.format_version << '\n'
           << "Page size: " << report.header.page_size << '\n'
           << "Checkpoint sequence: " << report.header.checkpoint_sequence << '\n'
           << "Checksum word order: "
           << wal_checksum_byte_order_name(report.header.checksum_byte_order) << '\n'
           << "File size: " << report.file_size << " bytes\n"
           << "Frames: " << report.total_frames << " total, "
           << report.valid_frames << " valid, " << report.invalid_frames << " invalid\n"
           << "Transactions: " << report.committed_transactions << " committed, "
           << report.incomplete_transactions << " incomplete\n"
           << "Last commit frame: " << report.last_commit_frame << '\n'
           << "Header valid: " << (report.header_valid ? "yes" : "no") << '\n'
           << "Checksums valid: " << (report.checksums_valid ? "yes" : "no") << '\n'
           << "Complete analysis: " << (report.complete_analysis ? "yes" : "no") << '\n';
    for (const auto& event : report.timeline) {
        output << "Event " << event.event_id << " [" << wal_event_type_name(event.type)
               << "] " << event.description << '\n';
    }
    for (const auto& issue : report.issues) {
        output << '[' << wal_issue_severity_name(issue.severity) << "] "
               << wal_issue_type_name(issue.type) << ": "
               << issue.description << '\n';
    }
    return output.str();
}

std::string export_wal_report_json(const WALAnalysisReport& report) {
    std::ostringstream output;
    output << "{"
           << "\"format\":\"sqlite-wal\","
           << "\"file_size\":" << report.file_size << ','
           << "\"total_frames\":" << report.total_frames << ','
           << "\"valid_frames\":" << report.valid_frames << ','
           << "\"invalid_frames\":" << report.invalid_frames << ','
           << "\"committed_transactions\":" << report.committed_transactions << ','
           << "\"incomplete_transactions\":" << report.incomplete_transactions << ','
           << "\"last_commit_frame\":" << report.last_commit_frame << ','
           << "\"header_valid\":" << (report.header_valid ? "true" : "false") << ','
           << "\"checksums_valid\":" << (report.checksums_valid ? "true" : "false") << ','
           << "\"complete_analysis\":" << (report.complete_analysis ? "true" : "false") << ','
           << "\"transactions\":[";
    for (std::size_t index = 0; index < report.transactions.size(); ++index) {
        if (index != 0) output << ',';
        const auto& transaction = report.transactions[index];
        output << "{\"id\":" << transaction.transaction_id
               << ",\"first_frame\":" << transaction.first_frame
               << ",\"last_frame\":" << transaction.last_frame
               << ",\"frame_count\":" << transaction.frame_count
               << ",\"committed_database_pages\":"
               << transaction.committed_database_pages
               << ",\"committed\":" << (transaction.committed ? "true" : "false")
               << ",\"complete\":" << (transaction.complete ? "true" : "false")
               << '}';
    }
    output << "],\"issues\":[";
    for (std::size_t index = 0; index < report.issues.size(); ++index) {
        if (index != 0) output << ',';
        const auto& issue = report.issues[index];
        output << "{\"type\":\"" << wal_issue_type_name(issue.type)
               << "\",\"severity\":\"" << wal_issue_severity_name(issue.severity)
               << "\",\"description\":\"" << json_escape(issue.description)
               << "\",\"expected\":\"" << json_escape(issue.expected)
               << "\",\"observed\":\"" << json_escape(issue.observed)
               << "\",\"valid\":" << (issue.valid ? "true" : "false") << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace storq
