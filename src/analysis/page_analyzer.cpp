#include <storq/analysis/page_analyzer.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <storq/core/runtime.hpp>

namespace storq {
namespace {

StorqError analysis_error(ErrorCode code, std::string message) {
    return {ErrorCategory::Parsing, code, std::move(message), {}};
}

std::uint16_t read_be16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(bytes[offset]) << 8) |
        std::to_integer<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t read_be32(std::span<const std::byte> bytes, std::size_t offset) {
    return (std::to_integer<std::uint32_t>(bytes[offset]) << 24) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8) |
           std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t effective_page_count(
    const SQLiteMetadata& metadata, std::uint64_t physical_pages) {
    if (metadata.header_page_count_reliable &&
        metadata.header_reported_page_count != 0 &&
        metadata.header_reported_page_count <= physical_pages) {
        return metadata.header_reported_page_count;
    }
    return physical_pages;
}

std::uint64_t estimated_page_analysis_memory(
    std::uint32_t page_size, std::uint32_t maximum_cells) {
    const auto cell_limit = std::min<std::uint64_t>(
        maximum_cells, page_size / 2U);
    const auto freeblock_limit = page_size / 4U;
    return static_cast<std::uint64_t>(page_size) +
           cell_limit * (sizeof(CellPointer) + sizeof(CellStructure)) +
           static_cast<std::uint64_t>(freeblock_limit) * sizeof(Freeblock);
}

void append_local_finding(
    PageAnalysisResult& page, CorruptionType type, std::uint32_t offset,
    std::string description, std::string expected, std::string observed,
    const PageAnalysisOptions& options, bool confirmed = true) {
    CorruptionDetector::add(page, type, page.page_number, offset,
                            std::move(description), std::move(expected),
                            std::move(observed), confirmed, options.limits.maximum_findings);
}

bool overlaps(
    std::uint32_t first_begin, std::uint32_t first_end,
    std::uint32_t second_begin, std::uint32_t second_end) {
    return first_begin < second_end && second_begin < first_end;
}

} // namespace

bool SQLitePageHeader::is_leaf() const noexcept {
    return page_type == SQLitePageType::LeafIndex ||
           page_type == SQLitePageType::LeafTable;
}

bool SQLitePageHeader::is_interior() const noexcept {
    return page_type == SQLitePageType::InteriorIndex ||
           page_type == SQLitePageType::InteriorTable;
}

bool SQLitePageHeader::contains_index_records() const noexcept {
    return page_type == SQLitePageType::InteriorIndex ||
           page_type == SQLitePageType::LeafIndex;
}

bool SQLitePageHeader::contains_table_records() const noexcept {
    return page_type == SQLitePageType::InteriorTable ||
           page_type == SQLitePageType::LeafTable;
}

PageReader::PageReader(StorageFile& file) : file_(file) {}

Result<std::vector<std::byte>> PageReader::read_page(
    std::uint32_t page_number, std::uint32_t page_size,
    std::uint64_t page_count) const {
    if (!file_.is_open()) {
        return analysis_error(ErrorCode::InvalidState, "Page reader has no open storage file.");
    }
    if (page_number == 0 || page_number > page_count ||
        !SQLiteFormatParser::valid_page_size(page_size)) {
        return analysis_error(ErrorCode::InvalidArgument, "Page number or size is outside the database.");
    }
    auto offset = SQLiteFormatParser::page_offset(page_size, page_number);
    if (!offset) {
        return offset.error();
    }
    auto file_size = file_.size();
    if (!file_size) {
        return file_size.error();
    }
    const auto page_end = offset.value() + page_size;
    if (page_end < offset.value() || page_end > file_size.value()) {
        return analysis_error(ErrorCode::InvalidArgument, "Requested page is truncated or missing.");
    }
    std::vector<std::byte> page(page_size);
    auto read = file_.read_at(offset.value(), page);
    if (!read) {
        return read.error();
    }
    if (read.value() != page_size) {
        return analysis_error(ErrorCode::IoError, "Page read returned fewer bytes than requested.");
    }
    return page;
}

Result<SQLitePageHeader> SQLitePageHeaderParser::parse(
    std::uint32_t page_number, std::span<const std::byte> page,
    std::uint32_t page_size) {
    if (page_number == 0 || !SQLiteFormatParser::valid_page_size(page_size) ||
        page.size() < page_size) {
        return analysis_error(ErrorCode::InvalidArgument, "Page buffer or page number is invalid.");
    }
    const std::uint32_t header_offset = page_number == 1 ? 100U : 0U;
    if (page_size < header_offset + 8 || page.size() < header_offset + 8) {
        return analysis_error(ErrorCode::InvalidArgument, "B-tree page header is truncated.");
    }
    const auto raw_type = std::to_integer<std::uint8_t>(page[header_offset]);
    SQLitePageHeader header;
    header.header_offset = header_offset;
    switch (raw_type) {
    case 0x02: header.page_type = SQLitePageType::InteriorIndex; break;
    case 0x05: header.page_type = SQLitePageType::InteriorTable; break;
    case 0x0a: header.page_type = SQLitePageType::LeafIndex; break;
    case 0x0d: header.page_type = SQLitePageType::LeafTable; break;
    default: return header;
    }
    header.header_size = header.is_interior() ? 12U : 8U;
    if (page_size < header_offset + header.header_size ||
        page.size() < header_offset + header.header_size) {
        return analysis_error(ErrorCode::InvalidArgument, "B-tree page header is truncated.");
    }
    header.first_freeblock_offset = read_be16(page, header_offset + 1);
    header.cell_count = read_be16(page, header_offset + 3);
    header.cell_content_area_offset = read_be16(page, header_offset + 5);
    header.fragmented_free_bytes =
        std::to_integer<std::uint8_t>(page[header_offset + 7]);
    if (header.is_interior()) {
        header.rightmost_child_page = read_be32(page, header_offset + 8);
    }
    return header;
}

Result<SQLiteVarint> SQLiteCellParser::decode_varint(
    std::span<const std::byte> bytes, std::size_t offset) {
    if (offset >= bytes.size()) {
        return analysis_error(ErrorCode::InvalidArgument, "SQLite varint starts outside the buffer.");
    }
    std::uint64_t value = 0;
    for (std::uint8_t index = 0; index < 8; ++index) {
        const auto position = offset + index;
        if (position >= bytes.size()) {
            return analysis_error(ErrorCode::InvalidArgument, "SQLite varint is truncated.");
        }
        const auto current = std::to_integer<std::uint8_t>(bytes[position]);
        value = (value << 7) | (current & 0x7fU);
        if ((current & 0x80U) == 0) {
            return SQLiteVarint{value, static_cast<std::uint8_t>(index + 1)};
        }
    }
    if (offset + 8 >= bytes.size()) {
        return analysis_error(ErrorCode::InvalidArgument, "Nine-byte SQLite varint is truncated.");
    }
    value = (value << 8) | std::to_integer<std::uint8_t>(bytes[offset + 8]);
    return SQLiteVarint{value, 9};
}

