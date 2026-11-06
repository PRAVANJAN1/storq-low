#pragma once

#include <cstdint>
#include <string>

namespace storq {

struct RecordID {
    std::uint64_t page_id{0};
    std::uint32_t slot_id{0};
    std::uint32_t generation{0};

    bool operator==(const RecordID&) const = default;
    [[nodiscard]] bool operator<(const RecordID& other) const noexcept {
        return page_id != other.page_id ? page_id < other.page_id :
               slot_id != other.slot_id ? slot_id < other.slot_id : generation < other.generation;
    }
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] static RecordID from_string(const std::string& value);
    [[nodiscard]] bool is_valid() const noexcept;
};

} // namespace storq
