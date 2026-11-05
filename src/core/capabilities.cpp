#include <storq/core/capabilities.hpp>

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace storq {

bool CapabilityRegistry::register_capability(std::string capability) {
    if (capability.empty()) {
        return false;
    }
    std::unique_lock lock(mutex_);
    if (std::find(capabilities_.begin(), capabilities_.end(), capability) != capabilities_.end()) {
        return false;
    }
    capabilities_.push_back(std::move(capability));
    return true;
}

bool CapabilityRegistry::contains(std::string_view capability) const {
    std::shared_lock lock(mutex_);
    return std::find(capabilities_.begin(), capabilities_.end(), capability) != capabilities_.end();
}

std::vector<std::string> CapabilityRegistry::list() const {
    std::shared_lock lock(mutex_);
    return capabilities_;
}

} // namespace storq
