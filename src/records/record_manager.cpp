#include <storq/records/record_manager.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_set>

#include <storq/core/error.hpp>
#include <storq/format/storq_format.hpp>
#include <storq/page/data_page.hpp>
#include <storq/storage/storage.hpp>

namespace storq {
namespace {

StorqError record_error(ErrorCode code, std::string message) {
    return {ErrorCategory::Storage, code, std::move(message), {}};
}

void put_u16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
}
void put_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i)
        bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
}
void put_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i)
        bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
}
std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i)
        value |= std::uint32_t{std::to_integer<unsigned>(bytes[offset + i])} << (8 * i);
    return value;
}
std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i)
        value |= std::uint64_t{std::to_integer<unsigned>(bytes[offset + i])} << (8 * i);
    return value;
}

} // namespace

RecordManager::RecordManager(std::filesystem::path path) : path_(std::move(path)) {}

Result<void> RecordManager::open() {
    file_ = std::make_shared<StorageFile>();
    StorageOpenOptions options;
    options.access = StorageAccess::ReadWrite;
    auto opened = file_->open(path_, options);
    if (!opened) return opened.error();
    std::array<std::byte, storq_header_size> header_bytes{};
    auto read = file_->read_at(0, header_bytes);
    if (!read || read.value() != header_bytes.size()) return record_error(ErrorCode::IoError, "Unable to read Storq header.");
    auto header = StorqFormatHeader::deserialize(header_bytes);
    if (!header) return header.error();
    page_size_ = header.value().page_size;
    return {};
}

Result<void> RecordManager::close() {
    if (!file_) return {};
    auto sync = file_->sync();
    if (!sync) return sync.error();
    return file_->close();
}

Result<std::vector<std::byte>> RecordManager::serialize(const Record& record) {
    std::vector<std::byte> output{std::byte{1}};
    output.resize(5);
    put_u32(output, 1, static_cast<std::uint32_t>(record.size()));
    for (const auto& [name, record_value] : record) {
        output.resize(output.size() + 4);
        const auto name_length_offset = output.size() - 4;
        put_u32(output, name_length_offset, static_cast<std::uint32_t>(name.size()));
        for (char c : name) output.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        std::uint8_t type = 0;
        std::vector<std::byte> value;
        const auto& variant = record_value.value;
        if (std::holds_alternative<std::monostate>(variant)) type = 0;
        else if (std::holds_alternative<bool>(variant)) {
            type = 1;
            value.push_back(std::get<bool>(variant) ? std::byte{1} : std::byte{0});
        } else if (std::holds_alternative<std::int64_t>(variant)) {
            type = 2;
            value.resize(8);
            auto number = static_cast<std::uint64_t>(std::get<std::int64_t>(variant));
            for (unsigned i = 0; i != 8; ++i) value[i] = static_cast<std::byte>((number >> (8 * i)) & 0xff);
        } else if (std::holds_alternative<double>(variant)) {
            type = 3;
            value.resize(8);
            std::uint64_t bits;
            static_assert(sizeof(bits) == sizeof(double));
            std::memcpy(&bits, &std::get<double>(variant), sizeof(bits));
            for (unsigned i = 0; i != 8; ++i) value[i] = static_cast<std::byte>((bits >> (8 * i)) & 0xff);
        } else if (std::holds_alternative<std::string>(variant)) {
            type = 4;
            const auto& text = std::get<std::string>(variant);
            value.reserve(text.size());
            for (char c : text) value.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        } else {
            type = 5;
            value = std::get<std::vector<std::byte>>(variant);
        }
        output.push_back(static_cast<std::byte>(type));
        output.resize(output.size() + 4);
        const auto value_length_offset = output.size() - 4;
        put_u32(output, value_length_offset, static_cast<std::uint32_t>(value.size()));
        output.insert(output.end(), value.begin(), value.end());
    }
    return output;
}

