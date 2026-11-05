#include <storq/core/version.hpp>

#include <storq/common/constants.hpp>

#ifndef STORQ_BUILD_TYPE
#define STORQ_BUILD_TYPE "Unknown"
#endif

#define STORQ_DETAIL_STRINGIFY_IMPL(value) #value
#define STORQ_DETAIL_STRINGIFY(value) STORQ_DETAIL_STRINGIFY_IMPL(value)

#if defined(_MSC_VER)
#define STORQ_COMPILER_INFO "MSVC " STORQ_DETAIL_STRINGIFY(_MSC_VER)
#elif defined(__clang__)
#define STORQ_COMPILER_INFO "Clang " __clang_version__
#elif defined(__GNUC__)
#define STORQ_COMPILER_INFO "GCC " __VERSION__
#else
#define STORQ_COMPILER_INFO "Unknown compiler"
#endif

namespace storq {

const VersionInfo& version() noexcept {
    static constexpr VersionInfo info{
        kVersionMajor,
        kVersionMinor,
        kVersionPatch,
        "0.1.0",
        STORQ_BUILD_TYPE,
        STORQ_COMPILER_INFO
    };
    return info;
}

} // namespace storq
