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

constexpr std::uint32_t page_size = 512;
constexpr std::uint32_t salt_1 = 0x12345678;
constexpr std::uint32_t salt_2 = 0x90abcdef;

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

std::vector<std::byte> checksum_input(
    const std::vector<std::byte>& frame_header,
    const std::vector<std::byte>& page_data) {
    std::vector<std::byte> input;
    input.insert(input.end(), frame_header.begin(), frame_header.begin() + 8);
    input.insert(input.end(), page_data.begin(), page_data.end());
    return input;
}

std::vector<std::byte> make_wal(
    std::uint32_t magic, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& frames) {
    const auto byte_order = magic == 0x377f0682U
        ? storq::WALChecksumByteOrder::LittleEndian
        : storq::WALChecksumByteOrder::BigEndian;
    std::vector<std::byte> bytes(32, std::byte{0});
    write_be32(bytes, 0, magic);
    write_be32(bytes, 4, 3007000);
    write_be32(bytes, 8, page_size);
    write_be32(bytes, 12, 9);
    write_be32(bytes, 16, salt_1);
    write_be32(bytes, 20, salt_2);
    auto header_checksum = storq::SQLiteWALChecksum::calculate(
        std::span<const std::byte>(bytes).first(24), byte_order);
    require(header_checksum.has_value(), "Unable to calculate test WAL header checksum.");
    write_be32(bytes, 24, header_checksum.value().first);
    write_be32(bytes, 28, header_checksum.value().second);

    auto rolling = header_checksum.value();
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto frame_offset = bytes.size();
        bytes.resize(frame_offset + 24 + page_size, std::byte{0});
        write_be32(bytes, frame_offset, frames[index].first);
        write_be32(bytes, frame_offset + 4, frames[index].second);
        write_be32(bytes, frame_offset + 8, salt_1);
        write_be32(bytes, frame_offset + 12, salt_2);
        std::vector<std::byte> page(page_size, static_cast<std::byte>(index + 1));
        std::copy(page.begin(), page.end(), bytes.begin() + frame_offset + 24);
        std::vector<std::byte> frame_header(24);
        std::copy_n(bytes.begin() + frame_offset, 24, frame_header.begin());
        const auto input = checksum_input(frame_header, page);
        auto checksum = storq::SQLiteWALChecksum::calculate(input, byte_order, rolling);
        require(checksum.has_value(), "Unable to calculate test WAL frame checksum.");
        write_be32(bytes, frame_offset + 16, checksum.value().first);
        write_be32(bytes, frame_offset + 20, checksum.value().second);
        rolling = checksum.value();
    }
    return bytes;
}

std::filesystem::path make_temp_directory() {
    auto path = std::filesystem::temp_directory_path() /
                ("storq-wal-" + storq::generate_uuid());
    std::filesystem::create_directories(path);
    return path;
}

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "Unable to create WAL fixture.");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "Unable to write WAL fixture.");
}

std::vector<char> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Unable to read WAL fixture.");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_checksum_byte_orders() {
    const std::array<std::byte, 8> bytes{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    auto little = storq::SQLiteWALChecksum::calculate(
        bytes, storq::WALChecksumByteOrder::LittleEndian);
    require(little.has_value() && little.value().first == 0x04030201U &&
                little.value().second == 0x0c0a0806U,
            "Expected little-endian SQLite rolling checksum.");
    auto big = storq::SQLiteWALChecksum::calculate(
        bytes, storq::WALChecksumByteOrder::BigEndian);
    require(big.has_value() && big.value().first == 0x01020304U &&
                big.value().second == 0x06080a0cU,
            "Expected big-endian SQLite rolling checksum.");
    auto rolling = storq::SQLiteWALChecksum::calculate(
        bytes, storq::WALChecksumByteOrder::BigEndian, {1, 2});
    require(rolling.has_value() && rolling.value().first == 0x01020307U &&
                rolling.value().second == 0x06080a11U,
            "Expected checksum accumulation from prior state.");
    require(!storq::SQLiteWALChecksum::calculate(
                std::span<const std::byte>(bytes).first(7),
                storq::WALChecksumByteOrder::BigEndian),
            "Expected checksum input with an invalid length to fail.");
}

