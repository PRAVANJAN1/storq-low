#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include <storq/storq.hpp>

namespace {

void write_be32(std::array<std::byte, 32>& bytes, std::size_t offset,
                std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<std::byte>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<std::byte>(value & 0xff);
}

} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("storq-truncated-wal-" + storq::generate_uuid() + ".wal");
    auto cleanup = storq::make_scope_exit([&path] {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    });

    std::array<std::byte, 32> header{};
    write_be32(header, 0, 0x377f0682U);
    write_be32(header, 4, 3007000U);
    write_be32(header, 8, 4096U);
    write_be32(header, 12, 1U);
    write_be32(header, 16, 0x10203040U);
    write_be32(header, 20, 0x50607080U);
    auto checksum = storq::SQLiteWALChecksum::calculate(
        std::span<const std::byte>(header).first(24),
        storq::WALChecksumByteOrder::LittleEndian);
    if (!checksum) {
        std::cerr << checksum.error().message << '\n';
        return 1;
    }
    write_be32(header, 24, checksum.value().first);
    write_be32(header, 28, checksum.value().second);

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        std::cerr << "Unable to create isolated truncated WAL fixture.\n";
        return 1;
    }
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    std::array<std::byte, 24 + 128> partial_frame{};
    output.write(reinterpret_cast<const char*>(partial_frame.data()),
                 static_cast<std::streamsize>(partial_frame.size()));
    output.close();
    if (!output) {
        std::cerr << "Unable to write truncated WAL fixture.\n";
        return 1;
    }

    storq::Runtime runtime;
    auto analyzer = std::make_shared<storq::WALAnalyzer>(runtime);
    auto registered = runtime.modules().register_module(analyzer);
    if (!registered) {
        std::cerr << registered.error().message << '\n';
        return 1;
    }
    auto initialized = runtime.initialize();
    if (!initialized) {
        std::cerr << initialized.error().message << '\n';
        return 1;
    }
    auto result = analyzer->inspect(path);
    if (!result) {
        std::cerr << result.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    std::cout << storq::format_wal_report(result.value());
    auto shutdown = runtime.shutdown();
    if (!shutdown) {
        std::cerr << shutdown.error().message << '\n';
        return 1;
    }
    return result.value().complete_analysis ? 1 : 0;
}