Result<Record> RecordManager::deserialize(std::span<const std::byte> bytes) {
    if (bytes.size() < 5 || std::to_integer<unsigned>(bytes[0]) != 1)
        return record_error(ErrorCode::InvalidArgument, "Invalid or unsupported record encoding.");
    Record record;
    std::size_t offset = 5;
    const auto count = [&] {
        std::uint32_t result = 0;
        for (unsigned i = 0; i != 4; ++i)
            result |= std::uint32_t{std::to_integer<unsigned>(bytes[1 + i])} << (8 * i);
        return result;
    }();
    for (std::uint32_t i = 0; i != count; ++i) {
        if (offset + 5 > bytes.size()) return record_error(ErrorCode::InvalidArgument, "Truncated record field.");
        std::uint32_t name_length = 0;
        for (unsigned n = 0; n != 4; ++n)
            name_length |= std::uint32_t{std::to_integer<unsigned>(bytes[offset + n])} << (8 * n);
        offset += 4;
        if (name_length > bytes.size() - offset) return record_error(ErrorCode::InvalidArgument, "Invalid record field name length.");
        std::string name(reinterpret_cast<const char*>(bytes.data() + offset), name_length);
        offset += name_length;
        if (offset >= bytes.size()) return record_error(ErrorCode::InvalidArgument, "Missing record value type.");
        const auto type = std::to_integer<unsigned>(bytes[offset++]);
        if (offset + 4 > bytes.size()) return record_error(ErrorCode::InvalidArgument, "Missing record value length.");
        std::uint32_t value_length = 0;
        for (unsigned n = 0; n != 4; ++n)
            value_length |= std::uint32_t{std::to_integer<unsigned>(bytes[offset + n])} << (8 * n);
        offset += 4;
        if (value_length > bytes.size() - offset) return record_error(ErrorCode::InvalidArgument, "Invalid record value length.");
        RecordValue value;
        if (type == 0) value = RecordValue{};
        else if (type == 1 && value_length == 1) value = RecordValue(std::to_integer<unsigned>(bytes[offset]) != 0);
        else if (type == 2 && value_length == 8) {
            std::uint64_t raw = 0;
            for (unsigned n = 0; n != 8; ++n) raw |= std::uint64_t{std::to_integer<unsigned>(bytes[offset + n])} << (8 * n);
            value = RecordValue(static_cast<std::int64_t>(raw));
        } else if (type == 3 && value_length == 8) {
            std::uint64_t raw = 0;
            for (unsigned n = 0; n != 8; ++n) raw |= std::uint64_t{std::to_integer<unsigned>(bytes[offset + n])} << (8 * n);
            double number; std::memcpy(&number, &raw, sizeof(number));
            value = RecordValue(number);
        } else if (type == 4) value = RecordValue(std::string(reinterpret_cast<const char*>(bytes.data() + offset), value_length));
        else if (type == 5) value = RecordValue(std::vector<std::byte>(bytes.begin() + offset, bytes.begin() + offset + value_length));
        else return record_error(ErrorCode::InvalidArgument, "Invalid record field encoding.");
        record.emplace(std::move(name), std::move(value));
        offset += value_length;
    }
    return record;
}

Result<RecordID> RecordManager::parse_record_id(const std::string& value) {
    auto id = RecordID::from_string(value);
    return id.is_valid() ? Result<RecordID>(id) : record_error(ErrorCode::InvalidArgument, "Invalid record identifier.");
}

Result<void> RecordManager::update_page_count(std::uint64_t count) {
    std::array<std::byte, storq_header_size> bytes{};
    auto read = file_->read_at(0, bytes);
    if (!read || read.value() != bytes.size()) return record_error(ErrorCode::IoError, "Unable to read Storq header.");
    auto header = StorqFormatHeader::deserialize(bytes);
    if (!header) return header.error();
    header.value().page_count = count;
    auto serialized = StorqFormatHeader::serialize(header.value());
    if (!serialized) return serialized.error();
    auto written = file_->write_at(0, serialized.value());
    if (!written || written.value() != serialized.value().size()) return record_error(ErrorCode::IoError, "Unable to persist Storq page count.");
    return {};
}

Result<std::uint64_t> RecordManager::allocate_page() {
    std::array<std::byte, storq_header_size> bytes{};
    auto read = file_->read_at(0, bytes);
    if (!read || read.value() != bytes.size()) return record_error(ErrorCode::IoError, "Unable to read Storq header.");
    auto header = StorqFormatHeader::deserialize(bytes);
    if (!header) return header.error();
    const auto page_id = header.value().page_count;
    auto size = file_->size();
    if (!size) return size.error();
    std::vector<std::byte> empty(page_size_, std::byte{0});
    DataPage page(page_size_);
    auto initialized = page.initialize(page_id);
    if (!initialized) return initialized.error();
    auto serialized = page.serialize(page_size_);
    if (!serialized) return serialized.error();
    auto written = file_->write_at(storq_header_size + (page_id * page_size_), serialized.value());
    if (!written || written.value() != serialized.value().size())
        return record_error(ErrorCode::IoError, "Unable to persist allocated data page.");
    auto count = update_page_count(page_id + 1);
    if (!count) return count.error();
    return page_id;
}