std::uint32_t SQLiteCellParser::maximum_local_payload(
    SQLitePageType type, std::uint32_t usable_size) noexcept {
    if (usable_size < 12) {
        return 0;
    }
    if (type == SQLitePageType::LeafTable) {
        return usable_size - 35;
    }
    if (type == SQLitePageType::LeafIndex ||
        type == SQLitePageType::InteriorIndex) {
        return ((usable_size - 12) * 64U) / 255U - 23U;
    }
    return 0;
}

std::uint32_t SQLiteCellParser::minimum_local_payload(
    std::uint32_t usable_size) noexcept {
    if (usable_size < 12) {
        return 0;
    }
    return ((usable_size - 12) * 32U) / 255U - 23U;
}

Result<std::uint32_t> SQLiteCellParser::local_payload_size(
    SQLitePageType type, std::uint32_t usable_size, std::uint64_t payload_size) {
    const auto maximum = maximum_local_payload(type, usable_size);
    const auto minimum = minimum_local_payload(usable_size);
    if (maximum == 0 || minimum == 0) {
        return analysis_error(ErrorCode::InvalidArgument, "Usable page size is too small for payload calculation.");
    }
    if (payload_size <= maximum) {
        return static_cast<std::uint32_t>(payload_size);
    }
    const auto modulus = static_cast<std::uint64_t>(usable_size - 4);
    if (modulus == 0) {
        return analysis_error(ErrorCode::InvalidArgument, "Usable page size cannot hold overflow metadata.");
    }
    const auto local = static_cast<std::uint64_t>(minimum) +
                       ((payload_size - minimum) % modulus);
    return static_cast<std::uint32_t>(local <= maximum ? local : minimum);
}

Result<OverflowAnalysisResult> SQLiteOverflowParser::analyze(
    StorageFile& file, std::uint32_t source_page,
    std::uint32_t first_overflow_page, std::uint64_t expected_payload_bytes,
    std::uint32_t page_size, std::uint32_t usable_size,
    std::uint64_t page_count, const PageAnalysisOptions& options) {
    if (file.access() != StorageAccess::ReadOnly) {
        return analysis_error(ErrorCode::InvalidArgument, "Overflow inspection requires a read-only file.");
    }
    if (page_size > options.limits.maximum_memory_bytes ||
        !SQLiteFormatParser::valid_page_size(page_size) ||
        usable_size < 4 || usable_size > page_size) {
        return analysis_error(ErrorCode::InvalidArgument, "Overflow page parameters exceed safe bounds.");
    }
    OverflowAnalysisResult result;
    result.reference = {
        source_page, first_overflow_page, expected_payload_bytes,
        expected_payload_bytes, 0, false};
    PageReader reader(file);
    std::unordered_set<std::uint32_t> visited;
    auto current = first_overflow_page;
    auto remaining = expected_payload_bytes;
    while (current != 0 && remaining > 0 &&
           result.reference.pages_traversed < options.limits.maximum_overflow_chain_depth) {
        if (current == source_page || !visited.insert(current).second) {
            result.findings.push_back({
                CorruptionType::OverflowCycle,
                CorruptionDetector::severity(CorruptionType::OverflowCycle),
                source_page, std::nullopt,
                "Overflow chain contains a cycle or refers to its source page.",
                "acyclic chain without source page", std::to_string(current), true});
            break;
        }
        if (current > page_count) {
            result.findings.push_back({
                CorruptionType::InvalidOverflowReference,
                CorruptionDetector::severity(CorruptionType::InvalidOverflowReference),
                source_page, std::nullopt,
                "Overflow chain references a missing page.",
                "page number within database", std::to_string(current), true});
            break;
        }
        auto page = reader.read_page(current, page_size, page_count);
        if (!page) {
            result.findings.push_back({
                CorruptionType::InvalidOverflowReference,
                CorruptionDetector::severity(CorruptionType::InvalidOverflowReference),
                source_page, std::nullopt,
                "Overflow page could not be read completely.",
                "complete page available", page.error().message, true});
            break;
        }
        const auto next = read_be32(page.value(), 0);
        if (result.relationships.size() < options.limits.maximum_relationship_nodes) {
            result.relationships.push_back(
                {current, next, PageRelationship::Type::OverflowNext});
        } else {
            result.findings.push_back({
                CorruptionType::InvalidPageRelationship,
                CorruptionDetector::severity(CorruptionType::InvalidPageRelationship),
                current, 0,
                "Overflow relationship count reached its configured safety limit.",
                "relationships within configured limit",
                std::to_string(options.limits.maximum_relationship_nodes), false});
            break;
        }
        ++result.reference.pages_traversed;
        const auto capacity = usable_size - 4U;
        remaining = remaining > capacity ? remaining - capacity : 0;
        if (next != 0 && (next > page_count || next == current)) {
            result.findings.push_back({
                CorruptionType::InvalidOverflowReference,
                CorruptionDetector::severity(CorruptionType::InvalidOverflowReference),
                current, 0, "Overflow page next pointer is invalid.",
                "zero or existing different page", std::to_string(next), true});
            break;
        }
        if (remaining == 0 && next != 0) {
            result.findings.push_back({
                CorruptionType::InvalidOverflowReference,
                CorruptionDetector::severity(CorruptionType::InvalidOverflowReference),
                current, 0, "Overflow chain continues after all declared payload bytes.",
                "zero next pointer on final required page", std::to_string(next), false});
            break;
        }
        current = next;
    }
    result.reference.remaining_payload_bytes = remaining;
    result.reference.complete = remaining == 0 && result.findings.empty();
    if (remaining != 0 && result.findings.empty()) {
        result.findings.push_back({
            CorruptionType::InvalidOverflowReference,
            CorruptionDetector::severity(CorruptionType::InvalidOverflowReference),
            source_page, std::nullopt,
            result.reference.pages_traversed >= options.limits.maximum_overflow_chain_depth
                ? "Overflow traversal reached its configured safety limit."
                : "Overflow chain ended before carrying the declared payload.",
            "chain terminates after satisfying payload",
            std::to_string(remaining) + " payload bytes remain",
            result.reference.pages_traversed < options.limits.maximum_overflow_chain_depth});
    }
    return result;
}

CorruptionSeverity CorruptionDetector::severity(CorruptionType type) noexcept {
    switch (type) {
    case CorruptionType::InvalidPageType:
    case CorruptionType::UnreachablePage:
    case CorruptionType::Unknown:
        return CorruptionSeverity::Low;
    case CorruptionType::InvalidFreeblock:
    case CorruptionType::InvalidSpaceAccounting:
    case CorruptionType::InvalidPageRelationship:
        return CorruptionSeverity::Medium;
    case CorruptionType::InvalidPageHeader:
    case CorruptionType::InvalidCellPointer:
    case CorruptionType::InvalidCellBoundary:
    case CorruptionType::OverlappingCells:
    case CorruptionType::InvalidVarint:
    case CorruptionType::InvalidChildReference:
    case CorruptionType::InvalidOverflowReference:
        return CorruptionSeverity::High;
    case CorruptionType::FreeblockCycle:
    case CorruptionType::OverflowCycle:
        return CorruptionSeverity::Critical;
    }
    return CorruptionSeverity::Low;
}

