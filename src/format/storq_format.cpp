#include <storq/format/storq_format.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <random>

#include <storq/common/utilities.hpp>
#include <storq/storage/storage.hpp>

namespace storq {
namespace {

constexpr std::array<std::byte, 8> storq_magic{
    std::byte{'S'}, std::byte{'T'}, std::byte{'O'}, std::byte{'R'},
    std::byte{'Q'}, std::byte{'D'}, std::byte{'B'}, std::byte{0}};
constexpr std::uint32_t storq_page_magic = 0x51525354;
constexpr std::size_t metadata_payload_size = 72;

StorqError format_error(std::string message) {
    return {ErrorCategory::Parsing, ErrorCode::InvalidArgument, std::move(message), {}};
}

void write_u16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
}
void write_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
}
void write_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
}
std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset])) |
           (static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset + 1])) << 8);
}
std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value |= std::uint32_t{std::to_integer<unsigned>(bytes[offset + i])} << (8 * i);
    return value;
}
std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) value |= std::uint64_t{std::to_integer<unsigned>(bytes[offset + i])} << (8 * i);
    return value;
}

std::array<std::byte, 16> uuid_bytes(const std::string& uuid) {
    std::array<std::byte, 16> result{};
    std::size_t value_index = 0;
    for (char c : uuid) {
        if (c == '-') continue;
        if (value_index >= result.size()) break;
        unsigned nibble = 0;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else continue;
        const auto byte_index = value_index / 2;
        if ((value_index++ & 1) == 0) result[byte_index] = static_cast<std::byte>(nibble << 4);
        else result[byte_index] |= static_cast<std::byte>(nibble);
    }
    return result;
}

std::string uuid_string(std::span<const std::byte> bytes) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result(36, '0');
    std::size_t output = 0;
    static constexpr std::array<std::size_t, 4> hyphen_positions = {8, 13, 18, 23};
    for (std::size_t source = 0; source < bytes.size(); ++source) {
        if (std::find(hyphen_positions.begin(), hyphen_positions.end(), output) != hyphen_positions.end())
            result[output++] = '-';
        const auto value = std::to_integer<unsigned>(bytes[source]);
        result[output++] = hex[(value >> 4) & 0xf];
        result[output++] = hex[value & 0xf];
    }
    return result;
}

std::vector<std::byte> metadata_payload(
    const StorqFileHeader& header, const std::array<std::byte, 16>& uuid) {
    std::vector<std::byte> payload(metadata_payload_size, std::byte{0});
    write_u32(payload, 0, header.major_version);
    write_u32(payload, 4, header.minor_version);
    std::copy(uuid.begin(), uuid.end(), payload.begin() + 8);
    write_u64(payload, 24, header.creation_timestamp_ms);
    write_u64(payload, 32, header.page_count);
    write_u64(payload, 40, header.metadata_generation);
    write_u64(payload, 48, header.metadata_root_page_id);
    write_u64(payload, 56, storq_missing_page_id);
    write_u64(payload, 64, storq_missing_page_id);
    return payload;
}

