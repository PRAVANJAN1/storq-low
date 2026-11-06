#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace storq {

struct RecordValue {
    using value_type = std::variant<std::monostate, bool, std::int64_t, double,
                                    std::string, std::vector<std::byte>>;
    value_type value;

    RecordValue() = default;
    RecordValue(bool v) : value(v) {}
    RecordValue(std::int64_t v) : value(v) {}
    RecordValue(std::int32_t v) : value(static_cast<std::int64_t>(v)) {}
    RecordValue(double v) : value(v) {}
    RecordValue(const char* v) : value(std::string(v)) {}
    RecordValue(std::string v) : value(std::move(v)) {}
    RecordValue(std::vector<std::byte> v) : value(std::move(v)) {}
};

using Record = std::map<std::string, RecordValue>;

} // namespace storq
