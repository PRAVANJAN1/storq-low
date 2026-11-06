#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <storq/common/result.hpp>

namespace storq {

enum class IndexKeyType : std::uint8_t {
    Null = 0,
    Boolean = 1,
    SignedInteger = 2,
    UnsignedInteger = 3,
    FloatingPoint = 4,
    String = 5,
    Binary = 6,
    UUID = 7,
    Date = 8,
    Timestamp = 9
};

struct IndexKey {
    IndexKeyType type{IndexKeyType::Null};
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double,
                 std::string, std::vector<std::byte>> value;

    IndexKey() = default;
    explicit IndexKey(bool value);
    explicit IndexKey(std::int64_t value);
    explicit IndexKey(std::uint64_t value);
    explicit IndexKey(double value);
    explicit IndexKey(std::string value);
    explicit IndexKey(std::vector<std::byte> value);

    [[nodiscard]] Result<std::vector<std::byte>> serialize() const;
    [[nodiscard]] static Result<IndexKey> deserialize(std::span<const std::byte> bytes);
    friend bool operator==(const IndexKey&, const IndexKey&);
    friend bool operator<(const IndexKey&, const IndexKey&);
};

} // namespace storq
