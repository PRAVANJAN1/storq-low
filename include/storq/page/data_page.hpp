#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <storq/common/result.hpp>
#include <storq/records/record_id.hpp>

namespace storq {

struct DataSlot {
    std::uint32_t id{0};
    std::uint32_t offset{0};
    std::uint32_t length{0};
    std::uint32_t generation{1};
    std::uint16_t flags{0};
};

class DataPage {
public:
    static constexpr std::uint32_t format_version = 1;
    static constexpr std::uint32_t data_header_size = 32;
    static constexpr std::uint32_t slot_size = 16;

    DataPage() = default;
    explicit DataPage(std::uint32_t page_size);

    [[nodiscard]] Result<void> initialize(std::uint64_t page_id, std::uint32_t generation = 1);
    [[nodiscard]] Result<void> deserialize(std::uint32_t page_size, const std::vector<std::byte>& bytes);
    [[nodiscard]] Result<std::vector<std::byte>> serialize(std::uint32_t page_size) const;
    [[nodiscard]] Result<void> validate() const;
    [[nodiscard]] Result<void> insert(std::uint32_t slot_id, std::uint32_t generation,
                                      std::span<const std::byte> payload, std::uint32_t& allocated_slot);
    [[nodiscard]] Result<void> erase(const RecordID& id);
    [[nodiscard]] Result<void> update(const RecordID& id, std::span<const std::byte> payload);
    [[nodiscard]] Result<std::vector<std::byte>> read(const RecordID& id) const;
    [[nodiscard]] Result<void> compact();
    [[nodiscard]] bool has_room(std::size_t bytes) const noexcept;

    std::uint64_t page_id() const noexcept { return page_id_; }
    std::uint32_t generation() const noexcept { return generation_; }
    std::uint32_t fragmented_bytes() const noexcept { return fragmented_bytes_; }
    const std::vector<DataSlot>& slots() const noexcept { return slots_; }

private:
    void recalculate_free_space() noexcept;
    std::uint32_t page_size_{4096};
    std::uint64_t page_id_{0};
    std::uint32_t generation_{1};
    std::uint32_t payload_start_{data_header_size};
    std::uint32_t payload_end_{0};
    std::uint32_t free_bytes_{0};
    std::uint32_t fragmented_bytes_{0};
    std::vector<DataSlot> slots_;
    std::vector<std::byte> payload_bytes_;
};

} // namespace storq
