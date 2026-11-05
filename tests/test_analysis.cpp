#include <storq/storq.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(STORQ_TEST_SQLITE3)
#include <sqlite3.h>
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_ok(storq::Result<void> result, const char* message) {
    if (!result) {
        throw std::runtime_error(std::string(message) + ": " + result.error().message);
    }
}

void write_be16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 8) & 0xff);
    bytes[offset + 1] = static_cast<std::byte>(value & 0xff);
}

void write_be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<std::byte>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<std::byte>(value & 0xff);
}

std::vector<std::byte> page_with_type(std::uint8_t type, std::uint32_t page_number = 2) {
    std::vector<std::byte> page(512, std::byte{0});
    const std::size_t header = page_number == 1 ? 100 : 0;
    page[header] = static_cast<std::byte>(type);
    write_be16(page, header + 5, 500);
    return page;
}

std::filesystem::path make_temp_directory() {
    const auto path =
        std::filesystem::temp_directory_path() / ("storq-analysis-" + storq::generate_uuid());
    std::filesystem::create_directories(path);
    return path;
}

void test_page_headers() {
    const std::array<std::pair<std::uint8_t, storq::SQLitePageType>, 4> cases{{
        {0x02, storq::SQLitePageType::InteriorIndex},
        {0x05, storq::SQLitePageType::InteriorTable},
        {0x0a, storq::SQLitePageType::LeafIndex},
        {0x0d, storq::SQLitePageType::LeafTable}}};
    for (const auto& [raw, expected] : cases) {
        auto page = page_with_type(raw);
        auto parsed = storq::SQLitePageHeaderParser::parse(2, page, 512);
        require(parsed.has_value() && parsed.value().page_type == expected,
                "Expected correct SQLite B-tree page type.");
        require(parsed.value().is_leaf() ==
                    (expected == storq::SQLitePageType::LeafIndex ||
                     expected == storq::SQLitePageType::LeafTable),
                "Expected correct leaf classification.");
    }
    auto first_page = page_with_type(0x0d, 1);
    auto parsed_first = storq::SQLitePageHeaderParser::parse(1, first_page, 512);
    require(parsed_first.has_value() && parsed_first.value().header_offset == 100,
            "Expected page one B-tree header offset of 100.");
    auto unknown = page_with_type(0x07);
    auto parsed_unknown = storq::SQLitePageHeaderParser::parse(2, unknown, 512);
    require(parsed_unknown.has_value() &&
                parsed_unknown.value().page_type == storq::SQLitePageType::Unknown,
            "Expected unknown SQLite page type to remain unparsed.");
}