void CorruptionDetector::add(
    PageAnalysisResult& result, CorruptionType type, std::uint32_t page_number,
    std::optional<std::uint32_t> byte_offset, std::string description,
    std::string expected, std::string observed, bool confirmed,
    std::uint64_t maximum_findings) {
    if (result.findings.size() >= maximum_findings) {
        return;
    }
    result.findings.push_back({
        type, severity(type), page_number, byte_offset, std::move(description),
        std::move(expected), std::move(observed), confirmed});
}

Result<PageAnalysisResult> SQLiteBTreePageParser::parse(
    std::uint32_t page_number, std::span<const std::byte> page,
    std::uint32_t page_size, std::uint32_t usable_size,
    std::uint64_t page_count, const PageAnalysisOptions& options) {
    auto parsed_header = SQLitePageHeaderParser::parse(page_number, page, page_size);
    if (!parsed_header) {
        return parsed_header.error();
    }
    PageAnalysisResult result;
    result.page_number = page_number;
    result.header = parsed_header.value();
    result.page_type = result.header.page_type;
    if (result.page_type == SQLitePageType::Unknown) {
        result.supported = false;
        result.message = "Page is not a supported SQLite B-tree page type.";
        return result;
    }
    if (estimated_page_analysis_memory(
            page_size, options.limits.maximum_cells_per_page) >
        options.limits.maximum_memory_bytes) {
        return analysis_error(
            ErrorCode::InvalidArgument,
            "Configured memory limit is too small for bounded page analysis.");
    }

    const auto usable_limit = std::min<std::uint32_t>(usable_size, page_size);
    const auto content_offset = result.header.cell_content_area_offset == 0 &&
                                page_size == 65536
                                    ? 65536U
                                    : result.header.cell_content_area_offset;
    const auto pointer_start = result.header.header_offset + result.header.header_size;
    const auto add_relationship = [&result, &options](PageRelationship relationship) {
        if (result.relationships.size() < options.limits.maximum_relationship_nodes) {
            result.relationships.push_back(relationship);
        } else {
            result.complete_analysis = false;
        }
    };
    const auto pointer_bytes =
        static_cast<std::uint64_t>(result.header.cell_count) * 2U;
    const auto pointer_end64 = static_cast<std::uint64_t>(pointer_start) + pointer_bytes;
    const bool pointer_boundary_valid =
        result.header.cell_count <= options.limits.maximum_cells_per_page &&
        pointer_end64 <= usable_limit;
    if (!pointer_boundary_valid) {
        append_local_finding(
            result, CorruptionType::InvalidPageHeader, pointer_start,
            "Cell pointer array exceeds configured or physical page bounds.",
            "pointer array ends within usable page", std::to_string(pointer_end64),
            options);
    }
    if (result.header.cell_count > options.limits.maximum_cells_per_page) {
        result.supported = false;
        result.message = "Cell count exceeds configured per-page safety limit.";
        return result;
    }
    if (content_offset < pointer_end64 || content_offset > usable_limit ||
        content_offset < result.header.header_offset + result.header.header_size) {
        append_local_finding(
            result, CorruptionType::InvalidPageHeader,
            result.header.header_offset + 5,
            "Cell content area offset is outside the valid page region.",
            "content offset is after pointer array and within usable size",
            std::to_string(content_offset), options);
    }
    if (result.header.fragmented_free_bytes > 60) {
        append_local_finding(
            result, CorruptionType::InvalidPageHeader,
            result.header.header_offset + 7,
            "Fragmented free-byte count exceeds SQLite's maximum of 60.",
            "0..60", std::to_string(result.header.fragmented_free_bytes), options);
    }

    result.space.usable_size = usable_limit;
    result.space.header_size = result.header.header_offset + result.header.header_size;
    result.space.pointer_array_size = static_cast<std::uint32_t>(pointer_bytes);
    result.space.cell_content_size =
        content_offset <= usable_limit ? usable_limit - content_offset : 0;
    result.space.fragmented_bytes = result.header.fragmented_free_bytes;
    result.space.unallocated_bytes =
        content_offset >= pointer_end64
            ? static_cast<std::uint32_t>(content_offset - pointer_end64)
            : 0;

    if (!pointer_boundary_valid ||
        content_offset < pointer_end64 || content_offset > usable_limit) {
        result.structurally_valid = false;
        return result;
    }

    std::unordered_set<std::uint16_t> unique_pointers;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cell_ranges;
    result.cell_pointers.reserve(result.header.cell_count);
    result.cells.reserve(result.header.cell_count);
    for (std::uint32_t index = 0; index < result.header.cell_count; ++index) {
        const auto array_offset = pointer_start + index * 2U;
        const auto cell_offset = read_be16(page, array_offset);
        result.cell_pointers.push_back({array_offset, cell_offset});
        if (!unique_pointers.insert(cell_offset).second) {
            append_local_finding(
                result, CorruptionType::InvalidCellPointer, array_offset,
                "Duplicate cell pointer detected.", "unique cell offsets",
                std::to_string(cell_offset), options);
            continue;
        }
        if (cell_offset < content_offset || cell_offset >= usable_limit ||
            cell_offset < pointer_end64) {
            append_local_finding(
                result, CorruptionType::InvalidCellPointer, array_offset,
                "Cell pointer lies outside the cell content region.",
                "offset within cell content area", std::to_string(cell_offset), options);
            continue;
        }

        CellStructure cell;
        cell.cell_offset = cell_offset;
        std::size_t cursor = cell_offset;
        const auto page_end = static_cast<std::size_t>(usable_limit);
        const auto require_bytes = [&page_end, &cursor](std::size_t length) {
            return cursor <= page_end && length <= page_end - cursor;
        };
        bool valid = true;
        if (result.header.is_interior()) {
            if (!require_bytes(4)) {
                valid = false;
            } else {
                const auto child = read_be32(page, cursor);
                cell.left_child_page = child;
                add_relationship(
                    {page_number, child, PageRelationship::Type::BTreeChild});
                if (child == 0 || child > page_count || child == page_number) {
                    append_local_finding(
                        result, CorruptionType::InvalidChildReference,
                        static_cast<std::uint32_t>(cursor),
                        "Interior cell references an invalid child page.",
                        "existing page distinct from source",
                        std::to_string(child), options);
                }
                cursor += 4;
            }
        }

        if (valid && result.page_type == SQLitePageType::InteriorTable) {
            auto rowid = SQLiteCellParser::decode_varint(page.first(page_end), cursor);
            if (!rowid) {
                valid = false;
                append_local_finding(
                    result, CorruptionType::InvalidVarint,
                    static_cast<std::uint32_t>(cursor),
                    "Interior table rowid varint is invalid or truncated.",
                    "complete SQLite varint", rowid.error().message, options);
            } else {
                cell.rowid = rowid.value().value;
                cursor += rowid.value().bytes;
            }
        } else if (valid) {
            if (result.page_type == SQLitePageType::InteriorIndex ||
                result.page_type == SQLitePageType::LeafIndex ||
                result.page_type == SQLitePageType::LeafTable) {
                auto payload = SQLiteCellParser::decode_varint(page.first(page_end), cursor);
                if (!payload) {
                    valid = false;
                    append_local_finding(
                        result, CorruptionType::InvalidVarint,
                        static_cast<std::uint32_t>(cursor),
                        "Cell payload-size varint is invalid or truncated.",
                        "complete SQLite varint", payload.error().message, options);
                } else {
                    cell.payload_size = payload.value().value;
                    cursor += payload.value().bytes;
                }
                if (valid && result.page_type == SQLitePageType::LeafTable) {
                    auto rowid = SQLiteCellParser::decode_varint(page.first(page_end), cursor);
                    if (!rowid) {
                        valid = false;
                        append_local_finding(
                            result, CorruptionType::InvalidVarint,
                            static_cast<std::uint32_t>(cursor),
                            "Leaf table rowid varint is invalid or truncated.",
                            "complete SQLite varint", rowid.error().message, options);
                    } else {
                        cell.rowid = rowid.value().value;
                        cursor += rowid.value().bytes;
                    }
                }
                if (valid) {
                    auto local_payload = SQLiteCellParser::local_payload_size(
                        result.page_type, usable_limit, cell.payload_size);
                    if (!local_payload) {
                        valid = false;
                    } else {
                        cell.local_payload_size = local_payload.value();
                        cell.local_payload_offset = static_cast<std::uint32_t>(cursor);
                        if (!require_bytes(cell.local_payload_size)) {
                            valid = false;
                        } else {
                            cursor += cell.local_payload_size;
                            if (cell.payload_size > cell.local_payload_size) {
                                if (!require_bytes(4)) {
                                    valid = false;
                                } else {
                                    const auto overflow = read_be32(page, cursor);
                                    cell.first_overflow_page = overflow;
                                    add_relationship(
                                        {page_number, overflow, PageRelationship::Type::OverflowNext});
                                    if (overflow == 0 || overflow > page_count ||
                                        overflow == page_number) {
                                        append_local_finding(
                                            result, CorruptionType::InvalidOverflowReference,
                                            static_cast<std::uint32_t>(cursor),
                                            "Cell references an invalid first overflow page.",
                                            "existing page distinct from source",
                                            std::to_string(overflow), options);
                                    }
                                    result.overflow_references.push_back(
                                        {page_number, overflow,
                                         cell.payload_size - cell.local_payload_size,
                                         cell.payload_size - cell.local_payload_size,
                                         0, false});
                                    cursor += 4;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (!valid || cursor <= cell_offset || cursor > page_end) {
            append_local_finding(
                result, CorruptionType::InvalidCellBoundary,
                cell_offset, "Cell extends outside the usable page boundary.",
                "cell ends within usable page", std::to_string(cursor), options);
            continue;
        }
        cell.cell_size = static_cast<std::uint32_t>(cursor - cell_offset);
        cell.valid = true;
        result.space.cell_bytes += cell.cell_size;
        cell_ranges.emplace_back(cell_offset, static_cast<std::uint32_t>(cursor));
        result.cells.push_back(std::move(cell));
    }

    if (result.header.is_interior()) {
        const auto child = result.header.rightmost_child_page;
        add_relationship(
            {page_number, child, PageRelationship::Type::BTreeChild});
        if (child == 0 || child > page_count || child == page_number) {
            append_local_finding(
                result, CorruptionType::InvalidChildReference,
                result.header.header_offset + 8,
                "Right-most child page reference is invalid.",
                "existing page distinct from source", std::to_string(child), options);
        }
    }

    std::sort(cell_ranges.begin(), cell_ranges.end());
    for (std::size_t i = 1; i < cell_ranges.size(); ++i) {
        if (overlaps(cell_ranges[i - 1].first, cell_ranges[i - 1].second,
                     cell_ranges[i].first, cell_ranges[i].second)) {
            append_local_finding(
                result, CorruptionType::OverlappingCells, cell_ranges[i].first,
                "Parsed cell structures overlap.", "disjoint cell byte ranges",
                std::to_string(cell_ranges[i - 1].first) + ".." +
                    std::to_string(cell_ranges[i - 1].second) + " overlaps " +
                    std::to_string(cell_ranges[i].first) + ".." +
                    std::to_string(cell_ranges[i].second), options);
        }
    }

    std::unordered_set<std::uint16_t> freeblock_offsets;
    auto freeblock_offset = result.header.first_freeblock_offset;
    while (freeblock_offset != 0) {
        if (!freeblock_offsets.insert(freeblock_offset).second) {
            append_local_finding(
                result, CorruptionType::FreeblockCycle, freeblock_offset,
                "Freeblock chain contains a cycle.", "acyclic linked list",
                std::to_string(freeblock_offset), options);
            break;
        }
        if (freeblock_offset < content_offset ||
            freeblock_offset > usable_limit ||
            static_cast<std::uint32_t>(freeblock_offset) + 4U > usable_limit) {
            append_local_finding(
                result, CorruptionType::InvalidFreeblock, freeblock_offset,
                "Freeblock header is outside the cell content region.",
                "four-byte header within usable page",
                std::to_string(freeblock_offset), options);
            break;
        }
        const auto next = read_be16(page, freeblock_offset);
        const auto size = read_be16(page, freeblock_offset + 2);
        const auto end = static_cast<std::uint32_t>(freeblock_offset) + size;
        if (next != 0 && freeblock_offsets.contains(next)) {
            append_local_finding(
                result, CorruptionType::FreeblockCycle, freeblock_offset,
                "Freeblock chain contains a cycle.", "acyclic linked list",
                std::to_string(next), options);
            break;
        }
        if (size < 4 || end > usable_limit ||
            (next != 0 && next <= freeblock_offset)) {
            append_local_finding(
                result, CorruptionType::InvalidFreeblock, freeblock_offset,
                "Freeblock size or next pointer is invalid.",
                "size >= 4, in bounds, and ascending next pointer",
                "size=" + std::to_string(size) + ", next=" + std::to_string(next),
                options);
            result.freeblocks.push_back({freeblock_offset, size, next, false});
            break;
        }
        Freeblock block{freeblock_offset, size, next, true};
        for (const auto& range : cell_ranges) {
            if (overlaps(freeblock_offset, end, range.first, range.second)) {
                block.valid = false;
                append_local_finding(
                    result, CorruptionType::InvalidFreeblock, freeblock_offset,
                    "Freeblock overlaps a parsed cell.", "disjoint freeblock and cell ranges",
                    std::to_string(range.first) + ".." + std::to_string(range.second),
                    options);
                break;
            }
        }
        for (const auto& previous : result.freeblocks) {
            if (overlaps(freeblock_offset, end, previous.offset,
                         static_cast<std::uint32_t>(previous.offset) + previous.size)) {
                block.valid = false;
                append_local_finding(
                    result, CorruptionType::InvalidFreeblock, freeblock_offset,
                    "Freeblock overlaps another freeblock.",
                    "non-overlapping freeblock ranges",
                    std::to_string(previous.offset), options);
                break;
            }
        }
        result.freeblocks.push_back(block);
        if (block.valid) {
            result.space.freeblock_bytes += size;
        }
        freeblock_offset = next;
    }

    const auto max_cell_start = cell_ranges.empty()
        ? usable_limit : std::min_element(
            cell_ranges.begin(), cell_ranges.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; })->first;
    if (max_cell_start < content_offset) {
        append_local_finding(
            result, CorruptionType::InvalidSpaceAccounting, content_offset,
            "Cell content begins before the declared content-area offset.",
            "cell data at or after content offset", std::to_string(max_cell_start), options);
    }
    result.space.accounted_bytes =
        result.space.header_size + result.space.pointer_array_size +
        result.space.unallocated_bytes + result.space.cell_bytes +
        result.space.freeblock_bytes + result.space.fragmented_bytes;
    if (result.space.accounted_bytes > result.space.usable_size) {
        append_local_finding(
            result, CorruptionType::InvalidSpaceAccounting, result.header.header_offset,
            "Page space accounting exceeds usable bytes.",
            "accounted bytes <= usable bytes",
            std::to_string(result.space.accounted_bytes), options);
    } else {
        result.space.unaccounted_bytes =
            result.space.usable_size - result.space.accounted_bytes;
    }
    SQLitePageValidator::validate(result, options);
    result.structurally_valid = std::none_of(
        result.findings.begin(), result.findings.end(),
        [](const CorruptionFinding& finding) { return finding.confirmed; });
    return result;
}

void SQLitePageValidator::validate(
    PageAnalysisResult& result, const PageAnalysisOptions& options) {
    if (!result.supported) {
        return;
    }
    if (result.cell_pointers.size() != result.header.cell_count) {
        CorruptionDetector::add(
            result, CorruptionType::InvalidPageHeader, result.page_number,
            result.header.header_offset + 3,
            "Not all declared cell pointers could be read.",
            std::to_string(result.header.cell_count) + " pointers",
            std::to_string(result.cell_pointers.size()) + " pointers",
            true, options.limits.maximum_findings);
    }
    if (result.space.accounted_bytes > result.space.usable_size) {
        CorruptionDetector::add(
            result, CorruptionType::InvalidSpaceAccounting, result.page_number,
            std::nullopt, "Page accounting exceeds usable bytes.",
            "accounted bytes <= usable bytes",
            std::to_string(result.space.accounted_bytes), true,
            options.limits.maximum_findings);
    }
    if (result.space.unaccounted_bytes != 0) {
        CorruptionDetector::add(
            result, CorruptionType::InvalidSpaceAccounting, result.page_number,
            std::nullopt, "Page contains bytes outside the recognized allocation regions.",
            "zero unaccounted bytes",
            std::to_string(result.space.unaccounted_bytes) + " bytes",
            false, options.limits.maximum_findings);
    }
    if (result.header.contains_table_records()) {
        std::optional<std::int64_t> previous_key;
        for (const auto& cell : result.cells) {
            if (!cell.rowid) {
                continue;
            }
            const auto key = std::bit_cast<std::int64_t>(*cell.rowid);
            if (previous_key && key <= *previous_key) {
                CorruptionDetector::add(
                    result, CorruptionType::InvalidPageRelationship,
                    result.page_number, cell.cell_offset,
                    "Table B-tree rowids are not strictly increasing.",
                    "ascending rowid keys", std::to_string(key), true,
                    options.limits.maximum_findings);
                break;
            }
            previous_key = key;
        }
    }
}

PageAnalyzer::PageAnalyzer(Runtime& runtime)
    : runtime_(runtime),
      owned_format_inspector_(runtime, owned_storage_manager_),
      storage_manager_(&owned_storage_manager_),
      format_inspector_(&owned_format_inspector_) {}

PageAnalyzer::PageAnalyzer(
    Runtime& runtime, StorageManager& storage_manager,
    FormatInspector& format_inspector)
    : runtime_(runtime),
      owned_format_inspector_(runtime, owned_storage_manager_),
      storage_manager_(&storage_manager),
      format_inspector_(&format_inspector) {}

PageAnalyzer::~PageAnalyzer() {
    if (initialized_.load()) {
        (void)shutdown();
    }
}

std::string_view PageAnalyzer::name() const noexcept {
    return "PageAnalyzer";
}

Result<void> PageAnalyzer::initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true)) {
        return analysis_error(ErrorCode::AlreadyInitialized, "Page analyzer is already initialized.");
    }
    if (!format_inspector_->is_initialized()) {
        auto initialized = format_inspector_->initialize();
        if (!initialized) {
            initialized_.store(false);
            return initialized.error();
        }
        initialized_owned_inspector_ = format_inspector_ == &owned_format_inspector_;
    }
    static_cast<void>(runtime_.capabilities().register_capability("page_analysis"));
    static_cast<void>(runtime_.capabilities().register_capability("page_analysis.sqlite"));
    return {};
}

Result<void> PageAnalyzer::shutdown() {
    if (!initialized_.exchange(false)) {
        return {};
    }
    if (initialized_owned_inspector_) {
        auto shutdown = format_inspector_->shutdown();
        initialized_owned_inspector_ = false;
        if (!shutdown) {
            initialized_.store(true);
            return shutdown.error();
        }
    }
    return {};
}

bool PageAnalyzer::is_initialized() const noexcept {
    return initialized_.load();
}

Result<FormatInspectionResult> PageAnalyzer::inspect_format(StorageFile& file) const {
    if (!is_initialized()) {
        return analysis_error(ErrorCode::InvalidState, "Page analyzer is not initialized.");
    }
    return format_inspector_->inspect(file);
}

Result<PageAnalysisResult> PageAnalyzer::inspect_page(
    const std::filesystem::path& path, std::uint32_t page_number,
    const PageAnalysisOptions& options) const {
    if (!is_initialized()) {
        return analysis_error(ErrorCode::InvalidState, "Page analyzer is not initialized.");
    }
    StorageOpenOptions open_options;
    open_options.access = StorageAccess::ReadOnly;
    auto file = storage_manager_->open_file(path, open_options);
    if (!file) {
        return file.error();
    }
    return inspect_page(*file.value(), page_number, options);
}

Result<PageAnalysisResult> PageAnalyzer::inspect_page(
    StorageFile& file, std::uint32_t page_number,
    const PageAnalysisOptions& options) const {
    auto format = inspect_format(file);
    if (!format) {
        return format.error();
    }
    const auto* metadata = format.value().metadata_as<SQLiteMetadata>();
    if (!metadata || format.value().format != DatabaseFormat::SQLite) {
        return analysis_error(ErrorCode::NotFound, "Page analysis currently supports SQLite only.");
    }
    const auto physical_pages = metadata->physically_available_complete_pages;
    const auto page_count = effective_page_count(*metadata, physical_pages);
    return inspect_page(file, page_number, *metadata, page_count, options);
}

Result<PageAnalysisResult> PageAnalyzer::inspect_page(
    StorageFile& file, std::uint32_t page_number,
    const SQLiteMetadata& metadata, std::uint64_t page_count,
    const PageAnalysisOptions& options) const {
    if (file.access() != StorageAccess::ReadOnly) {
        return analysis_error(ErrorCode::InvalidArgument, "Page analysis requires a read-only storage handle.");
    }
    if (page_number == 0 || page_number > page_count ||
        page_number > options.limits.maximum_pages) {
        return analysis_error(ErrorCode::InvalidArgument, "Page number exceeds database or configured limits.");
    }
    if (metadata.usable_page_size == 0 ||
        metadata.header.page_size > options.limits.maximum_memory_bytes) {
        return analysis_error(ErrorCode::InvalidArgument, "Page size exceeds configured memory limit.");
    }
    PageReader reader(file);
    auto page = reader.read_page(page_number, metadata.header.page_size, page_count);
    if (!page) {
        return page.error();
    }
    auto parsed = SQLiteBTreePageParser::parse(
        page_number, page.value(), metadata.header.page_size,
        metadata.usable_page_size, page_count, options);
    if (!parsed) {
        return parsed.error();
    }

    if (options.follow_overflow_chains && !parsed.value().overflow_references.empty()) {
        auto& result = parsed.value();
        for (auto& reference : result.overflow_references) {
            auto overflow = SQLiteOverflowParser::analyze(
                file, page_number, reference.first_overflow_page,
                reference.expected_payload_bytes, metadata.header.page_size,
                metadata.usable_page_size, page_count, options);
            if (!overflow) {
                return overflow.error();
            }
            reference = overflow.value().reference;
            for (const auto& relationship : overflow.value().relationships) {
                if (result.relationships.size() <
                    options.limits.maximum_relationship_nodes) {
                    result.relationships.push_back(relationship);
                } else {
                    result.complete_analysis = false;
                }
            }
            for (const auto& finding : overflow.value().findings) {
                if (result.findings.size() >= options.limits.maximum_findings) {
                    result.complete_analysis = false;
                    break;
                }
                result.findings.push_back(finding);
            }
        }
        result.structurally_valid = std::none_of(
            result.findings.begin(), result.findings.end(),
            [](const CorruptionFinding& finding) { return finding.confirmed; });
    }
    return std::move(parsed.value());
}

Result<std::vector<PageAnalysisResult>> PageAnalyzer::inspect_pages(
    StorageFile& file, std::span<const std::uint32_t> page_numbers,
    const PageAnalysisOptions& options) const {
    if (page_numbers.size() > options.limits.maximum_pages) {
        return analysis_error(ErrorCode::InvalidArgument, "Selected page count exceeds configured limit.");
    }
    auto format = inspect_format(file);
    if (!format) {
        return format.error();
    }
    const auto* metadata = format.value().metadata_as<SQLiteMetadata>();
    if (!metadata) {
        return analysis_error(ErrorCode::NotFound, "Page analysis currently supports SQLite only.");
    }
    const auto page_memory = estimated_page_analysis_memory(
        metadata->header.page_size, options.limits.maximum_cells_per_page);
    if (metadata->header.page_size == 0 || page_memory == 0 ||
        page_numbers.size() > options.limits.maximum_memory_bytes / page_memory) {
        return analysis_error(ErrorCode::InvalidArgument, "Selected pages exceed configured memory limit.");
    }
    const auto count = effective_page_count(
        *metadata, metadata->physically_available_complete_pages);
    std::vector<PageAnalysisResult> results;
    results.reserve(page_numbers.size());
    for (const auto number : page_numbers) {
        auto result = inspect_page(file, number, *metadata, count, options);
        if (!result) {
            return result.error();
        }
        results.push_back(std::move(result.value()));
    }
    return results;
}

Result<DatabasePageAnalysisResult> PageAnalyzer::scan_database(
    const std::filesystem::path& path, const PageAnalysisOptions& options) const {
    if (!is_initialized()) {
        return analysis_error(ErrorCode::InvalidState, "Page analyzer is not initialized.");
    }
    StorageOpenOptions open_options;
    open_options.access = StorageAccess::ReadOnly;
    auto file = storage_manager_->open_file(path, open_options);
    if (!file) {
        return file.error();
    }
    return scan_database(*file.value(), options);
}

Result<DatabasePageAnalysisResult> PageAnalyzer::scan_database(
    StorageFile& file, const PageAnalysisOptions& options) const {
    const auto started = std::chrono::steady_clock::now();
    auto format = inspect_format(file);
    if (!format) {
        return format.error();
    }
    const auto* metadata = format.value().metadata_as<SQLiteMetadata>();
    if (!metadata) {
        return analysis_error(ErrorCode::NotFound, "Page analysis currently supports SQLite only.");
    }
    if (file.access() != StorageAccess::ReadOnly) {
        return analysis_error(ErrorCode::InvalidArgument, "Page analysis requires a read-only storage handle.");
    }
    const auto page_count = effective_page_count(
        *metadata, metadata->physically_available_complete_pages);
    const auto scan_count = std::min({
        page_count, options.limits.maximum_pages,
        options.limits.maximum_memory_bytes / sizeof(std::uint32_t),
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())});

    DatabasePageAnalysisResult summary;
    summary.complete_analysis = scan_count == page_count;
    std::vector<std::uint32_t> parsed_btree_pages;
    std::vector<PageRelationship> relationships;
    parsed_btree_pages.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(scan_count, std::numeric_limits<std::size_t>::max())));
    for (std::uint64_t number = 1; number <= scan_count; ++number) {
        auto page = inspect_page(
            file, static_cast<std::uint32_t>(number), *metadata, page_count, options);
        if (!page) {
            return page.error();
        }
        ++summary.total_pages_analyzed;
        if (!page.value().supported) {
            ++summary.unsupported_pages;
        } else if (std::any_of(
                       page.value().findings.begin(), page.value().findings.end(),
                       [](const CorruptionFinding& finding) { return finding.confirmed; })) {
            ++summary.corrupted_pages;
        } else {
            ++summary.valid_pages;
        }
        if (page.value().supported) {
            parsed_btree_pages.push_back(page.value().page_number);
        }
        if (!page.value().complete_analysis) {
            summary.complete_analysis = false;
        }
        if (!page.value().findings.empty()) {
            ++summary.pages_with_warnings;
        }
        for (const auto& relationship : page.value().relationships) {
            if (summary.relationships.size() < options.limits.maximum_relationship_nodes) {
                summary.relationships.push_back(relationship);
                relationships.push_back(relationship);
            } else {
                summary.complete_analysis = false;
            }
        }
        for (const auto& finding : page.value().findings) {
            if (summary.findings.size() < options.limits.maximum_findings) {
                summary.findings.push_back(finding);
            } else {
                summary.complete_analysis = false;
                break;
            }
        }
    }
    if (options.validate_relationships) {
        auto relationship_findings = PageRelationshipAnalyzer::validate(
            relationships, parsed_btree_pages, page_count,
            options.limits.maximum_findings, scan_count == page_count);
        for (auto& finding : relationship_findings) {
            if (summary.findings.size() >= options.limits.maximum_findings) {
                summary.complete_analysis = false;
                break;
            }
            summary.findings.push_back(std::move(finding));
        }
    }
    summary.total_findings = summary.findings.size();
    summary.analysis_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return summary;
}

