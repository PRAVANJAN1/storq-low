#include <storq/storq.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

namespace {

void write_be32(std::vector<std::byte>& bytes, std::size_t offset,
                std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<std::byte>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<std::byte>(value & 0xff);
}

storq::Result<std::vector<std::byte>> make_wal() {
    constexpr std::uint32_t page_size = 512;
    std::vector<std::byte> bytes(32 + 24 + page_size, std::byte{0});
    write_be32(bytes, 0, 0x377f0682U);
    write_be32(bytes, 4, 3007000U);
    write_be32(bytes, 8, page_size);
    write_be32(bytes, 12, 1U);
    write_be32(bytes, 16, 0x12345678U);
    write_be32(bytes, 20, 0x90abcdefU);
    auto header_checksum = storq::SQLiteWALChecksum::calculate(
        std::span<const std::byte>(bytes).first(24),
        storq::WALChecksumByteOrder::LittleEndian);
    if (!header_checksum) return header_checksum.error();
    write_be32(bytes, 24, header_checksum.value().first);
    write_be32(bytes, 28, header_checksum.value().second);

    write_be32(bytes, 32, 1U);
    write_be32(bytes, 36, 1U);
    write_be32(bytes, 40, 0x12345678U);
    write_be32(bytes, 44, 0x90abcdefU);
    std::fill(bytes.begin() + 56, bytes.end(), std::byte{0x41});
    std::vector<std::byte> checksum_input;
    checksum_input.insert(checksum_input.end(), bytes.begin() + 32, bytes.begin() + 40);
    checksum_input.insert(checksum_input.end(), bytes.begin() + 56, bytes.end());
    auto frame_checksum = storq::SQLiteWALChecksum::calculate(
        checksum_input, storq::WALChecksumByteOrder::LittleEndian,
        header_checksum.value());
    if (!frame_checksum) return frame_checksum.error();
    write_be32(bytes, 48, frame_checksum.value().first);
    write_be32(bytes, 52, frame_checksum.value().second);
    return bytes;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("storq-wal-fault-" + storq::generate_uuid());
    std::error_code filesystem_error;
    if (!std::filesystem::create_directory(root, filesystem_error) || filesystem_error) {
        std::cerr << "Unable to create isolated WAL test directory: "
                  << filesystem_error.message() << '\n';
        return 1;
    }
    const auto source_path = root / "source.wal";
    const auto copy_path = root / "working.wal";
    int exit_code = 0;
    {
        storq::Runtime runtime;
        storq::StorageManager storage;
        auto injector = std::make_shared<storq::FaultInjector>(runtime);
        auto analyzer = std::make_shared<storq::WALAnalyzer>(runtime);
        auto registered = runtime.modules().register_module(injector);
        if (registered) registered = runtime.modules().register_module(analyzer);
        if (!registered) {
            std::cerr << registered.error().message << '\n';
            exit_code = 1;
        }
        auto initialized = exit_code == 0 ? runtime.initialize()
                                          : storq::Result<void>(registered.error());
        if (!initialized) {
            std::cerr << initialized.error().message << '\n';
            exit_code = 1;
        }
        if (exit_code == 0) {
            auto configured = injector->configure_environment({root, true, true, true});
            if (!configured) {
                std::cerr << configured.error().message << '\n';
                exit_code = 1;
            }
        }
        auto wal = make_wal();
        if (!wal) {
            std::cerr << wal.error().message << '\n';
            exit_code = 1;
        }
        storq::StorageOpenOptions create_options;
        create_options.access = storq::StorageAccess::CreateNew;
        if (exit_code == 0) {
            auto original = storage.open_file(source_path, create_options);
            if (!original) {
                std::cerr << original.error().message << '\n';
                exit_code = 1;
            } else {
                auto written = original.value()->write_at(0, wal.value());
                if (!written || written.value() != wal.value().size()) {
                    std::cerr << (written ? "Incomplete WAL fixture write."
                                          : written.error().message) << '\n';
                    exit_code = 1;
                }
                auto closed = original.value()->close();
                if (!closed) {
                    std::cerr << closed.error().message << '\n';
                    exit_code = 1;
                }
            }
        }
        if (exit_code == 0) {
            auto copied = injector->create_test_copy(storage, source_path, copy_path);
            if (!copied) {
                std::cerr << copied.error().message << '\n';
                exit_code = 1;
            }
        }
        if (exit_code == 0) {
            auto rule = storq::FaultRule{};
            rule.id = "corrupt-wal-frame-one";
            rule.config.type = storq::FaultType::WALRecordCorruption;
            rule.config.wal_frame_number = 1;
            rule.config.wal_region = storq::WALCorruptionRegion::PageContent;
            rule.config.corruption_length = 1;
            rule.config.corruption_pattern = {std::byte{0xff}};
            auto added = injector->add_rule(rule);
            if (!added) {
                std::cerr << added.error().message << '\n';
                exit_code = 1;
            }
        }
        if (exit_code == 0) {
            storq::StorageOpenOptions writable;
            writable.access = storq::StorageAccess::ReadWrite;
            auto working = injector->open_file(storage, copy_path, writable);
            if (!working) {
                std::cerr << working.error().message << '\n';
                exit_code = 1;
            } else {
                std::vector<std::byte> page(512, std::byte{0x41});
                auto write = working.value()->write_at(56, page);
                if (!write || write.value() != page.size()) {
                    std::cerr << (write ? "Incomplete WAL write."
                                        : write.error().message) << '\n';
                    exit_code = 1;
                }
                auto closed = working.value()->close();
                if (!closed) {
                    std::cerr << closed.error().message << '\n';
                    exit_code = 1;
                }
            }
        }
        if (exit_code == 0) {
            auto report = analyzer->inspect(copy_path);
            if (!report) {
                std::cerr << report.error().message << '\n';
                exit_code = 1;
            } else {
                std::cout << storq::format_wal_report(report.value());
            }
        }
        if (exit_code == 0) {
            storq::StorageOpenOptions read_only;
            read_only.access = storq::StorageAccess::ReadOnly;
            auto original = storage.open_file(source_path, read_only);
            if (!original) {
                std::cerr << original.error().message << '\n';
                exit_code = 1;
            } else {
                std::vector<std::byte> source_bytes(wal.value().size());
                auto read = original.value()->read_at(0, source_bytes);
                if (!read || read.value() != source_bytes.size() ||
                    source_bytes != wal.value()) {
                    std::cerr << "Original WAL changed during copy corruption.\n";
                    exit_code = 1;
                }
                auto closed = original.value()->close();
                if (!closed) {
                    std::cerr << closed.error().message << '\n';
                    exit_code = 1;
                }
            }
        }
        auto cleaned = injector->cleanup_file(copy_path);
        if (!cleaned && std::filesystem::exists(copy_path)) {
            std::cerr << cleaned.error().message << '\n';
            exit_code = 1;
        }
        auto shutdown = runtime.shutdown();
        if (!shutdown) {
            std::cerr << shutdown.error().message << '\n';
            exit_code = 1;
        }
    }
    std::filesystem::remove(source_path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::remove(root, filesystem_error);
    return exit_code;
}