void test_cell_varints_and_payloads() {
    const std::array<std::byte, 1> one_byte{std::byte{0x7f}};
    auto one = storq::SQLiteCellParser::decode_varint(one_byte, 0);
    require(one.has_value() && one.value().value == 127 && one.value().bytes == 1,
            "Expected one-byte SQLite varint decoding.");

    const std::array<std::byte, 2> two_bytes{std::byte{0x81}, std::byte{0x00}};
    auto two = storq::SQLiteCellParser::decode_varint(two_bytes, 0);
    require(two.has_value() && two.value().value == 128 && two.value().bytes == 2,
            "Expected multi-byte SQLite varint decoding.");

    const std::array<std::byte, 9> nine_bytes{
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
    auto nine = storq::SQLiteCellParser::decode_varint(nine_bytes, 0);
    require(nine.has_value() && nine.value().bytes == 9 &&
                nine.value().value == std::numeric_limits<std::uint64_t>::max(),
            "Expected nine-byte SQLite varint decoding.");
    require(!storq::SQLiteCellParser::decode_varint(
                std::span<const std::byte>(nine_bytes).first(8), 0),
            "Expected truncated nine-byte varint rejection.");

    const auto table_max =
        storq::SQLiteCellParser::maximum_local_payload(storq::SQLitePageType::LeafTable, 512);
    const auto index_max =
        storq::SQLiteCellParser::maximum_local_payload(storq::SQLitePageType::LeafIndex, 512);
    require(table_max == 477 && index_max == ((500U * 64U) / 255U - 23U),
            "Expected SQLite table/index local payload thresholds.");
    auto local = storq::SQLiteCellParser::local_payload_size(
        storq::SQLitePageType::LeafIndex, 512, 100000);
    require(local.has_value() && local.value() >=
                storq::SQLiteCellParser::minimum_local_payload(512) &&
                local.value() <= index_max,
            "Expected index payload overflow distribution.");
}

void test_cell_and_freeblock_parsing() {
    storq::PageAnalysisOptions options;
    auto leaf_table = page_with_type(0x0d);
    write_be16(leaf_table, 3, 1);
    write_be16(leaf_table, 5, 509);
    write_be16(leaf_table, 8, 509);
    leaf_table[509] = std::byte{1};
    leaf_table[510] = std::byte{1};
    leaf_table[511] = std::byte{'x'};
    auto parsed_table = storq::SQLiteBTreePageParser::parse(
        2, leaf_table, 512, 512, 1, options);
    require(parsed_table.has_value() && parsed_table.value().cells.size() == 1 &&
                parsed_table.value().cells.front().rowid == 1 &&
                parsed_table.value().cells.front().payload_size == 1,
            "Expected valid leaf-table cell parsing.");

    auto interior_table = page_with_type(0x05);
    write_be16(interior_table, 3, 1);
    write_be16(interior_table, 5, 507);
    write_be32(interior_table, 8, 3);
    write_be16(interior_table, 12, 507);
    write_be32(interior_table, 507, 4);
    interior_table[511] = std::byte{9};
    write_be32(interior_table, 8, 5);
    auto parsed_interior = storq::SQLiteBTreePageParser::parse(
        2, interior_table, 512, 512, 5, options);
    require(parsed_interior.has_value() && parsed_interior.value().cells.size() == 1 &&
                parsed_interior.value().cells.front().left_child_page == 4 &&
                parsed_interior.value().cells.front().rowid == 9,
            "Expected valid interior-table cell parsing.");

    auto leaf_index = page_with_type(0x0a);
    write_be16(leaf_index, 3, 1);
    write_be16(leaf_index, 5, 510);
    write_be16(leaf_index, 8, 510);
    leaf_index[510] = std::byte{1};
    leaf_index[511] = std::byte{'i'};
    auto parsed_index = storq::SQLiteBTreePageParser::parse(
        2, leaf_index, 512, 512, 1, options);
    require(parsed_index.has_value() && parsed_index.value().cells.size() == 1 &&
                parsed_index.value().cells.front().payload_size == 1,
            "Expected valid leaf-index cell parsing.");

    auto interior_index = page_with_type(0x02);
    write_be16(interior_index, 3, 1);
    write_be16(interior_index, 5, 506);
    write_be32(interior_index, 8, 3);
    write_be16(interior_index, 12, 506);
    write_be32(interior_index, 506, 4);
    interior_index[510] = std::byte{1};
    interior_index[511] = std::byte{'k'};
    write_be32(interior_index, 8, 5);
    auto parsed_interior_index = storq::SQLiteBTreePageParser::parse(
        2, interior_index, 512, 512, 5, options);
    require(parsed_interior_index.has_value() &&
                parsed_interior_index.value().cells.size() == 1 &&
                parsed_interior_index.value().cells.front().payload_size == 1,
            "Expected valid interior-index cell parsing.");

    auto freeblock_page = page_with_type(0x0d);
    write_be16(freeblock_page, 1, 400);
    write_be16(freeblock_page, 5, 400);
    write_be16(freeblock_page, 400, 0);
    write_be16(freeblock_page, 402, 112);
    auto parsed_freeblock = storq::SQLiteBTreePageParser::parse(
        2, freeblock_page, 512, 512, 1, options);
    require(parsed_freeblock.has_value() &&
                parsed_freeblock.value().freeblocks.size() == 1 &&
                parsed_freeblock.value().space.freeblock_bytes == 112,
            "Expected freeblock traversal and space accounting.");

    auto cyclic_page = page_with_type(0x0d);
    write_be16(cyclic_page, 1, 400);
    write_be16(cyclic_page, 5, 400);
    write_be16(cyclic_page, 400, 400);
    write_be16(cyclic_page, 402, 112);
    auto parsed_cycle = storq::SQLiteBTreePageParser::parse(
        2, cyclic_page, 512, 512, 1, options);
    require(parsed_cycle.has_value() &&
                std::any_of(parsed_cycle.value().findings.begin(),
                            parsed_cycle.value().findings.end(),
                            [](const storq::CorruptionFinding& finding) {
                                return finding.type == storq::CorruptionType::FreeblockCycle;
                            }),
            "Expected freeblock cycle detection.");

    auto invalid_pointer_page = page_with_type(0x0d);
    write_be16(invalid_pointer_page, 3, 1);
    write_be16(invalid_pointer_page, 8, 50);
    auto parsed_bad_pointer = storq::SQLiteBTreePageParser::parse(
        2, invalid_pointer_page, 512, 512, 1, options);
    require(parsed_bad_pointer.has_value() &&
                std::any_of(parsed_bad_pointer.value().findings.begin(),
                            parsed_bad_pointer.value().findings.end(),
                            [](const storq::CorruptionFinding& finding) {
                                return finding.type ==
                                       storq::CorruptionType::InvalidCellPointer;
                            }),
            "Expected an out-of-range cell pointer finding.");

    auto truncated_cell_page = page_with_type(0x0d);
    write_be16(truncated_cell_page, 3, 1);
    write_be16(truncated_cell_page, 5, 511);
    write_be16(truncated_cell_page, 8, 511);
    truncated_cell_page[511] = std::byte{0x80};
    auto parsed_truncated_cell = storq::SQLiteBTreePageParser::parse(
        2, truncated_cell_page, 512, 512, 1, options);
    require(parsed_truncated_cell.has_value() &&
                std::any_of(parsed_truncated_cell.value().findings.begin(),
                            parsed_truncated_cell.value().findings.end(),
                            [](const storq::CorruptionFinding& finding) {
                                return finding.type == storq::CorruptionType::InvalidVarint;
                            }),
            "Expected truncated cell varint detection.");
}

void test_relationship_validation() {
    const std::vector<storq::PageRelationship> relationships{
        {2, 3, storq::PageRelationship::Type::BTreeChild},
        {2, 3, storq::PageRelationship::Type::BTreeChild},
        {4, 3, storq::PageRelationship::Type::BTreeChild}};
    const std::vector<std::uint32_t> btree_pages{2, 3, 4};
    const auto findings = storq::PageRelationshipAnalyzer::validate(
        relationships, btree_pages, 4, 10, true);
    require(findings.size() >= 2,
            "Expected duplicate and multiply-parented child findings.");
}

#if defined(STORQ_TEST_SQLITE3)
std::filesystem::path create_sqlite_database(const std::filesystem::path& directory) {
    const auto path = directory / "pages.sqlite";
    sqlite3* database = nullptr;
    require(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK,
            "Unable to create SQLite integration fixture.");
    char* error = nullptr;
    const char* schema =
        "PRAGMA page_size=512;"
        "VACUUM;"
        "CREATE TABLE records(id INTEGER PRIMARY KEY, payload BLOB);";
    const auto schema_result = sqlite3_exec(database, schema, nullptr, nullptr, &error);
    const std::string schema_error = error ? error : "";
    sqlite3_free(error);
    require(schema_result == SQLITE_OK, schema_error.c_str());

    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(
                database, "INSERT INTO records(payload) VALUES(zeroblob(?1))",
                -1, &statement, nullptr) == SQLITE_OK,
            "Unable to prepare large-blob fixture insert.");
    for (int i = 0; i < 30; ++i) {
        sqlite3_bind_int(statement, 1, 10000);
        require(sqlite3_step(statement) == SQLITE_DONE,
                "Unable to insert page-analysis fixture row.");
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    }
    sqlite3_finalize(statement);
    require(sqlite3_close(database) == SQLITE_OK,
            "Unable to close SQLite integration fixture.");
    return path;
}