std::vector<CorruptionFinding> PageRelationshipAnalyzer::validate(
    const std::vector<PageAnalysisResult>& pages, std::uint64_t page_count,
    std::uint64_t maximum_findings) {
    std::vector<PageRelationship> relationships;
    std::vector<std::uint32_t> btree_pages;
    for (const auto& page : pages) {
        if (!page.supported) {
            continue;
        }
        btree_pages.push_back(page.page_number);
        for (const auto& relation : page.relationships) {
            relationships.push_back(relation);
        }
    }
    return validate(relationships, btree_pages, page_count, maximum_findings, true);
}

std::vector<CorruptionFinding> PageRelationshipAnalyzer::validate(
    const std::vector<PageRelationship>& relationships,
    const std::vector<std::uint32_t>& parsed_btree_pages,
    std::uint64_t page_count, std::uint64_t maximum_findings,
    bool complete_page_set) {
    std::vector<CorruptionFinding> findings;
    std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>> children;
    std::unordered_set<std::uint32_t> known_pages(
        parsed_btree_pages.begin(), parsed_btree_pages.end());
    std::unordered_map<std::uint32_t, std::uint32_t> incoming_children;
    for (const auto& relation : relationships) {
        if (relation.type != PageRelationship::Type::BTreeChild) {
            continue;
        }
        if (relation.target_page == 0 || relation.target_page > page_count ||
            relation.target_page == relation.source_page) {
            if (findings.size() < maximum_findings) {
                findings.push_back({
                    CorruptionType::InvalidChildReference,
                    CorruptionDetector::severity(CorruptionType::InvalidChildReference),
                    relation.source_page, std::nullopt,
                    "B-tree child relationship is outside the database or self-referential.",
                    "existing distinct page", std::to_string(relation.target_page), true});
            }
            continue;
        }
        if (!children[relation.source_page].insert(relation.target_page).second &&
            findings.size() < maximum_findings) {
            findings.push_back({
                CorruptionType::InvalidPageRelationship,
                CorruptionDetector::severity(CorruptionType::InvalidPageRelationship),
                relation.source_page, std::nullopt,
                "B-tree page references the same child more than once.",
                "unique child references", std::to_string(relation.target_page), true});
        }
        ++incoming_children[relation.target_page];
        if (incoming_children[relation.target_page] > 1 &&
            findings.size() < maximum_findings) {
            findings.push_back({
                CorruptionType::InvalidPageRelationship,
                CorruptionDetector::severity(CorruptionType::InvalidPageRelationship),
                relation.target_page, std::nullopt,
                "B-tree child page has multiple incoming parent references.",
                "at most one parent in a B-tree",
                std::to_string(incoming_children[relation.target_page]) + " parents", true});
        }
        if (complete_page_set && !known_pages.contains(relation.target_page) &&
            findings.size() < maximum_findings) {
            findings.push_back({
                CorruptionType::InvalidChildReference,
                CorruptionDetector::severity(CorruptionType::InvalidChildReference),
                relation.source_page, std::nullopt,
                "Referenced child page is not a parsed B-tree page.",
                "existing B-tree page", std::to_string(relation.target_page), true});
        }
    }
    return findings;
}