void test_wal_fixtures(const std::filesystem::path& directory) {
    storq::Runtime runtime;
    auto analyzer = std::make_shared<storq::WALAnalyzer>(runtime);
    require_ok(runtime.modules().register_module(analyzer), "Register WAL analyzer");
    require_ok(runtime.initialize(), "Initialize WAL analyzer");
    require(runtime.capabilities().contains("wal_analysis.sqlite"),
            "Expected SQLite WAL-analysis capability.");

    for (const auto magic : {0x377f0682U, 0x377f0683U}) {
        const auto path = directory / (magic == 0x377f0682U ? "little.wal" : "big.wal");
        auto bytes = make_wal(magic, {{1, 0}, {2, 2}, {1, 0}});
        write_file(path, bytes);
        auto report = analyzer->inspect(path);
        require(report.has_value(), "Expected valid WAL fixture inspection.");
        require(report.value().header.magic == magic && report.value().header_valid &&
                    report.value().checksums_valid,
                "Expected valid WAL header and checksum chain.");
        require(report.value().total_frames == 3 && report.value().valid_frames == 3,
                "Expected all generated frames to validate.");
        require(report.value().committed_transactions == 1 &&
                    report.value().incomplete_transactions == 1 &&
                    report.value().last_commit_frame == 2,
                "Expected commit and incomplete transaction boundaries.");
        require(report.value().transactions.size() == 2 &&
                    report.value().transactions[0].committed &&
                    !report.value().transactions[1].committed,
                "Expected transaction grouping around the commit marker.");
        require(report.value().checkpoint.checkpoint_sequence == 9 &&
                    !report.value().checkpoint.exact_checkpoint_state_known,
                "Expected observed checkpoint metadata without an exact state claim.");
        const auto history = storq::FrameAnalyzer::page_history(report.value(), 1);
        require(history == std::vector<std::uint64_t>({1, 3}),
                "Expected repeated-page history to preserve frame order.");
        require(!report.value().timeline.empty() &&
                    report.value().timeline.front().type == storq::WALEventType::HeaderValidated,
                "Expected ordered WAL timeline.");
        require(storq::export_wal_report_json(report.value()).front() == '{',
                "Expected JSON-compatible report output.");

        storq::StorageManager manager;
        storq::StorageOpenOptions options;
        options.access = storq::StorageAccess::ReadOnly;
        auto file = manager.open_file(path, options);
        require(file.has_value(), "Unable to open test WAL read-only.");
        auto frame = analyzer->inspect_frame(*file.value(), 2);
        require(frame.has_value() && frame.value().frame.is_commit() &&
                    frame.value().frame.checksum_valid &&
                    frame.value().page_data.size() == page_size,
                "Expected selected frame inspection.");
        require_ok(file.value()->close(), "Close fixture WAL");

        bytes[32 + 24 + 20] ^= std::byte{1};
        const auto damaged_path = directory /
            (magic == 0x377f0682U ? "damaged-little.wal" : "damaged-big.wal");
        write_file(damaged_path, bytes);
        auto damaged = analyzer->inspect(damaged_path);
        require(damaged.has_value() && damaged.value().invalid_frames > 0 &&
                    !damaged.value().checksums_valid,
                "Expected modified frame page data to fail rolling checksum.");
    }

    auto truncated_bytes = make_wal(0x377f0682U, {{1, 1}});
    truncated_bytes.resize(truncated_bytes.size() - 3);
    const auto truncated_path = directory / "truncated.wal";
    write_file(truncated_path, truncated_bytes);
    auto truncated = analyzer->inspect(truncated_path);
    require(truncated.has_value() && !truncated.value().complete_analysis &&
                std::any_of(truncated.value().issues.begin(), truncated.value().issues.end(),
                            [](const storq::WALIssue& issue) {
                                return issue.type == storq::WALIssueType::TruncatedFrame;
                            }),
            "Expected trailing incomplete frame detection.");

    auto tiny_path = directory / "tiny.wal";
    write_file(tiny_path, {std::byte{1}, std::byte{2}, std::byte{3}});
    auto tiny = analyzer->inspect(tiny_path);
    require(tiny.has_value() && std::any_of(
                tiny.value().issues.begin(), tiny.value().issues.end(),
                [](const storq::WALIssue& issue) {
                    return issue.type == storq::WALIssueType::TruncatedHeader;
                }),
            "Expected truncated WAL header detection.");

    auto invalid_magic_bytes = make_wal(0x377f0682U, {});
    write_be32(invalid_magic_bytes, 0, 0x12345678U);
    const auto invalid_magic_path = directory / "invalid-magic.wal";
    write_file(invalid_magic_path, invalid_magic_bytes);
    auto invalid_magic = analyzer->inspect(invalid_magic_path);
    require(invalid_magic.has_value() && std::any_of(
                invalid_magic.value().issues.begin(), invalid_magic.value().issues.end(),
                [](const storq::WALIssue& issue) {
                    return issue.type == storq::WALIssueType::InvalidMagic;
                }),
            "Expected invalid WAL magic reporting.");

    auto limited_bytes = make_wal(0x377f0682U, {{1, 0}, {2, 2}});
    const auto limited_path = directory / "limited.wal";
    write_file(limited_path, limited_bytes);
    storq::WALAnalysisOptions limits;
    limits.limits.maximum_frames = 1;
    auto limited = analyzer->inspect(limited_path, limits);
    require(limited.has_value() && !limited.value().complete_analysis &&
                limited.value().frames.size() == 1,
            "Expected configured frame limit enforcement.");

    require_ok(runtime.shutdown(), "Shutdown WAL analyzer");
}