std::uint32_t sqlite_root_page(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    require(sqlite3_open_v2(
                path.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
            "Unable to open SQLite fixture read-only for root lookup.");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(
                database, "SELECT rootpage FROM sqlite_master WHERE name='records'",
                -1, &statement, nullptr) == SQLITE_OK,
            "Unable to query generated table root.");
    require(sqlite3_step(statement) == SQLITE_ROW,
            "Generated table root page was not found.");
    const auto root_page = static_cast<std::uint32_t>(sqlite3_column_int(statement, 0));
    sqlite3_finalize(statement);
    require(sqlite3_close(database) == SQLITE_OK,
            "Unable to close SQLite root lookup handle.");
    return root_page;
}

void test_sqlite_integration(const std::filesystem::path& directory) {
    const auto path = create_sqlite_database(directory);
    const auto root_page = sqlite_root_page(path);
    std::ifstream before_stream(path, std::ios::binary);
    const std::vector<char> before{
        std::istreambuf_iterator<char>(before_stream), std::istreambuf_iterator<char>()};

    storq::Runtime runtime;
    auto analyzer = std::make_shared<storq::PageAnalyzer>(runtime);
    require_ok(runtime.modules().register_module(analyzer), "Register page analyzer module");
    require_ok(runtime.initialize(), "Initialize page-analysis runtime");
    require(runtime.capabilities().contains("page_analysis.sqlite"),
            "Expected SQLite page-analysis capability.");

    auto page_one = analyzer->inspect_page(path, 1);
    require(page_one.has_value() &&
                page_one.value().page_type == storq::SQLitePageType::LeafTable,
            "Expected SQLite page one inspection.");
    auto scan = analyzer->scan_database(path);
    require(scan.has_value() && scan.value().total_pages_analyzed > 1 &&
                scan.value().complete_analysis,
            "Expected complete multi-page SQLite database scan.");
    require(scan.value().relationships.size() > 0,
            "Expected physical child/overflow relationships to be reported.");
    require(scan.value().corrupted_pages == 0,
            "Expected generated SQLite pages to pass structural validation.");
    require(std::any_of(
                scan.value().relationships.begin(), scan.value().relationships.end(),
                [](const storq::PageRelationship& relationship) {
                    return relationship.type == storq::PageRelationship::Type::OverflowNext;
                }),
            "Expected generated large records to contain overflow-page links.");

    storq::StorageManager manager;
    storq::StorageOpenOptions options;
    options.access = storq::StorageAccess::ReadOnly;
    auto file = manager.open_file(path, options);
    require(file.has_value(), "Unable to open SQLite fixture through StorageManager.");
    bool found_complete_overflow = false;
    for (std::uint64_t number = 1; number <= scan.value().total_pages_analyzed; ++number) {
        auto page = analyzer->inspect_page(
            *file.value(), static_cast<std::uint32_t>(number));
        require(page.has_value(), "Unable to inspect SQLite page for overflow chain.");
        for (const auto& reference : page.value().overflow_references) {
            found_complete_overflow =
                found_complete_overflow ||
                (reference.complete && reference.pages_traversed > 0 &&
                 reference.remaining_payload_bytes == 0);
        }
        if (found_complete_overflow) {
            break;
        }
    }
    require(found_complete_overflow,
            "Expected a generated overflow payload chain to be fully traversed.");
    auto tree = analyzer->analyze_btree(*file.value(), root_page);
    require(tree.has_value() && tree.value().pages_visited > 1 &&
                tree.value().interior_pages > 0 && tree.value().leaf_pages > 0,
            "Expected multi-page B-tree traversal.");
    require_ok(file.value()->close(), "Close analyzed database");

    std::ifstream after_stream(path, std::ios::binary);
    const std::vector<char> after{
        std::istreambuf_iterator<char>(after_stream), std::istreambuf_iterator<char>()};
    require(before == after, "Page analysis modified the source SQLite database.");
    require_ok(runtime.shutdown(), "Shutdown page-analysis runtime");
}
#endif

} // namespace

int main() {
    try {
        test_page_headers();
        test_cell_varints_and_payloads();
        test_cell_and_freeblock_parsing();
        test_relationship_validation();
#if defined(STORQ_TEST_SQLITE3)
        const auto directory = make_temp_directory();
        auto cleanup = storq::make_scope_exit([&directory] {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        });
        test_sqlite_integration(directory);
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Page analysis test failure: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "Page analysis tests passed.\n";
    return 0;
}