Result<BTreeAnalysisResult> BTreeAnalyzer::analyze(
    PageAnalyzer& analyzer, StorageFile& file, std::uint32_t root_page,
    const PageAnalysisOptions& options) {
    if (!analyzer.is_initialized()) {
        return analysis_error(ErrorCode::InvalidState, "Page analyzer is not initialized.");
    }
    return analyzer.analyze_btree(file, root_page, options);
}

Result<BTreeAnalysisResult> PageAnalyzer::analyze_btree(
    StorageFile& file, std::uint32_t root_page,
    const PageAnalysisOptions& options) const {
    auto format = inspect_format(file);
    if (!format) {
        return format.error();
    }
    const auto* metadata = format.value().metadata_as<SQLiteMetadata>();
    if (!metadata) {
        return analysis_error(ErrorCode::NotFound, "B-tree analysis currently supports SQLite only.");
    }
    const auto page_count = effective_page_count(
        *metadata, metadata->physically_available_complete_pages);
    if (root_page == 0 || root_page > page_count ||
        root_page > options.limits.maximum_pages ||
        options.limits.maximum_memory_bytes / 64U == 0) {
        return analysis_error(ErrorCode::InvalidArgument, "B-tree root page is outside configured bounds.");
    }
    const auto maximum_tree_pages = std::min(
        options.limits.maximum_pages,
        options.limits.maximum_memory_bytes / 64U);

    struct PendingPage {
        std::uint32_t number;
        std::uint32_t depth;
    };
    std::vector<PendingPage> pending{{root_page, 1}};
    std::unordered_set<std::uint32_t> visited;
    BTreeAnalysisResult result;
    result.root_page = root_page;
    const auto add_finding = [&result, &options](CorruptionFinding finding) {
        if (result.findings.size() < options.limits.maximum_findings) {
            result.findings.push_back(std::move(finding));
        }
    };
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (current.depth > options.limits.maximum_btree_depth) {
            add_finding({
                CorruptionType::InvalidPageRelationship,
                CorruptionSeverity::Medium, current.number, std::nullopt,
                "B-tree traversal reached the configured depth limit.",
                "depth within configured limit",
                std::to_string(options.limits.maximum_btree_depth), false});
            break;
        }
        if (!visited.insert(current.number).second) {
            add_finding({
                CorruptionType::InvalidPageRelationship,
                CorruptionSeverity::High, current.number, std::nullopt,
                "B-tree traversal encountered a cycle or duplicate child.",
                "acyclic tree with unique parentage",
                std::to_string(current.number), true});
            continue;
        }
        if (visited.size() > maximum_tree_pages) {
            return analysis_error(ErrorCode::InvalidArgument, "B-tree traversal exceeded page limit.");
        }
        auto page = inspect_page(file, current.number, *metadata, page_count, options);
        if (!page) {
            return page.error();
        }
        if (!page.value().supported) {
            add_finding({
                CorruptionType::InvalidPageType, CorruptionSeverity::Medium,
                current.number, std::nullopt,
                "B-tree child is not a supported B-tree page.",
                "SQLite B-tree page type", page.value().message, false});
            continue;
        }
        result.maximum_depth = std::max(result.maximum_depth, current.depth);
        if (page.value().header.is_interior()) {
            ++result.interior_pages;
            for (const auto& relationship : page.value().relationships) {
                if (relationship.type == PageRelationship::Type::BTreeChild) {
                    if (relationship.target_page == 0 ||
                        relationship.target_page > page_count ||
                        relationship.target_page == current.number) {
                        continue;
                    }
                    if (pending.size() >= options.limits.maximum_relationship_nodes) {
                        add_finding({
                            CorruptionType::InvalidPageRelationship,
                            CorruptionSeverity::Medium, current.number, std::nullopt,
                            "B-tree work queue reached its configured safety limit.",
                            "queued pages within configured limit",
                            std::to_string(options.limits.maximum_relationship_nodes), false});
                        pending.clear();
                        break;
                    }
                    pending.push_back({relationship.target_page, current.depth + 1});
                }
            }
        } else {
            ++result.leaf_pages;
        }
        result.findings.insert(
            result.findings.end(), page.value().findings.begin(),
            page.value().findings.end());
        if (result.findings.size() > options.limits.maximum_findings) {
            result.findings.resize(static_cast<std::size_t>(options.limits.maximum_findings));
            break;
        }
        result.visited_pages.push_back(current.number);
    }
    result.pages_visited = result.visited_pages.size();
    return result;
}

