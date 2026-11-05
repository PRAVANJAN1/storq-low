#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

namespace storq {

using Metadata = std::unordered_map<std::string, std::string>;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
using String = std::string;

} // namespace storq