Result<StorqFormatMetadata> parse_file(StorageFile& file) {
    auto size = file.size();
    if (!size) return size.error();
    if (size.value() < storq_header_size) return format_error("Truncated Storq file header.");
    std::array<std::byte, storq_header_size> raw{};
    auto read = file.read_at(0, raw);
    if (!read || read.value() != raw.size()) return format_error("Unable to read complete Storq file header.");
    auto header = StorqFormatHeader::deserialize(raw);
    if (!header) return header.error();
    if (size.value() < storq_header_size + header.value().page_size) return format_error("Truncated Storq metadata page.");

    std::vector<std::byte> page(header.value().page_size);
    read = file.read_at(storq_header_size, page);
    if (!read || read.value() != page.size()) return format_error("Unable to read complete Storq metadata page.");
    auto page_header = StorqPage::deserialize_header(page);
    if (!page_header) return page_header.error();
    if (page_header.value().type != StorqPageType::Metadata || page_header.value().page_id != 1)
        return format_error("Invalid Storq metadata page identity.");
    if (page_header.value().payload_length != metadata_payload_size || page_header.value().reserved != 0)
        return format_error("Invalid Storq metadata payload.");
    std::vector<std::byte> checksum_page(page);
    write_u32(checksum_page, 24, 0);
    if (page_header.value().checksum != CRC32C::calculate(checksum_page))
        return format_error("Storq metadata page checksum mismatch.");

    StorqFormatMetadata metadata;
    metadata.header = header.value();
    metadata.database_uuid = header.value().database_uuid;
    metadata.database_uuid_string = uuid_string(metadata.database_uuid);
    metadata.metadata_generation = read_u64(page, 32 + 40);
    metadata.catalog_page_id = read_u64(page, 32 + 56);
    metadata.free_space_page_id = read_u64(page, 32 + 64);
    metadata.physical_file_size = size.value();
    const auto compatibility = StorqFormatVersionValidator::check({header.value().major_version, header.value().minor_version});
    if (compatibility == StorqFormatCompatibility::UnsupportedMajor ||
        compatibility == StorqFormatCompatibility::InvalidVersion)
        return format_error("Unsupported Storq format version.");
    if (header.value().feature_flags != 0)
        return format_error("Storq database requires unsupported format features.");
    if (header.value().metadata_root_page_id != 1 || header.value().page_count == 0)
        return format_error("Invalid Storq metadata root or page count.");
    if (metadata.metadata_generation != header.value().metadata_generation ||
        metadata.catalog_page_id != storq_missing_page_id ||
        metadata.free_space_page_id != storq_missing_page_id ||
        !std::equal(metadata.database_uuid.begin(), metadata.database_uuid.end(), page.begin() + 32 + 8))
        return format_error("Storq header and metadata page are inconsistent.");
    if (size.value() != storq_header_size + header.value().page_count * header.value().page_size)
        return format_error("Storq physical file size does not match its declared page count.");
    return metadata;
}

} // namespace

Result<void> BinaryReader::read_u16(std::uint16_t& value) {
    if (offset_ > bytes_.size() || bytes_.size() - offset_ < 2) return format_error("Two-byte binary read exceeds bounds.");
    value = ::storq::read_u16(bytes_, offset_); offset_ += 2; return {};
}
Result<void> BinaryReader::read_u32(std::uint32_t& value) {
    if (offset_ > bytes_.size() || bytes_.size() - offset_ < 4) return format_error("Four-byte binary read exceeds bounds.");
    value = ::storq::read_u32(bytes_, offset_); offset_ += 4; return {};
}
Result<void> BinaryReader::read_u64(std::uint64_t& value) {
    if (offset_ > bytes_.size() || bytes_.size() - offset_ < 8) return format_error("Eight-byte binary read exceeds bounds.");
    value = ::storq::read_u64(bytes_, offset_); offset_ += 8; return {};
}
Result<void> BinaryReader::read_bytes(std::span<std::byte> output) {
    if (offset_ > bytes_.size() || output.size() > bytes_.size() - offset_) return format_error("Byte-array read exceeds bounds.");
    std::copy_n(bytes_.begin() + offset_, output.size(), output.begin());
    offset_ += output.size(); return {};
}
Result<void> BinaryWriter::write_u16(std::uint16_t value) {
    if (offset_ > bytes_.size() || bytes_.size() - offset_ < 2) return format_error("Two-byte binary write exceeds bounds.");
    ::storq::write_u16(bytes_, offset_, value); offset_ += 2; return {};
}
Result<void> BinaryWriter::write_u32(std::uint32_t value) {
    if (offset_ > bytes_.size() || bytes_.size() - offset_ < 4) return format_error("Four-byte binary write exceeds bounds.");
    ::storq::write_u32(bytes_, offset_, value); offset_ += 4; return {};
}
Result<void> BinaryWriter::write_u64(std::uint64_t value) {
    if (offset_ > bytes_.size() || bytes_.size() - offset_ < 8) return format_error("Eight-byte binary write exceeds bounds.");
    ::storq::write_u64(bytes_, offset_, value); offset_ += 8; return {};
}
Result<void> BinaryWriter::write_bytes(std::span<const std::byte> input) {
    if (offset_ > bytes_.size() || input.size() > bytes_.size() - offset_) return format_error("Byte-array write exceeds bounds.");
    std::copy(input.begin(), input.end(), bytes_.begin() + offset_);
    offset_ += input.size(); return {};
}

