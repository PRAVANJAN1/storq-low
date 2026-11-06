#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <storq/common/result.hpp>
#include <storq/format/format_inspector.hpp>

namespace storq {

inline constexpr std::uint32_t storq_format_major_version = 1;
inline constexpr std::uint32_t storq_format_minor_version = 0;
inline constexpr std::size_t storq_header_size = 128;
inline constexpr std::size_t storq_page_header_size = 32;
inline constexpr std::uint64_t storq_missing_page_id = UINT64_MAX;

enum class StorqPageType : std::uint16_t {
    ReservedSystem = 0,
    Metadata = 1,
    Catalog = 2,
    FreeSpace = 3,
    Data = 4,
    Index = 5,
    WalMetadata = 6,
    Overflow = 7
};

enum class StorqFormatCompatibility {
    Supported,
    OlderSupported,
    NewerMinorSupported,
    UnsupportedMajor,
    UnsupportedFeature,
    InvalidVersion
};

struct StorqFormatVersion {
    std::uint32_t major{storq_format_major_version};
    std::uint32_t minor{storq_format_minor_version};

    [[nodiscard]] bool is_current() const noexcept { return major == storq_format_major_version && minor == storq_format_minor_version; }
};

struct StorqFileHeader {
    std::array<std::byte, 8> magic{};
    std::uint32_t major_version{storq_format_major_version};
    std::uint32_t minor_version{storq_format_minor_version};
    std::uint32_t header_size{static_cast<std::uint32_t>(storq_header_size)};
    std::uint32_t page_size{4096};
    std::uint32_t feature_flags{0};
    std::uint32_t compatibility_flags{0};
    std::array<std::byte, 16> database_uuid{};
    std::uint64_t creation_timestamp_ms{0};
    std::uint64_t metadata_root_page_id{1};
    std::uint64_t page_count{1};
    std::uint64_t metadata_generation{1};
    std::uint32_t encoding{1};
    std::uint32_t checksum_algorithm{1};
    std::uint32_t checksum{0};
};

struct StorqPageHeader {
    std::uint32_t magic{0x51525354};
    StorqPageType type{StorqPageType::Metadata};
    std::uint16_t flags{0};
    std::uint64_t page_id{1};
    std::uint32_t generation{1};
    std::uint32_t payload_length{0};
    std::uint32_t checksum{0};
    std::uint32_t reserved{0};
};

struct StorqFormatMetadata : IFormatMetadata {
    StorqFileHeader header;
    std::array<std::byte, 16> database_uuid{};
    std::string database_uuid_string;
    std::uint64_t metadata_generation{1};
    std::uint64_t catalog_page_id{storq_missing_page_id};
    std::uint64_t free_space_page_id{storq_missing_page_id};
    std::uint64_t physical_file_size{0};
};

struct StorqValidationFinding {
    std::string code;
    std::string message;
    bool passed{true};
};

struct StorqValidationReport {
    bool is_valid{true};
    StorqFormatMetadata metadata;
    std::vector<StorqValidationFinding> findings;
};

class CRC32C {
public:
    [[nodiscard]] static std::uint32_t calculate(std::span<const std::byte> bytes) noexcept;
};

class BinaryReader {
public:
    BinaryReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}
    [[nodiscard]] Result<void> read_u16(std::uint16_t& value);
    [[nodiscard]] Result<void> read_u32(std::uint32_t& value);
    [[nodiscard]] Result<void> read_u64(std::uint64_t& value);
    [[nodiscard]] Result<void> read_bytes(std::span<std::byte> output);
    [[nodiscard]] bool finished() const noexcept { return offset_ == bytes_.size(); }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{0};
};

class BinaryWriter {
public:
    explicit BinaryWriter(std::span<std::byte> bytes) noexcept : bytes_(bytes) {}
    [[nodiscard]] Result<void> write_u16(std::uint16_t value);
    [[nodiscard]] Result<void> write_u32(std::uint32_t value);
    [[nodiscard]] Result<void> write_u64(std::uint64_t value);
    [[nodiscard]] Result<void> write_bytes(std::span<const std::byte> bytes);
    [[nodiscard]] std::size_t size() const noexcept { return offset_; }
private:
    std::span<std::byte> bytes_;
    std::size_t offset_{0};
};

class StorqFormatHeader {
public:
    [[nodiscard]] static Result<std::array<std::byte, storq_header_size>> serialize(const StorqFileHeader& header);
    [[nodiscard]] static Result<StorqFileHeader> deserialize(std::span<const std::byte> bytes);
    [[nodiscard]] static std::uint32_t calculate_checksum(std::span<const std::byte> header_bytes) noexcept;
};

class StorqPage {
public:
    [[nodiscard]] static Result<std::vector<std::byte>> serialize(
        std::uint32_t page_size, const StorqPageHeader& header,
        std::span<const std::byte> payload);
    [[nodiscard]] static Result<StorqPageHeader> deserialize_header(std::span<const std::byte> bytes);
    [[nodiscard]] static std::uint32_t calculate_checksum(std::span<const std::byte> page) noexcept;
    [[nodiscard]] static bool is_valid_page_size(std::uint32_t page_size) noexcept;
};

class StorqFormatVersionValidator {
public:
    [[nodiscard]] static StorqFormatCompatibility check(const StorqFormatVersion& version) noexcept;
};

class StorqFormatCreator {
public:
    [[nodiscard]] static Result<void> create(
        const std::filesystem::path& path, std::uint32_t page_size = 4096);
};

class StorqFormatOpener {
public:
    [[nodiscard]] static Result<StorqFormatMetadata> open(
        const std::filesystem::path& path, bool read_only = true);
};

class StorqFormatReader {
public:
    [[nodiscard]] static Result<std::array<std::byte, storq_header_size>> read_header(
        StorageFile& file);
    [[nodiscard]] static Result<std::vector<std::byte>> read_metadata_page(
        StorageFile& file, std::uint32_t page_size);
};

class StorqFormatValidator {
public:
    [[nodiscard]] static Result<StorqValidationReport> validate(
        const std::filesystem::path& path);
};

class StorqFormat {
public:
    [[nodiscard]] static Result<void> create(
        const std::filesystem::path& path, std::uint32_t page_size = 4096);
    [[nodiscard]] static Result<StorqFormatMetadata> open(
        const std::filesystem::path& path, bool read_only = true);
    [[nodiscard]] static Result<StorqValidationReport> validate(
        const std::filesystem::path& path);
    [[nodiscard]] static StorqFormatCompatibility compatibility(
        std::uint32_t major, std::uint32_t minor) noexcept;
};

class StorqNativeParser final : public StorqFormatParser {
public:
    [[nodiscard]] FormatDescriptor descriptor() const override;
    [[nodiscard]] Result<bool> matches(StorageFile& file) const override;
    [[nodiscard]] Result<FormatInspectionResult> inspect(StorageFile& file) const override;
};

} // namespace storq
