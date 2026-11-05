#pragma once

#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace storq {

class CapabilityRegistry {
public:
    [[nodiscard]] bool register_capability(std::string capability);
    [[nodiscard]] bool contains(std::string_view capability) const;
    [[nodiscard]] std::vector<std::string> list() const;

private:
    mutable std::shared_mutex mutex_;
    std::vector<std::string> capabilities_;
};

} // namespace storq