std::uint32_t CRC32C::calculate(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = UINT32_MAX;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint32_t>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82f63b78U & (0U - (crc & 1U)));
    }
    return crc ^ UINT32_MAX;
}

Result<std::array<std::byte, storq_header_size>> StorqFormatHeader::serialize(const StorqFileHeader& header) {
    std::array<std::byte, storq_header_size> bytes{};
    std::copy(storq_magic.begin(), storq_magic.end(), bytes.begin());
    write_u32(bytes, 8, header.major_version); write_u32(bytes, 12, header.minor_version);
    write_u32(bytes, 16, storq_header_size); write_u32(bytes, 20, header.page_size);
    write_u32(bytes, 24, header.feature_flags); write_u32(bytes, 28, header.compatibility_flags);
    std::copy(header.database_uuid.begin(), header.database_uuid.end(), bytes.begin() + 32);
    write_u64(bytes, 48, header.creation_timestamp_ms); write_u64(bytes, 56, header.metadata_root_page_id);
    write_u64(bytes, 64, header.page_count); write_u64(bytes, 72, header.metadata_generation);
    write_u32(bytes, 80, header.encoding); write_u32(bytes, 84, header.checksum_algorithm);
    write_u32(bytes, 88, 0); write_u32(bytes, 92, 0);
    write_u32(bytes, 88, calculate_checksum(bytes));
    return bytes;
}

Result<StorqFileHeader> StorqFormatHeader::deserialize(std::span<const std::byte> bytes) {
    if (bytes.size() < storq_header_size) return format_error("Truncated Storq header.");
    if (!std::equal(storq_magic.begin(), storq_magic.end(), bytes.begin())) return format_error("Invalid Storq file magic.");
    if (read_u32(bytes, 16) != storq_header_size) return format_error("Invalid Storq header size.");
    if (!StorqPage::is_valid_page_size(read_u32(bytes, 20))) return format_error("Unsupported Storq page size.");
    if (read_u32(bytes, 92) != 0) return format_error("Storq reserved header field is non-zero.");
    std::array<std::byte, storq_header_size> checksum_bytes;
    std::copy_n(bytes.begin(), checksum_bytes.size(), checksum_bytes.begin());
    write_u32(checksum_bytes, 88, 0);
    if (read_u32(bytes, 88) != calculate_checksum(checksum_bytes))
        return format_error("Storq header checksum mismatch.");
    StorqFileHeader header;
    header.magic = storq_magic; header.major_version = read_u32(bytes, 8); header.minor_version = read_u32(bytes, 12);
    header.header_size = read_u32(bytes, 16); header.page_size = read_u32(bytes, 20);
    header.feature_flags = read_u32(bytes, 24); header.compatibility_flags = read_u32(bytes, 28);
    std::copy_n(bytes.begin() + 32, 16, header.database_uuid.begin());
    header.creation_timestamp_ms = read_u64(bytes, 48); header.metadata_root_page_id = read_u64(bytes, 56);
    header.page_count = read_u64(bytes, 64); header.metadata_generation = read_u64(bytes, 72);
    header.encoding = read_u32(bytes, 80); header.checksum_algorithm = read_u32(bytes, 84); header.checksum = read_u32(bytes, 88);
    return header;
}
std::uint32_t StorqFormatHeader::calculate_checksum(std::span<const std::byte> header_bytes) noexcept {
    return CRC32C::calculate(header_bytes);
}

