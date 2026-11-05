#include <storq/core/error.hpp>

#include <utility>

namespace storq {

StorqException::StorqException(StorqError error)
    : std::runtime_error(error.message), error_(std::move(error)) {}

const StorqError& StorqException::error() const noexcept {
    return error_;
}

std::string_view error_category_name(ErrorCategory category) noexcept {
    switch (category) {
    case ErrorCategory::General: return "General";
    case ErrorCategory::Configuration: return "Configuration";
    case ErrorCategory::Runtime: return "Runtime";
    case ErrorCategory::Module: return "Module";
    case ErrorCategory::Filesystem: return "Filesystem";
    case ErrorCategory::Storage: return "Storage";
    case ErrorCategory::Parsing: return "Parsing";
    case ErrorCategory::Integrity: return "Integrity";
    case ErrorCategory::FaultInjection: return "FaultInjection";
    case ErrorCategory::Recovery: return "Recovery";
    case ErrorCategory::Internal: return "Internal";
    }
    return "Unknown";
}

std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown: return "Unknown";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::InvalidState: return "InvalidState";
    case ErrorCode::DuplicateRegistration: return "DuplicateRegistration";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AlreadyInitialized: return "AlreadyInitialized";
    case ErrorCode::InitializationFailed: return "InitializationFailed";
    case ErrorCode::ShutdownFailed: return "ShutdownFailed";
    case ErrorCode::IoError: return "IoError";
    }
    return "Unknown";
}

} // namespace storq