#if defined(STORQ_TEST_SQLITE3)
std::filesystem::path create_sqlite_wal(
    const std::filesystem::path& directory, sqlite3*& active_database) {
    const auto path = directory / "sqlite-generated.sqlite";
    require(sqlite3_open(path.string().c_str(), &active_database) == SQLITE_OK,
            "Unable to create SQLite WAL database.");
    char* error = nullptr;
    const char* setup =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE events(value TEXT);"
        "BEGIN;"
        "INSERT INTO events VALUES('first');"
        "INSERT INTO events VALUES('second');"
        "COMMIT;";
    const auto result = sqlite3_exec(active_database, setup, nullptr, nullptr, &error);
    const std::string message = error ? error : "";
    sqlite3_free(error);
    require(result == SQLITE_OK, message.c_str());
    return path;
}

void test_sqlite_generated_wal(const std::filesystem::path& directory) {
    sqlite3* database = nullptr;
    const auto database_path = create_sqlite_wal(directory, database);
    const auto wal_path = std::filesystem::path(database_path.string() + "-wal");
    require(std::filesystem::exists(wal_path), "SQLite did not leave a WAL file.");
    const auto before = read_file(wal_path);

    storq::Runtime runtime;
    auto analyzer = std::make_shared<storq::WALAnalyzer>(runtime);
    require_ok(runtime.modules().register_module(analyzer), "Register SQLite WAL analyzer");
    require_ok(runtime.initialize(), "Initialize SQLite WAL analyzer");
    auto report = analyzer->inspect(wal_path);
    require(report.has_value() && report.value().header_valid &&
                report.value().checksums_valid && report.value().valid_frames > 0 &&
                report.value().committed_transactions > 0,
            "Expected generated SQLite WAL header, frames, and transactions to validate.");
    require(read_file(wal_path) == before,
            "WAL inspection modified the SQLite-generated WAL.");
    require_ok(runtime.shutdown(), "Shutdown SQLite WAL analyzer");
    require(sqlite3_close(database) == SQLITE_OK,
            "Unable to close SQLite WAL database.");
}
#endif

} // namespace

int main() {
    try {
        test_checksum_byte_orders();
        const auto directory = make_temp_directory();
        auto cleanup = storq::make_scope_exit([&directory] {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        });
        test_wal_fixtures(directory);
#if defined(STORQ_TEST_SQLITE3)
        test_sqlite_generated_wal(directory);
#endif
    } catch (const std::exception& exception) {
        std::cerr << "WAL test failure: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "WAL analysis tests passed.\n";
    return 0;
}