Result<RecordID> RecordManager::insert(const Record& record) {
    if (!file_) return record_error(ErrorCode::InvalidArgument, "Record manager is not open.");
    auto encoded = serialize(record);
    if (!encoded) return encoded.error();
    std::array<std::byte, storq_header_size> header_bytes{};
    auto read_header = file_->read_at(0, header_bytes);
    if (!read_header || read_header.value() != header_bytes.size())
        return record_error(ErrorCode::IoError, "Unable to read Storq header.");
    auto header_result = StorqFormatHeader::deserialize(header_bytes);
    if (!header_result) return header_result.error();
    const auto page_count = header_result.value().page_count;

    for (std::uint64_t page_id = 2; page_id < page_count; ++page_id) {
        std::vector<std::byte> page_bytes(page_size_);
        auto read = file_->read_at(storq_header_size + page_id * page_size_, page_bytes);
        if (!read || read.value() != page_bytes.size()) return record_error(ErrorCode::IoError, "Unable to read data page.");
        DataPage page(page_size_);
        auto deserialized = page.deserialize(page_size_, page_bytes);
        if (!deserialized) return deserialized.error();
        if (page.has_room(encoded.value().size() + DataPage::slot_size)) {
            std::uint32_t slot = 0;
            const auto generation = 1u + static_cast<std::uint32_t>(page.slots().size());
            auto inserted = page.insert(static_cast<std::uint32_t>(page.slots().size()), generation, encoded.value(), slot);
            if (!inserted) return inserted.error();
            auto serialized = page.serialize(page_size_);
            if (!serialized) return serialized.error();
            auto written = file_->write_at(storq_header_size + page_id * page_size_, serialized.value());
            if (!written || written.value() != serialized.value().size())
                return record_error(ErrorCode::IoError, "Unable to persist data page.");
            return RecordID{page_id, slot, generation};
        }
    }
    auto allocated = allocate_page();
    if (!allocated) return allocated.error();
    const auto page_id = allocated.value();
    std::vector<std::byte> page_bytes(page_size_);
    auto read = file_->read_at(storq_header_size + page_id * page_size_, page_bytes);
    if (!read || read.value() != page_bytes.size()) return record_error(ErrorCode::IoError, "Unable to read allocated page.");
    DataPage page(page_size_);
    auto deserialized = page.deserialize(page_size_, page_bytes);
    if (!deserialized) return deserialized.error();
    std::uint32_t slot = 0;
    auto inserted = page.insert(0, 1, encoded.value(), slot);
    if (!inserted) return inserted.error();
    auto serialized = page.serialize(page_size_);
    if (!serialized) return serialized.error();
    auto written = file_->write_at(storq_header_size + page_id * page_size_, serialized.value());
    if (!written || written.value() != serialized.value().size())
        return record_error(ErrorCode::IoError, "Unable to persist data page.");
    return RecordID{page_id, slot, 1};
}

Result<bool> RecordManager::exists(const RecordID& id) {
    if (!id.is_valid()) return false;
    std::vector<std::byte> bytes(page_size_);
    auto read = file_->read_at(storq_header_size + id.page_id * page_size_, bytes);
    if (!read || read.value() != bytes.size()) return false;
    DataPage page(page_size_);
    if (!page.deserialize(page_size_, bytes)) return false;
    auto value = page.read(id);
    return static_cast<bool>(value);
}

Result<Record> RecordManager::read(const RecordID& id) {
    if (!id.is_valid()) return record_error(ErrorCode::InvalidArgument, "Invalid record identifier.");
    std::vector<std::byte> bytes(page_size_);
    auto read = file_->read_at(storq_header_size + id.page_id * page_size_, bytes);
    if (!read || read.value() != bytes.size()) return record_error(ErrorCode::NotFound, "Record page was not found.");
    DataPage page(page_size_);
    auto deserialized = page.deserialize(page_size_, bytes);
    if (!deserialized) return deserialized.error();
    auto payload = page.read(id);
    if (!payload) return payload.error();
    return deserialize(payload.value());
}

Result<void> RecordManager::update(const RecordID& id, const Record& record) {
    auto encoded = serialize(record);
    if (!encoded) return encoded.error();
    std::vector<std::byte> bytes(page_size_);
    auto read = file_->read_at(storq_header_size + id.page_id * page_size_, bytes);
    if (!read || read.value() != bytes.size()) return record_error(ErrorCode::NotFound, "Record page was not found.");
    DataPage page(page_size_);
    auto deserialized = page.deserialize(page_size_, bytes);
    if (!deserialized) return deserialized.error();
    auto updated = page.update(id, encoded.value());
    if (!updated) return updated.error();
    auto serialized = page.serialize(page_size_);
    if (!serialized) return serialized.error();
    auto written = file_->write_at(storq_header_size + id.page_id * page_size_, serialized.value());
    if (!written || written.value() != serialized.value().size()) return record_error(ErrorCode::IoError, "Unable to persist record update.");
    return {};
}

Result<void> RecordManager::erase(const RecordID& id) {
    std::vector<std::byte> bytes(page_size_);
    auto read = file_->read_at(storq_header_size + id.page_id * page_size_, bytes);
    if (!read || read.value() != bytes.size()) return record_error(ErrorCode::NotFound, "Record page was not found.");
    DataPage page(page_size_);
    auto deserialized = page.deserialize(page_size_, bytes);
    if (!deserialized) return deserialized.error();
    auto erased = page.erase(id);
    if (!erased) return erased.error();
    auto serialized = page.serialize(page_size_);
    if (!serialized) return serialized.error();
    auto written = file_->write_at(storq_header_size + id.page_id * page_size_, serialized.value());
    if (!written || written.value() != serialized.value().size()) return record_error(ErrorCode::IoError, "Unable to persist record deletion.");
    return {};
}

} // namespace storq
