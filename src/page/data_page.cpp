#include <storq/page/data_page.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

#include <storq/core/error.hpp>
#include <storq/format/storq_format.hpp>

namespace storq {
namespace {

StorqError page_error(std::string message) {
    return {ErrorCategory::Storage, ErrorCode::InvalidArgument, std::move(message), {}};
}

void put_u16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xff);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
}
void put_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (unsigned i = 0; i != 4; ++i)
        bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
}
void put_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (unsigned i = 0; i != 8; ++i)
        bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
}
std::uint16_t get_u16(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset])) |
           (static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset + 1])) << 8);
}
std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i)
        value |= std::uint32_t{std::to_integer<unsigned>(bytes[offset + i])} << (8 * i);
    return value;
}
std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i)
        value |= std::uint64_t{std::to_integer<unsigned>(bytes[offset + i])} << (8 * i);
    return value;
}

} // namespace

DataPage::DataPage(std::uint32_t page_size) : page_size_(page_size), payload_end_(page_size) {}

Result<void> DataPage::initialize(std::uint64_t page_id, std::uint32_t generation) {
    if (page_id == 0 || page_size_ < storq_page_header_size + data_header_size)
        return page_error("Invalid data-page configuration.");
    page_id_ = page_id;
    generation_ = generation;
    payload_start_ = data_header_size;
    payload_end_ = page_size_ - storq_page_header_size;
    slots_.clear();
    payload_bytes_.assign(payload_end_ - data_header_size, std::byte{0});
    recalculate_free_space();
    return {};
}

Result<void> DataPage::deserialize(std::uint32_t page_size, const std::vector<std::byte>& bytes) {
    page_size_ = page_size;
    if (bytes.size() != page_size) return page_error("Data page has an invalid size.");
    auto header = StorqPage::deserialize_header(bytes);
    if (!header) return header.error();
    if (header.value().type != StorqPageType::Data || header.value().payload_length != bytes.size() - 32)
        return page_error("Page is not a valid Storq data page.");
    page_id_ = header.value().page_id;
    generation_ = header.value().generation;
    std::span<const std::byte> payload(bytes.data() + 32, bytes.size() - 32);
    if (payload.size() < data_header_size) return page_error("Data-page payload header is truncated.");
    if (get_u32(payload, 0) != 0x44574150 || get_u32(payload, 4) != format_version)
        return page_error("Invalid data-page payload header.");
    const auto slot_count = get_u32(payload, 8);
    payload_start_ = get_u32(payload, 12);
    payload_end_ = get_u32(payload, 16);
    if (slot_count > (payload.size() - data_header_size) / slot_size ||
        payload_start_ != data_header_size + slot_count * slot_size ||
        payload_end_ > payload.size()) return page_error("Corrupted data-page slot directory.");

    slots_.clear();
    slots_.reserve(slot_count);
    for (std::uint32_t i = 0; i != slot_count; ++i) {
        const auto offset = data_header_size + i * slot_size;
        DataSlot slot;
        slot.id = get_u32(payload, offset);
        slot.offset = get_u32(payload, offset + 4);
        slot.length = get_u32(payload, offset + 8);
        slot.generation = get_u32(payload, offset + 12);
        slot.flags = get_u16(payload, offset + 14);
        slots_.push_back(slot);
        if ((slot.flags & ~std::uint16_t{3}) != 0 ||
            (slot.flags == 0 && (slot.offset < payload_end_ || slot.offset + slot.length > payload.size())))
            return page_error("Corrupted data-page slot.");
    }
    payload_bytes_.assign(payload.size() - data_header_size, std::byte{0});
    for (std::size_t i = data_header_size; i < payload.size(); ++i)
        payload_bytes_[i - data_header_size] = payload[i];
    fragmented_bytes_ = get_u32(payload, 24);
    recalculate_free_space();
    return validate();
}

Result<std::vector<std::byte>> DataPage::serialize(std::uint32_t page_size) const {
    if (page_size_ != page_size || page_id_ == 0)
        return page_error("Data page cannot be serialized.");
    const auto payload_size = page_size - storq_page_header_size;
    std::vector<std::byte> payload(payload_size, std::byte{0});
    put_u32(payload, 0, 0x44574150);
    put_u32(payload, 4, format_version);
    put_u32(payload, 8, static_cast<std::uint32_t>(slots_.size()));
    put_u32(payload, 12, payload_start_);
    put_u32(payload, 16, payload_end_);
    put_u32(payload, 20, free_bytes_);
    put_u32(payload, 24, fragmented_bytes_);
    put_u32(payload, 28, generation_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto offset = data_header_size + i * slot_size;
        put_u32(payload, offset, slots_[i].id);
        put_u32(payload, offset + 4, slots_[i].offset);
        put_u32(payload, offset + 8, slots_[i].length);
        put_u32(payload, offset + 12, slots_[i].generation);
        put_u16(payload, offset + 14, slots_[i].flags);
    }
    const auto copy_begin = payload_end_ - data_header_size;
    const auto copy_end = payload_bytes_.size();
    if (copy_begin <= copy_end)
        std::copy(payload_bytes_.begin() + copy_begin, payload_bytes_.begin() + copy_end,
                  payload.begin() + payload_end_);
    StorqPageHeader header;
    header.type = StorqPageType::Data;
    header.page_id = page_id_;
    header.generation = generation_;
    header.payload_length = static_cast<std::uint32_t>(payload.size());
    auto page = StorqPage::serialize(page_size, header, payload);
    if (!page) return page.error();
    return page.value();
}

