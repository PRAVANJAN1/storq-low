#pragma once

#include <string_view>

namespace storq {

struct VersionInfo {
    unsigned major;
    unsigned minor;
    unsigned patch;
    std::string_view string;
    std::string_view build_type;
    std::string_view compiler;
};

[[nodiscard]] const VersionInfo& version() noexcept;

} // namespace storq