bool StorqPage::is_valid_page_size(std::uint32_t page_size) noexcept {
    return page_size == 4096 || page_size == 8192 || page_size == 16384 || page_size == 32768 || page_size == 65536;
}

Result<std::vector<std::byte>> StorqPage::serialize(std::uint32_t page_size, const StorqPageHeader& header, std::span<const std::byte> payload) {
    if (!is_valid_page_size(page_size) || payload.size() > page_size - storq_page_header_size)
        return format_error("Invalid Storq page size or payload length.");
    if (header.payload_length != payload.size()) return format_error("Storq page payload length is inconsistent.");
    std::vector<std::byte> page(page_size, std::byte{0});
    write_u32(page, 0, storq_page_magic); write_u16(page, 4, static_cast<std::uint16_t>(header.type));
    write_u16(page, 6, header.flags); write_u64(page, 8, header.page_id);
    write_u32(page, 16, header.generation); write_u32(page, 20, header.payload_length);
    write_u32(page, 24, 0); write_u32(page, 28, header.reserved);
    std::copy(payload.begin(), payload.end(), page.begin() + storq_page_header_size);
    write_u32(page, 24, calculate_checksum(page));
    return page;
}

Result<StorqPageHeader> StorqPage::deserialize_header(std::span<const std::byte> bytes) {
    if (bytes.size() < storq_page_header_size) return format_error("Truncated Storq page header.");
    if (read_u32(bytes, 0) != storq_page_magic) return format_error("Invalid Storq page magic.");
    StorqPageHeader header;
    header.magic = read_u32(bytes, 0);
    const auto type = read_u16(bytes, 4);
    if (type > static_cast<std::uint16_t>(StorqPageType::Overflow) || type == 0) return format_error("Unsupported Storq page type.");
    header.type = static_cast<StorqPageType>(type); header.flags = read_u16(bytes, 6);
    header.page_id = read_u64(bytes, 8); header.generation = read_u32(bytes, 16);
    header.payload_length = read_u32(bytes, 20); header.checksum = read_u32(bytes, 24); header.reserved = read_u32(bytes, 28);
    if (header.reserved != 0) return format_error("Storq page reserved field is non-zero.");
    if (header.payload_length > bytes.size() - storq_page_header_size) return format_error("Storq page payload length exceeds page size.");
    std::vector<std::byte> checksum_bytes(bytes.begin(), bytes.end());
    write_u32(checksum_bytes, 24, 0);
    if (header.checksum != calculate_checksum(checksum_bytes)) return format_error("Storq page checksum mismatch.");
    return header;
}
std::uint32_t StorqPage::calculate_checksum(std::span<const std::byte> page) noexcept {
    return CRC32C::calculate(page);
}

StorqFormatCompatibility StorqFormatVersionValidator::check(const StorqFormatVersion& version) noexcept {
    if (version.major == 0 || version.major == UINT32_MAX) return StorqFormatCompatibility::InvalidVersion;
    if (version.major != storq_format_major_version) return StorqFormatCompatibility::UnsupportedMajor;
    if (version.minor > storq_format_minor_version) return StorqFormatCompatibility::NewerMinorSupported;
    if (version.minor < storq_format_minor_version) return StorqFormatCompatibility::OlderSupported;
    return StorqFormatCompatibility::Supported;
}