Result<std::vector<CorruptionFinding>> PageAnalyzer::analyze_relationships(
    StorageFile& file, const PageAnalysisOptions& options) const {
    auto format = inspect_format(file);
    if (!format) {
        return format.error();
    }
    const auto* metadata = format.value().metadata_as<SQLiteMetadata>();
    if (!metadata) {
        return analysis_error(ErrorCode::NotFound, "Page relationship analysis currently supports SQLite only.");
    }
    const auto page_count = effective_page_count(
        *metadata, metadata->physically_available_complete_pages);
    const auto count = std::min({
        page_count, options.limits.maximum_pages,
        options.limits.maximum_memory_bytes / sizeof(std::uint32_t),
        options.limits.maximum_memory_bytes / metadata->header.page_size,
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())});
    std::vector<std::uint32_t> btree_pages;
    std::vector<PageRelationship> relationships;
    btree_pages.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
        count, std::numeric_limits<std::size_t>::max())));
    for (std::uint64_t page_number = 1; page_number <= count; ++page_number) {
        auto page = inspect_page(file, static_cast<std::uint32_t>(page_number),
                                 *metadata, page_count, options);
        if (!page) {
            return page.error();
        }
        if (page.value().supported) {
            btree_pages.push_back(page.value().page_number);
        }
        const auto room = options.limits.maximum_relationship_nodes -
                          std::min<std::uint64_t>(
                              relationships.size(), options.limits.maximum_relationship_nodes);
        const auto number = std::min<std::uint64_t>(
            room, page.value().relationships.size());
        relationships.insert(
            relationships.end(), page.value().relationships.begin(),
            page.value().relationships.begin() + static_cast<std::ptrdiff_t>(number));
    }
    return PageRelationshipAnalyzer::validate(
        relationships, btree_pages, page_count,
        options.limits.maximum_findings, count == page_count);
}

