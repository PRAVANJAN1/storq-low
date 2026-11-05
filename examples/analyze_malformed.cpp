#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#include <storq/storq.hpp>

namespace {

void write_be32(std::array<unsigned char, 512>& bytes, std::size_t offset,
                std::uint32_t value) {
    bytes[offset] = static_cast<unsigned char>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<unsigned char>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<unsigned char>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<unsigned char>(value & 0xff);
}

void write_be16(std::array<unsigned char, 512>& bytes, std::size_t offset,
                std::uint16_t value) {
    bytes[offset] = static_cast<unsigned char>((value >> 8) & 0xff);
    bytes[offset + 1] = static_cast<unsigned char>(value & 0xff);
}

} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("storq-malformed-" + storq::generate_uuid() + ".sqlite");
    auto cleanup = storq::make_scope_exit([&path] {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    });

    std::array<unsigned char, 512> bytes{};
    constexpr char signature[] = "SQLite format 3";
    for (std::size_t index = 0; index < 15; ++index) {
        bytes[index] = static_cast<unsigned char>(signature[index]);
    }
    bytes[16] = 2;
    bytes[17] = 0;
    bytes[18] = 1;
    bytes[19] = 1;
    bytes[21] = 64;
    bytes[22] = 32;
    bytes[23] = 32;
    write_be32(bytes, 24, 1);
    write_be32(bytes, 28, 1);
    write_be32(bytes, 44, 4);
    write_be32(bytes, 56, 1);
    write_be32(bytes, 92, 1);
    bytes[100] = 0x0d;
    write_be16(bytes, 103, 1);
    write_be16(bytes, 105, 500);
    write_be16(bytes, 108, 50);

    {
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            std::cerr << "Unable to create isolated malformed fixture.\n";
            return 1;
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            std::cerr << "Unable to write isolated malformed fixture.\n";
            return 1;
        }
    }

    storq::Runtime runtime;
    auto analyzer = std::make_shared<storq::PageAnalyzer>(runtime);
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
    auto result = analyzer->inspect_page(path, 1);
    if (!result) {
        std::cerr << result.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    std::cout << storq::format_page_analysis_report(result.value());
    auto shutdown = runtime.shutdown();
    if (!shutdown) {
        std::cerr << shutdown.error().message << '\n';
        return 1;
    }
    return result.value().findings.empty() ? 1 : 0;
}
