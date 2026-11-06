#include <storq/wal/native_wal.hpp>

#include <array>
#include <cstring>
#include <fstream>

#include <storq/core/error.hpp>
#include <storq/format/storq_format.hpp>
#include <storq/storage/storage.hpp>

namespace storq {
namespace {

constexpr std::size_t header_size = 64;
constexpr std::size_t record_header_size = 80;
constexpr std::array<std::byte, 8> wal_magic{std::byte{'S'},std::byte{'T'},std::byte{'O'},std::byte{'R'},
                                             std::byte{'Q'},std::byte{'W'},std::byte{'L'},std::byte{0}};
constexpr std::array<std::byte, 4> record_magic{std::byte{'S'},std::byte{'W'},std::byte{'R'},std::byte{1}};

StorqError wal_error(std::string message) {
    return {ErrorCategory::Storage, ErrorCode::InvalidArgument, std::move(message), {}};
}
void put16(std::vector<std::byte>& b, std::size_t o, std::uint16_t v) noexcept {
    for (unsigned i = 0; i != 2; ++i) b[o + i] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
}
void put32(std::vector<std::byte>& b, std::size_t o, std::uint32_t v) noexcept {
    for (unsigned i = 0; i != 4; ++i) b[o + i] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
}
void put64(std::vector<std::byte>& b, std::size_t o, std::uint64_t v) noexcept {
    for (unsigned i = 0; i != 8; ++i) b[o + i] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
}
std::uint16_t get16(std::span<const std::byte> b, std::size_t o) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[o])) |
           (static_cast<std::uint16_t>(std::to_integer<unsigned>(b[o + 1])) << 8);
}
std::uint32_t get32(std::span<const std::byte> b, std::size_t o) noexcept {
    std::uint32_t v = 0;
    for (unsigned i = 0; i != 4; ++i) v |= std::uint32_t{std::to_integer<unsigned>(b[o + i])} << (8 * i);
    return v;
}
std::uint64_t get64(std::span<const std::byte> b, std::size_t o) noexcept {
    std::uint64_t v = 0;
    for (unsigned i = 0; i != 8; ++i) v |= std::uint64_t{std::uint64_t{std::to_integer<unsigned>(b[o + i])} << (8 * i)};
    return v;
}
bool valid_header_checksum(std::span<const std::byte> header) noexcept {
    std::vector<std::byte> checksum_input(header.begin(), header.end());
    put32(checksum_input, 56, 0);
    return get32(header, 56) == CRC32C::calculate(checksum_input);
}
bool valid_record_checksum(std::span<const std::byte> record) noexcept {
    std::vector<std::byte> checksum_input(record.begin(), record.end());
    put32(checksum_input, 72, 0);
    return get32(record, 72) == CRC32C::calculate(checksum_input);
}
std::vector<std::byte> make_header() {
    std::vector<std::byte> bytes(header_size, std::byte{0});
    std::copy(wal_magic.begin(), wal_magic.end(), bytes.begin());
    put32(bytes, 8, 1);
    put32(bytes, 12, 0);
    put32(bytes, 16, static_cast<std::uint32_t>(header_size));
    put32(bytes, 20, 0);
    put32(bytes, 52, 1); // CRC32C identifier
    put32(bytes, 56, 0);
    put32(bytes, 56, CRC32C::calculate(bytes));
    return bytes;
}
} // namespace

NativeWAL::NativeWAL(std::filesystem::path database_path)
    : path_(database_path.string() + ".storq-wal") {}

Result<void> NativeWAL::open() {
    StorageManager manager;
    StorageOpenOptions options;
    options.access = std::filesystem::exists(path_) ? StorageAccess::ReadWrite : StorageAccess::Create;
    auto file = manager.open_file(path_, options);
    if (!file) return file.error();
    if (!std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0) {
        auto header = make_header();
        auto written = file.value()->write_at(0, header);
        if (!written || written.value() != header.size()) return wal_error("Unable to write WAL header.");
        auto synced = file.value()->sync();
        if (!synced) return synced.error();
    } else {
        std::vector<std::byte> header(header_size);
        auto read = file.value()->read_at(0, header);
        if (!read || read.value() != header.size() ||
            !std::equal(wal_magic.begin(), wal_magic.end(), header.begin()) ||
            !valid_header_checksum(header))
            return wal_error("Invalid native WAL header.");
    }
    auto lsn = scan_last_lsn();
    if (!lsn) return lsn.error();
    last_lsn_ = lsn.value();
    return manager.close_all();
}

