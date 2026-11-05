#include <storq/storq.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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

void write_be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<std::byte>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<std::byte>(value & 0xff);
}

std::vector<std::byte> valid_sqlite_file() {
    std::vector<std::byte> bytes(512, std::byte{0});
    constexpr std::string_view signature = "SQLite format 3";
    for (std::size_t i = 0; i < signature.size(); ++i) {
        bytes[i] = static_cast<std::byte>(signature[i]);
    }
    bytes[15] = std::byte{0};
    bytes[16] = std::byte{2};
    bytes[17] = std::byte{0};
    bytes[18] = std::byte{1};
    bytes[19] = std::byte{1};
    bytes[21] = std::byte{64};
    bytes[22] = std::byte{32};
    bytes[23] = std::byte{32};
    write_be32(bytes, 24, 1);
    write_be32(bytes, 28, 1);
    write_be32(bytes, 44, 4);
    write_be32(bytes, 56, 1);
    write_be32(bytes, 92, 1);
    write_be32(bytes, 96, 3040001);
    return bytes;
}

std::filesystem::path make_temp_directory() {
    const auto path =
        std::filesystem::temp_directory_path() / ("storq-format-" + storq::generate_uuid());
    std::filesystem::create_directories(path);
    return path;
}

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "Unable to create fixture file.");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "Unable to write fixture file.");
}

class StorqTestParser final : public storq::StorqFormatParser {
public:
    [[nodiscard]] storq::FormatDescriptor descriptor() const override {
        return {storq::DatabaseFormat::Storq, "storq-test", "Storq test", "1",
                "Test parser extension.", {}, true, true};
    }

    [[nodiscard]] storq::Result<bool> matches(storq::StorageFile& file) const override {
        std::array<std::byte, 5> signature{};
        auto read = file.read_at(0, signature);
        if (!read) {
            return read.error();
        }
        return read.value() == signature.size() &&
               signature[0] == std::byte{'S'} && signature[1] == std::byte{'T'} &&
               signature[2] == std::byte{'O'} && signature[3] == std::byte{'R'} &&
               signature[4] == std::byte{'Q'};
    }

    [[nodiscard]] storq::Result<storq::FormatInspectionResult> inspect(
        storq::StorageFile&) const override {
        storq::FormatInspectionResult result;
        result.format = storq::DatabaseFormat::Storq;
        result.descriptor = descriptor();
        result.status = storq::FormatStatus::Valid;
        result.header_valid = true;
        result.structure_valid = true;
        return result;
    }
};

void test_header_helpers() {
    auto file = valid_sqlite_file();
    auto header = storq::SQLiteFormatParser::parse_header(file);
    require(header.has_value(), "Expected valid SQLite header parsing.");
    require(header.value().page_size == 512, "Expected 512-byte SQLite pages.");
    require(header.value().database_encoding == storq::SQLiteEncoding::UTF8,
            "Expected UTF-8 encoding.");
    require(header.value().header_page_count_is_valid(),
            "Expected matching counters to validate the header page count.");
    auto large_page_file = valid_sqlite_file();
    large_page_file[16] = std::byte{0};
    large_page_file[17] = std::byte{1};
    auto large_page_header = storq::SQLiteFormatParser::parse_header(large_page_file);
    require(large_page_header.has_value() && large_page_header.value().page_size == 65536,
            "Expected the SQLite page-size sentinel to map to 65536.");
    require(storq::SQLiteFormatParser::valid_page_size(65536),
            "Expected 65536-byte page size to be supported.");
    require(!storq::SQLiteFormatParser::valid_page_size(1000),
            "Expected non-power-of-two page size to be rejected.");
    require(storq::SQLiteFormatParser::page_offset(512, 2).value() == 512,
            "Expected second page offset.");
    require(storq::SQLiteFormatParser::page_boundary(512, 2).value() == 1024,
            "Expected second page boundary.");
    require(!storq::SQLiteFormatParser::page_boundary(
                65536, std::numeric_limits<std::uint64_t>::max()),
            "Expected page boundary overflow to fail.");
}

