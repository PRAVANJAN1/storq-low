#include <storq/records/record_id.hpp>

#include <cstdlib>

namespace storq {

bool RecordID::is_valid() const noexcept {
    return page_id != 0 && slot_id != UINT32_MAX && generation != 0;
}

std::string RecordID::to_string() const {
    return std::to_string(page_id) + ":" + std::to_string(slot_id) + ":" +
           std::to_string(generation);
}

RecordID RecordID::from_string(const std::string& value) {
    RecordID id;
    try {
        const auto first = value.find(':');
        const auto second = value.find(':', first == std::string::npos ? 0 : first + 1);
        if (first == std::string::npos || second == std::string::npos ||
            value.find(':', second + 1) != std::string::npos) return id;
        const auto page = std::stoull(value.substr(0, first));
        const auto slot = std::stoul(value.substr(first + 1, second - first - 1));
        const auto generation = std::stoul(value.substr(second + 1));
        id.page_id = page;
        id.slot_id = static_cast<std::uint32_t>(slot);
        id.generation = static_cast<std::uint32_t>(generation);
    } catch (...) {
        return {};
    }
    return id;
}

} // namespace storq
