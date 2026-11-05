#pragma once

#include <stdexcept>
#include <string>

#include <storq/common/types.hpp>

namespace storq {

enum class ErrorCategory {
    General,
    Configuration,
    Runtime,
    Module,
    Filesystem,
    Storage,
    Parsing,
    Integrity,
    FaultInjection,
    Recovery,
    Internal
};

enum class ErrorCode {
    Unknown,
    InvalidArgument,
    InvalidState,
    DuplicateRegistration,
    NotFound,
    AlreadyInitialized,
    InitializationFailed,
    ShutdownFailed,
    IoError
};

struct StorqError {
    ErrorCategory category{ErrorCategory::General};
    ErrorCode code{ErrorCode::Unknown};
    std::string message;
    Metadata metadata;
};

class StorqException final : public std::runtime_error {
public:
    explicit StorqException(StorqError error);

    [[nodiscard]] const StorqError& error() const noexcept;

private:
    StorqError error_;
};

[[nodiscard]] std::string_view error_category_name(ErrorCategory category) noexcept;
[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

} // namespace storq