std::string_view sqlite_page_type_name(SQLitePageType type) noexcept {
    switch (type) {
    case SQLitePageType::InteriorIndex: return "Interior index";
    case SQLitePageType::InteriorTable: return "Interior table";
    case SQLitePageType::LeafIndex: return "Leaf index";
    case SQLitePageType::LeafTable: return "Leaf table";
    case SQLitePageType::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::string_view corruption_type_name(CorruptionType type) noexcept {
    switch (type) {
    case CorruptionType::InvalidPageType: return "Invalid page type";
    case CorruptionType::InvalidPageHeader: return "Invalid page header";
    case CorruptionType::InvalidCellPointer: return "Invalid cell pointer";
    case CorruptionType::InvalidCellBoundary: return "Invalid cell boundary";
    case CorruptionType::OverlappingCells: return "Overlapping cells";
    case CorruptionType::InvalidFreeblock: return "Invalid freeblock";
    case CorruptionType::FreeblockCycle: return "Freeblock cycle";
    case CorruptionType::InvalidVarint: return "Invalid varint";
    case CorruptionType::InvalidChildReference: return "Invalid child reference";
    case CorruptionType::InvalidOverflowReference: return "Invalid overflow reference";
    case CorruptionType::OverflowCycle: return "Overflow cycle";
    case CorruptionType::InvalidPageRelationship: return "Invalid page relationship";
    case CorruptionType::InvalidSpaceAccounting: return "Invalid space accounting";
    case CorruptionType::UnreachablePage: return "Unreachable page";
    case CorruptionType::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::string_view corruption_severity_name(CorruptionSeverity severity) noexcept {
    switch (severity) {
    case CorruptionSeverity::Low: return "Low";
    case CorruptionSeverity::Medium: return "Medium";
    case CorruptionSeverity::High: return "High";
    case CorruptionSeverity::Critical: return "Critical";
    }
    return "Unknown";
}

std::string format_page_analysis_report(const PageAnalysisResult& result) {
    std::ostringstream report;
    report << "Page: " << result.page_number << '\n'
           << "Type: " << sqlite_page_type_name(result.page_type) << '\n'
           << "Header offset/size: " << result.header.header_offset << '/'
           << result.header.header_size << '\n'
           << "Cell count: " << result.header.cell_count << '\n'
           << "Cell content area: " << result.header.cell_content_area_offset << '\n'
           << "First freeblock: " << result.header.first_freeblock_offset << '\n'
           << "Fragmented free bytes: "
           << static_cast<unsigned int>(result.header.fragmented_free_bytes) << '\n'
           << "Right-most child: " << result.header.rightmost_child_page << '\n'
           << "Cells: " << result.cells.size() << '\n'
           << "Freeblocks: " << result.freeblocks.size() << '\n'
           << "Structurally valid: " << (result.structurally_valid ? "yes" : "no") << '\n'
           << "Space: " << result.space.accounted_bytes << '/'
           << result.space.usable_size << " bytes accounted\n"
           << "  Cells: " << result.space.cell_bytes << " bytes\n"
           << "  Freeblocks: " << result.space.freeblock_bytes << " bytes\n"
           << "  Fragmented: " << result.space.fragmented_bytes << " bytes\n"
           << "  Unallocated: " << result.space.unallocated_bytes << " bytes\n"
           << "  Unaccounted: " << result.space.unaccounted_bytes << " bytes\n";
    for (const auto& freeblock : result.freeblocks) {
        report << "  Freeblock at " << freeblock.offset << ", size "
               << freeblock.size << ", next " << freeblock.next_offset
               << (freeblock.valid ? " (valid)\n" : " (invalid)\n");
    }
    for (const auto& finding : result.findings) {
        report << '[' << corruption_severity_name(finding.severity) << "] "
               << corruption_type_name(finding.type) << ": "
               << finding.description << '\n';
    }
    return report.str();
}

std::string format_database_analysis_report(
    const DatabasePageAnalysisResult& result) {
    std::ostringstream report;
    report << "Pages analyzed: " << result.total_pages_analyzed << '\n'
           << "Complete analysis: " << (result.complete_analysis ? "yes" : "no") << '\n'
           << "Valid pages: " << result.valid_pages << '\n'
           << "Pages with findings: " << result.pages_with_warnings << '\n'
           << "Corrupted pages: " << result.corrupted_pages << '\n'
           << "Unsupported pages: " << result.unsupported_pages << '\n'
           << "Relationships: " << result.relationships.size() << '\n'
           << "Findings: " << result.total_findings << '\n'
           << "Duration: " << result.analysis_duration.count() << " ms\n";
    for (const auto& finding : result.findings) {
        report << '[' << corruption_severity_name(finding.severity) << "] Page "
               << finding.page_number << ' ' << corruption_type_name(finding.type)
               << ": " << finding.description << '\n';
    }
    return report.str();
}

} // namespace storq