Result<void> StorqFormatCreator::create(const std::filesystem::path& path, std::uint32_t page_size) {
    if (!StorqPage::is_valid_page_size(page_size)) return format_error("Unsupported Storq page size.");
    if (path.empty()) return format_error("Storq database path is empty.");
    StorageManager manager;
    StorageOpenOptions options;
    options.access = StorageAccess::CreateNew;
    options.create_directories = false;
    auto file_result = manager.open_file(path, options);
    if (!file_result) {
        if (std::filesystem::exists(path)) return format_error("Storq database already exists.");
        return file_result.error();
    }
    auto file = file_result.value();
    std::string uuid = generate_uuid();
    auto uuid_value = uuid_bytes(uuid);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    StorqFileHeader header;
    header.database_uuid = uuid_value;
    header.creation_timestamp_ms = static_cast<std::uint64_t>(now);
    header.page_size = page_size;
    header.page_count = 1;
    auto serialized_header = StorqFormatHeader::serialize(header);
    auto payload = metadata_payload(header, uuid_value);
    StorqPageHeader page_header;
    page_header.payload_length = static_cast<std::uint32_t>(payload.size());
    auto page = StorqPage::serialize(page_size, page_header, payload);
    if (!serialized_header || !page) return serialized_header ? page.error() : serialized_header.error();
    auto written_header = file->write_at(0, serialized_header.value());
    if (!written_header || written_header.value() != serialized_header.value().size()) return format_error("Unable to write Storq file header.");
    auto written_page = file->write_at(storq_header_size, page.value());
    if (!written_page || written_page.value() != page.value().size()) return format_error("Unable to write Storq metadata page.");
    auto synced = file->sync();
    if (!synced) return synced.error();
    static_cast<void>(manager.close_all());
    auto opened = StorqFormatOpener::open(path, true);
    if (!opened) return opened.error();
    return {};
}

Result<StorqFormatMetadata> StorqFormatOpener::open(const std::filesystem::path& path, bool read_only) {
    StorageManager manager;
    StorageOpenOptions options;
    options.access = read_only ? StorageAccess::ReadOnly : StorageAccess::ReadWrite;
    auto file = manager.open_file(path, options);
    if (!file) return file.error();
    auto metadata = parse_file(*file.value());
    static_cast<void>(manager.close_all());
    return metadata;
}

Result<std::array<std::byte, storq_header_size>> StorqFormatReader::read_header(StorageFile& file) {
    std::array<std::byte, storq_header_size> bytes{};
    auto read = file.read_at(0, bytes);
    if (!read || read.value() != bytes.size()) return format_error("Unable to read Storq file header.");
    return bytes;
}
Result<std::vector<std::byte>> StorqFormatReader::read_metadata_page(StorageFile& file, std::uint32_t page_size) {
    if (!StorqPage::is_valid_page_size(page_size)) return format_error("Unsupported Storq page size.");
    std::vector<std::byte> page(page_size);
    auto read = file.read_at(storq_header_size, page);
    if (!read || read.value() != page.size()) return format_error("Unable to read Storq metadata page.");
    return page;
}

Result<StorqValidationReport> StorqFormatValidator::validate(const std::filesystem::path& path) {
    StorqValidationReport report;
    auto metadata = StorqFormatOpener::open(path, true);
    if (!metadata) return metadata.error();
    report.metadata = metadata.value();
    report.findings.push_back({"header_valid", "File header magic, layout, page size, and checksum are valid.", true});
    report.findings.push_back({"metadata_valid", "Metadata page layout, checksum, and header consistency are valid.", true});
    report.findings.push_back({"compatibility_valid", "Format version and feature set are supported.", true});
    return report;
}

Result<void> StorqFormat::create(const std::filesystem::path& path, std::uint32_t page_size) {
    return StorqFormatCreator::create(path, page_size);
}
Result<StorqFormatMetadata> StorqFormat::open(const std::filesystem::path& path, bool read_only) {
    return StorqFormatOpener::open(path, read_only);
}
Result<StorqValidationReport> StorqFormat::validate(const std::filesystem::path& path) {
    return StorqFormatValidator::validate(path);
}
StorqFormatCompatibility StorqFormat::compatibility(std::uint32_t major, std::uint32_t minor) noexcept {
    return StorqFormatVersionValidator::check({major, minor});
}

} // namespace storq
