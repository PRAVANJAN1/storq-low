#include <storq/common/utilities.hpp>

#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

namespace storq {

std::string generate_uuid() {
    thread_local std::mt19937_64 generator([] {
        std::random_device device;
        std::seed_seq seed{
            device(), device(),
            static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())};
        return std::mt19937_64(seed);
    }());

    std::array<unsigned char, 16> bytes{};
    for (std::size_t index = 0; index < bytes.size(); index += 8) {
        const auto value = generator();
        for (std::size_t offset = 0; offset < 8; ++offset) {
            bytes[index + offset] = static_cast<unsigned char>(value >> (offset * 8));
        }
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        stream << std::setw(2) << static_cast<unsigned>(bytes[index]);
        if (index == 3 || index == 5 || index == 7 || index == 9) {
            stream << '-';
        }
    }
    return stream.str();
}

std::string format_timestamp(std::chrono::system_clock::time_point time) {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    const auto remainder = milliseconds - seconds;
    const auto time_value = std::chrono::system_clock::to_time_t(time);

    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &time_value);
#else
    localtime_r(&time_value, &local_time);
#endif

    std::ostringstream stream;
    stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << remainder.count();
    return stream.str();
}

std::string current_thread_id() {
    std::ostringstream stream;
    stream << std::this_thread::get_id();
    return stream.str();
}

std::string trim(std::string_view value) {
    constexpr std::string_view whitespace{" \t\n\r\f\v"};
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return std::string(value.substr(first, last - first + 1));
}

std::string to_lower(std::string_view value) {
    std::string result(value);
    for (auto& character : result) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return result;
}

} // namespace storq