Result<void> NativeWAL::append(const WALRecord& record) {
    if (record.payload.size() > 64U * 1024U * 1024U)
        return wal_error("WAL payload exceeds the configured size limit.");
    if (record.transaction_id == 0 || record.type == WALRecordType::TransactionAbort &&
        record.transaction_id == 0)
        return wal_error("WAL record requires a transaction identifier.");
    std::vector<std::byte> bytes(record_header_size + record.payload.size(), std::byte{0});
    std::copy(record_magic.begin(), record_magic.end(), bytes.begin());
    put16(bytes, 4, static_cast<std::uint16_t>(record.type));
    put16(bytes, 6, 1);
    put32(bytes, 8, static_cast<std::uint32_t>(record_header_size));
    put64(bytes, 16, record.payload.size());
    put64(bytes, 24, record.lsn ? record.lsn : ++last_lsn_);
    put64(bytes, 32, record.transaction_id);
    put64(bytes, 40, record.page_id);
    put32(bytes, 48, record.page_generation);
    put64(bytes, 56, record.previous_lsn);
    std::copy(record.payload.begin(), record.payload.end(), bytes.begin() + record_header_size);
    put32(bytes, 72, CRC32C::calculate(bytes)); // record checksum also covers payload
    if (record.lsn == 0) {
        const_cast<WALRecord&>(record).lsn = get64(bytes, 24);
    }

    StorageManager manager;
    StorageOpenOptions options;
    options.access = StorageAccess::ReadWrite;
    auto file = manager.open_file(path_, options);
    if (!file) return file.error();
    auto size = file.value()->size();
    if (!size) return size.error();
    auto written = file.value()->write_at(size.value(), bytes);
    if (!written || written.value() != bytes.size()) return wal_error("Unable to append WAL record.");
    return manager.close_all();
}

Result<std::vector<WALRecord>> NativeWAL::read_all() const {
    StorageManager manager;
    StorageOpenOptions options;
    options.access = StorageAccess::ReadWrite;
    auto file = manager.open_file(path_, options);
    if (!file) return file.error();
    auto size = file.value()->size();
    if (!size) return size.error();
    if (size.value() < header_size) return wal_error("Truncated native WAL.");
    std::vector<std::byte> all(static_cast<std::size_t>(size.value()));
    auto read = file.value()->read_at(0, all);
    if (!read || read.value() != all.size()) return wal_error("Unable to read native WAL.");
    std::vector<WALRecord> records;
    std::size_t offset = header_size;
    while (offset < all.size()) {
        if (all.size() - offset < record_header_size) return wal_error("Truncated WAL record header.");
        std::span<const std::byte> header(all.data() + offset, record_header_size);
        if (!std::equal(record_magic.begin(), record_magic.end(), header.begin()))
            return wal_error("Invalid WAL record magic.");
        const auto payload_size = get64(header, 16);
        if (payload_size > 64U * 1024U * 1024U || payload_size > all.size() - offset - record_header_size)
            return wal_error("Invalid WAL record payload length.");
        const auto full_record_size = record_header_size + static_cast<std::size_t>(payload_size);
        if (!valid_record_checksum(std::span<const std::byte>(all.data() + offset, full_record_size)))
            return wal_error("Invalid WAL record checksum.");
        WALRecord record;
        record.type = static_cast<WALRecordType>(get16(header, 4));
        record.lsn = get64(header, 24);
        record.transaction_id = get64(header, 32);
        record.page_id = get64(header, 40);
        record.page_generation = get32(header, 48);
        record.previous_lsn = get64(header, 56);
        record.payload.assign(all.begin() + offset + record_header_size,
                              all.begin() + offset + record_header_size + payload_size);
        records.push_back(std::move(record));
        offset += record_header_size + static_cast<std::size_t>(payload_size);
    }
    return records;
}

Result<void> NativeWAL::sync() {
    StorageManager manager;
    StorageOpenOptions options;
    options.access = StorageAccess::ReadWrite;
    auto file = manager.open_file(path_, options);
    if (!file) return file.error();
    auto synced = file.value()->sync();
    return synced ? manager.close_all() : synced.error();
}

Result<void> NativeWAL::checkpoint(std::uint64_t transaction_id) {
    WALRecord begin{WALRecordType::CheckpointBegin, transaction_id, 0, last_lsn_, 0, 0, {}};
    auto appended = append(begin);
    if (!appended) return appended.error();
    WALRecord end{WALRecordType::CheckpointEnd, transaction_id, 0, begin.lsn, 0, 0, {}};
    return append(end);
}

Result<std::uint64_t> NativeWAL::scan_last_lsn() const {
    StorageManager manager;
    StorageOpenOptions options;
    options.access = StorageAccess::ReadWrite;
    auto file = manager.open_file(path_, options);
    if (!file) return file.error();
    auto size = file.value()->size();
    if (!size) return size.error();
    if (size.value() < header_size) return wal_error("Truncated native WAL.");
    std::vector<std::byte> buffer(static_cast<std::size_t>(size.value()));
    auto read = file.value()->read_at(0, buffer);
    if (!read || read.value() != buffer.size()) return wal_error("Unable to read native WAL.");
    std::uint64_t lsn = 0;
    std::size_t offset = header_size;
    while (offset + record_header_size <= buffer.size()) {
        const auto payload_size = get64(buffer, offset + 16);
        lsn = std::max(lsn, get64(buffer, offset + 24));
        if (payload_size > buffer.size() - offset - record_header_size) return wal_error("Invalid WAL payload length.");
        offset += record_header_size + static_cast<std::size_t>(payload_size);
    }
    auto closed = manager.close_all();
    return closed ? Result<std::uint64_t>(lsn) : Result<std::uint64_t>(closed.error());
}

} // namespace storq