void test_inspection_and_unknown(const std::filesystem::path& directory) {
    const auto valid_path = directory / "valid.sqlite";
    const auto valid_bytes = valid_sqlite_file();
    write_file(valid_path, valid_bytes);

    storq::Runtime runtime;
    auto inspector = std::make_shared<storq::FormatInspector>(runtime);
    require_ok(runtime.modules().register_module(inspector), "Register format inspector");
    require_ok(runtime.initialize(), "Initialize runtime");
    require(inspector->is_initialized(), "Expected format inspector module initialization.");
    require(runtime.capabilities().contains("format_inspection.sqlite"),
            "Expected SQLite inspection capability registration.");
    require(inspector->formats().size() == 1,
            "Expected built-in SQLite parser registration.");

    auto inspected = inspector->inspect(valid_path);
    require(inspected.has_value(), "Expected valid fixture inspection.");
    require(inspected.value().format == storq::DatabaseFormat::SQLite,
            "Expected SQLite format detection.");
    require(inspected.value().status == storq::FormatStatus::Valid,
            "Expected handcrafted valid fixture to pass validation.");
    require(inspected.value().page_size == 512 && inspected.value().page_count == 1,
            "Expected SQLite page metadata.");
    require(storq::format_inspection_report(inspected.value()).find("Format: SQLite") !=
                std::string::npos,
            "Expected a human-readable inspection report.");

    std::ifstream input(valid_path, std::ios::binary);
    std::vector<char> after((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    require(after.size() == valid_bytes.size(), "Inspection changed fixture size.");
    for (std::size_t i = 0; i < after.size(); ++i) {
        require(static_cast<unsigned char>(after[i]) ==
                    std::to_integer<unsigned char>(valid_bytes[i]),
                "Inspection modified the SQLite fixture.");
    }

    const auto unknown_path = directory / "unknown.bin";
    write_file(unknown_path, {std::byte{'N'}, std::byte{'O'}, std::byte{'P'}, std::byte{0}});
    auto unknown = inspector->inspect(unknown_path);
    require(unknown.has_value() && unknown.value().status == storq::FormatStatus::Unsupported,
            "Expected unknown format to be reported as unsupported.");

    const auto truncated_path = directory / "truncated.sqlite";
    auto truncated_bytes = valid_sqlite_file();
    truncated_bytes.resize(32);
    write_file(truncated_path, truncated_bytes);
    auto truncated = inspector->inspect(truncated_path);
    require(truncated.has_value() && truncated.value().status == storq::FormatStatus::Incomplete,
            "Expected truncated recognized SQLite file to be incomplete.");

    auto stale_counter_bytes = valid_sqlite_file();
    write_be32(stale_counter_bytes, 92, 2);
    const auto stale_counter_path = directory / "stale-counter.sqlite";
    write_file(stale_counter_path, stale_counter_bytes);
    auto stale_counter = inspector->inspect(stale_counter_path);
    require(stale_counter.has_value() &&
                stale_counter.value().status == storq::FormatStatus::ValidWithWarnings &&
                stale_counter.value().page_count == 1,
            "Expected unreliable header count to fall back to physical pages.");

    auto invalid_page_size_bytes = valid_sqlite_file();
    invalid_page_size_bytes[16] = std::byte{3};
    invalid_page_size_bytes[17] = std::byte{0xe8};
    const auto invalid_page_size_path = directory / "invalid-page-size.sqlite";
    write_file(invalid_page_size_path, invalid_page_size_bytes);
    auto invalid_page_size = inspector->inspect(invalid_page_size_path);
    require(invalid_page_size.has_value() &&
                invalid_page_size.value().status == storq::FormatStatus::Malformed,
            "Expected unsupported SQLite page size to be malformed.");

    auto overclaimed_pages_bytes = valid_sqlite_file();
    write_be32(overclaimed_pages_bytes, 28, 2);
    const auto overclaimed_pages_path = directory / "overclaimed-pages.sqlite";
    write_file(overclaimed_pages_path, overclaimed_pages_bytes);
    auto overclaimed_pages = inspector->inspect(overclaimed_pages_path);
    require(overclaimed_pages.has_value() &&
                overclaimed_pages.value().status == storq::FormatStatus::Corrupted,
            "Expected reliable header page count beyond physical size to be corrupted.");

    auto future_reader_bytes = valid_sqlite_file();
    future_reader_bytes[19] = std::byte{3};
    const auto future_reader_path = directory / "future-reader.sqlite";
    write_file(future_reader_path, future_reader_bytes);
    auto future_reader = inspector->inspect(future_reader_path);
    require(future_reader.has_value() &&
                future_reader.value().status == storq::FormatStatus::Unsupported,
            "Expected a future SQLite read format to be unsupported.");

    const auto custom_path = directory / "custom.storq";
    write_file(custom_path,
               {std::byte{'S'}, std::byte{'T'}, std::byte{'O'}, std::byte{'R'},
                std::byte{'Q'}, std::byte{0}});
    require_ok(inspector->register_parser(std::make_shared<StorqTestParser>()),
               "Register custom Storq parser");
    auto custom = inspector->inspect(custom_path);
    require(custom.has_value() && custom.value().format == storq::DatabaseFormat::Storq,
            "Expected custom Storq parser to be used.");

    require_ok(runtime.shutdown(), "Shutdown runtime");
}

void test_writable_handle_rejected() {
    storq::Runtime runtime;
    storq::FormatInspector inspector(runtime);
    require_ok(inspector.initialize(), "Initialize standalone inspector");
    storq::StorageManager manager;
    auto writable = manager.create_memory_storage();
    require(writable.has_value(), "Expected in-memory storage creation.");
    auto inspected = inspector.inspect(*writable.value());
    require(!inspected && inspected.error().code == storq::ErrorCode::InvalidArgument,
            "Expected writable storage handles to be rejected.");
    require_ok(inspector.shutdown(), "Shutdown standalone inspector");
}

#if defined(STORQ_TEST_SQLITE3)
void test_sqlite_generated_database(const std::filesystem::path& directory) {
    const auto path = directory / "generated.sqlite";
    sqlite3* database = nullptr;
    require(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK,
            "Unable to create SQLite fixture through SQLite.");
    const char* sql = "CREATE TABLE sample(value TEXT); INSERT INTO sample VALUES('fixture');";
    char* error = nullptr;
    const auto execute_result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    const std::string error_message = error ? error : "";
    sqlite3_free(error);
    const auto close_result = sqlite3_close(database);
    require(execute_result == SQLITE_OK, error_message.c_str());
    require(close_result == SQLITE_OK, "Unable to close generated SQLite fixture.");

    storq::Runtime runtime;
    auto inspector = std::make_shared<storq::FormatInspector>(runtime);
    require_ok(runtime.modules().register_module(inspector), "Register generated-database inspector");
    require_ok(runtime.initialize(), "Initialize generated-database runtime");
    auto inspected = inspector->inspect(path);
    require(inspected.has_value() && inspected.value().format == storq::DatabaseFormat::SQLite,
            "Expected SQLite-generated database to be recognized.");
    require(inspected.value().page_size >= 512 && inspected.value().page_count > 0,
            "Expected generated SQLite database page metadata.");
    require(inspected.value().status == storq::FormatStatus::Valid ||
                inspected.value().status == storq::FormatStatus::ValidWithWarnings,
            "Expected SQLite-generated database to pass structural inspection.");
    require_ok(runtime.shutdown(), "Shutdown generated-database runtime");
}
#endif

} // namespace

int main() {
    try {
        const auto directory = make_temp_directory();
        auto cleanup = storq::make_scope_exit([&directory] {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        });
        test_header_helpers();
        test_inspection_and_unknown(directory);
        test_writable_handle_rejected();
#if defined(STORQ_TEST_SQLITE3)
        test_sqlite_generated_database(directory);
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Format test failure: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "Format inspection tests passed.\n";
    return 0;
}