Result<void> DataPage::validate() const {
    if (page_id_ == 0 || payload_end_ > page_size_ - storq_page_header_size ||
        payload_start_ != data_header_size + slots_.size() * slot_size ||
        payload_start_ > payload_end_) return page_error("Invalid data page layout.");
    return {};
}

bool DataPage::has_room(std::size_t bytes) const noexcept {
    return bytes <= payload_end_ && bytes + payload_start_ + slot_size <= payload_end_;
}

Result<void> DataPage::insert(std::uint32_t slot_id, std::uint32_t generation,
                              std::span<const std::byte> payload,
                              std::uint32_t& allocated_slot) {
    if (!payload.empty() && !has_room(payload.size())) return page_error("Data page is full.");
    auto reusable = std::find_if(slots_.begin(), slots_.end(), [](const DataSlot& slot) {
        return slot.flags != 0;
    });
    if (reusable == slots_.end()) {
        allocated_slot = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({slot_id, payload_end_, static_cast<std::uint32_t>(payload.size()),
                          generation, 0});
        payload_start_ += slot_size;
    } else {
        allocated_slot = static_cast<std::uint32_t>(reusable - slots_.begin());
        *reusable = {slot_id, payload_end_, static_cast<std::uint32_t>(payload.size()),
                     generation, 0};
    }
    if (payload.size() > payload_end_ - payload_start_) return page_error("Record does not fit after slot allocation.");
    payload_end_ -= static_cast<std::uint32_t>(payload.size());
    slots_[allocated_slot].offset = payload_end_;
    const auto logical_offset = payload_end_ - data_header_size;
    std::copy(payload.begin(), payload.end(), payload_bytes_.begin() + logical_offset);
    recalculate_free_space();
    return {};
}

Result<void> DataPage::erase(const RecordID& id) {
    if (id.slot_id >= slots_.size() || slots_[id.slot_id].flags != 0 ||
        slots_[id.slot_id].generation != id.generation)
        return page_error("Record is deleted or stale.");
    slots_[id.slot_id].flags = 1;
    fragmented_bytes_ += slots_[id.slot_id].length;
    recalculate_free_space();
    return {};
}

Result<void> DataPage::update(const RecordID& id, std::span<const std::byte> payload) {
    if (id.slot_id >= slots_.size() || slots_[id.slot_id].flags != 0 ||
        slots_[id.slot_id].generation != id.generation)
        return page_error("Record is deleted or stale.");
    auto& slot = slots_[id.slot_id];
    if (payload.size() <= slot.length) {
        std::copy(payload.begin(), payload.end(), payload_bytes_.begin() + slot.offset - data_header_size);
        fragmented_bytes_ += slot.length - static_cast<std::uint32_t>(payload.size());
        slot.length = static_cast<std::uint32_t>(payload.size());
        recalculate_free_space();
        return {};
    }
    if (!has_room(payload.size() - slot.length)) return page_error("Updated record is too large for data page.");
    fragmented_bytes_ += slot.length;
    payload_end_ -= static_cast<std::uint32_t>(payload.size());
    slot.offset = payload_end_;
    slot.length = static_cast<std::uint32_t>(payload.size());
    std::copy(payload.begin(), payload.end(), payload_bytes_.begin() + payload_end_ - data_header_size);
    recalculate_free_space();
    return {};
}

Result<std::vector<std::byte>> DataPage::read(const RecordID& id) const {
    if (id.slot_id >= slots_.size()) return page_error("Record slot does not exist.");
    const auto& slot = slots_[id.slot_id];
    if (slot.flags != 0 || slot.generation != id.generation) return page_error("Record is deleted or stale.");
    if (slot.offset < data_header_size ||
        slot.offset > static_cast<std::uint32_t>(payload_bytes_.size() + data_header_size))
        return page_error("Record bounds are invalid.");
    const auto start = slot.offset - data_header_size;
    if (start > payload_bytes_.size() || slot.length > payload_bytes_.size() - start)
        return page_error("Record payload is truncated.");
    return std::vector<std::byte>(payload_bytes_.begin() + start,
                                  payload_bytes_.begin() + start + slot.length);
}

Result<void> DataPage::compact() {
    auto original_slots = slots_;
    std::uint32_t write_offset = static_cast<std::uint32_t>(payload_bytes_.size());
    std::vector<std::byte> compacted(payload_bytes_.size(), std::byte{0});
    for (auto& slot : original_slots) {
        if (slot.flags != 0) continue;
        if (slot.offset < data_header_size || slot.offset + slot.length > payload_end_)
            return page_error("Invalid record encountered during compaction.");
        const auto old_start = slot.offset - data_header_size;
        if (write_offset < slot.length) return page_error("Page compaction overflow.");
        write_offset -= slot.length;
        std::copy_n(payload_bytes_.begin() + old_start, slot.length, compacted.begin() + write_offset);
        slot.offset = data_header_size + write_offset;
    }
    payload_bytes_ = std::move(compacted);
    payload_end_ = page_size_ - storq_page_header_size;
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        if (original_slots[i].flags == 0)
            payload_end_ = std::min(payload_end_, original_slots[i].offset);
    }
    slots_ = std::move(original_slots);
    fragmented_bytes_ = 0;
    recalculate_free_space();
    return {};
}

void DataPage::recalculate_free_space() noexcept {
    const auto directory_end = data_header_size + static_cast<std::uint32_t>(slots_.size()) * slot_size;
    payload_start_ = std::min(directory_end, static_cast<std::uint32_t>(page_size_ - storq_page_header_size));
    free_bytes_ = payload_end_ > payload_start_ ? payload_end_ - payload_start_ : 0;
}

} // namespace storq
