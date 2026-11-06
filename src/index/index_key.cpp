#include <storq/index/index_key.hpp>

#include <cmath>
#include <cstring>
#include <limits>

namespace storq {
namespace {
StorqError key_error(std::string message) {
    return {ErrorCategory::Parsing, ErrorCode::InvalidArgument, std::move(message), {}};
}
void put_u16(std::vector<std::byte>& b, std::size_t o, std::uint16_t v) {
    for (unsigned i = 0; i != 2; ++i) b[o + i] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
}
void put_u32(std::vector<std::byte>& b, std::size_t o, std::uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) b[o + i] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
}
void put_u64(std::vector<std::byte>& b, std::size_t o, std::uint64_t v) {
    for (unsigned i = 0; i != 8; ++i) b[o + i] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
}
std::uint64_t get_u64(std::span<const std::byte> b, std::size_t o) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i != 8; ++i)
        v |= std::uint64_t{std::to_integer<unsigned>(b[o + i])} << (8 * i);
    return v;
}
std::uint32_t get_u32(std::span<const std::byte> b, std::size_t o) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i != 4; ++i) v |= std::uint32_t{std::to_integer<unsigned>(b[o + i])} << (8 * i);
    return v;
}
} // namespace

IndexKey::IndexKey(bool v) : type(IndexKeyType::Boolean), value(v) {}
IndexKey::IndexKey(std::int64_t v) : type(IndexKeyType::SignedInteger), value(v) {}
IndexKey::IndexKey(std::uint64_t v) : type(IndexKeyType::UnsignedInteger), value(v) {}
IndexKey::IndexKey(double v) : type(IndexKeyType::FloatingPoint), value(v) {}
IndexKey::IndexKey(std::string v) : type(IndexKeyType::String), value(std::move(v)) {}
IndexKey::IndexKey(std::vector<std::byte> v) : type(IndexKeyType::Binary), value(std::move(v)) {}

Result<std::vector<std::byte>> IndexKey::serialize() const {
    std::vector<std::byte> out(4);
    out[0] = static_cast<std::byte>(type);
    if (type == IndexKeyType::Null) out.resize(1);
    if (type == IndexKeyType::Boolean) {
        out.resize(2);
        out[1] = std::get<bool>(value) ? std::byte{1} : std::byte{0};
    }
    else if (type == IndexKeyType::SignedInteger) {
        out.resize(12);
        put_u64(out, 4, static_cast<std::uint64_t>(std::get<std::int64_t>(value)));
    } else if (type == IndexKeyType::UnsignedInteger) {
        out.resize(12);
        put_u64(out, 4, std::get<std::uint64_t>(value));
    } else if (type == IndexKeyType::FloatingPoint) {
        double number = std::get<double>(value);
        std::uint64_t bits;
        if (std::isnan(number)) bits = 0x7ff8000000000000ULL;
        else {
            if (number == 0.0) number = std::signbit(number) ? -0.0 : 0.0;
            std::memcpy(&bits, &number, sizeof(bits));
        }
        out.resize(12); put_u64(out, 4, bits);
    } else if (type == IndexKeyType::String) {
        const auto& text = std::get<std::string>(value);
        out.resize(8);
        put_u32(out, 4, static_cast<std::uint32_t>(text.size()));
        for (char c : text) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    } else if (type == IndexKeyType::Binary) {
        const auto& bytes = std::get<std::vector<std::byte>>(value);
        out.resize(8); put_u32(out, 4, static_cast<std::uint32_t>(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

Result<IndexKey> IndexKey::deserialize(std::span<const std::byte> bytes) {
    if (bytes.size() < 1) return key_error("Truncated index key.");
    const auto type = static_cast<IndexKeyType>(std::to_integer<unsigned>(bytes[0]));
    if (type == IndexKeyType::Null) return IndexKey{};
    if (type == IndexKeyType::Boolean && bytes.size() == 2)
        return IndexKey(std::to_integer<unsigned>(bytes[1]) != 0);
    if ((type == IndexKeyType::SignedInteger || type == IndexKeyType::UnsignedInteger ||
         type == IndexKeyType::FloatingPoint) && bytes.size() >= 12) {
        const auto raw = get_u64(bytes, 4);
        if (type == IndexKeyType::SignedInteger) return IndexKey(static_cast<std::int64_t>(raw));
        if (type == IndexKeyType::UnsignedInteger) return IndexKey(raw);
        double number; std::memcpy(&number, &raw, sizeof(number));
        return IndexKey(number);
    }
    if ((type == IndexKeyType::String || type == IndexKeyType::Binary) && bytes.size() >= 8) {
        const auto length = get_u32(bytes, 4);
        if (bytes.size() != 8 + length) return key_error("Invalid variable-length index key.");
        if (type == IndexKeyType::String)
            return IndexKey(std::string(reinterpret_cast<const char*>(bytes.data() + 8), length));
        return IndexKey(std::vector<std::byte>(bytes.begin() + 8, bytes.end()));
    }
    return key_error("Unsupported or malformed index key.");
}

bool operator==(const IndexKey& a, const IndexKey& b) {
    if (a.type != b.type) return false;
    if (a.type == IndexKeyType::FloatingPoint) {
        const auto x = std::get<double>(a.value), y = std::get<double>(b.value);
        if (std::isnan(x) || std::isnan(y)) return std::isnan(x) && std::isnan(y);
        return x == y && std::signbit(x) == std::signbit(y);
    }
    return a.value == b.value;
}

bool operator<(const IndexKey& a, const IndexKey& b) {
    if (a.type != b.type) return static_cast<std::uint8_t>(a.type) < static_cast<std::uint8_t>(b.type);
    if (a.type == IndexKeyType::FloatingPoint) {
        const auto x = std::get<double>(a.value), y = std::get<double>(b.value);
        if (std::isnan(x) != std::isnan(y)) return std::isnan(x);
        if (std::isnan(x)) return false;
        if (x == 0.0 && y == 0.0) return std::signbit(x) && !std::signbit(y);
        return x < y;
    }
    return a.value < b.value;
}

} // namespace storq
