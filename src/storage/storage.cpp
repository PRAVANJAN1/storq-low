#include <storq/storage/storage.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <exception>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <storq/common/utilities.hpp>
#include <storq/core/runtime.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace storq {
namespace {

StorqError storage_error(ErrorCode code, std::string message) {
    return {ErrorCategory::Storage, code, std::move(message), {}};
}

StorqError internal_storage_error(std::string message) {
    return {ErrorCategory::Internal, ErrorCode::Unknown, std::move(message), {}};
}

StorqError cleanup_snapshot(
    const std::shared_ptr<StorageFile>& destination,
    const std::filesystem::path& path,
    StorqError failure) {
    auto closed = destination->close();
    if (!closed) {
        failure.metadata.emplace("cleanup_close_error", closed.error().message);
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
        failure.metadata.emplace("cleanup_remove_error", error.message());
    }
    return failure;
}

template <typename Function>
auto invoke_backend(Function&& function) -> decltype(function()) {
    using ReturnType = decltype(function());
    try {
        return function();
    } catch (const std::exception& exception) {
        return ReturnType(storage_error(
            ErrorCode::IoError,
            "Storage backend threw an exception: " + std::string(exception.what())));
    } catch (...) {
        return ReturnType(
            storage_error(ErrorCode::IoError, "Storage backend threw an unknown exception."));
    }
}

StorqError io_error(std::string operation, const std::filesystem::path& path,
                    std::string detail) {
    return storage_error(ErrorCode::IoError,
                         std::move(operation) + " '" + path.string() + "': " +
                             std::move(detail));
}

StorqError transfer_error(ErrorCode code, std::string message, std::size_t transferred) {
    auto error = storage_error(code, std::move(message));
    if (transferred != 0) {
        error.metadata.emplace("bytes_transferred", std::to_string(transferred));
    }
    return error;
}

std::uint64_t transferred_bytes(const StorqError& error) noexcept {
    const auto found = error.metadata.find("bytes_transferred");
    if (found == error.metadata.end()) {
        return 0;
    }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(
        found->second.data(), found->second.data() + found->second.size(), result);
    return parsed.ec == std::errc{} ? result : 0;
}

std::uint64_t bounded_transferred_bytes(const StorqError& error, std::size_t requested) noexcept {
    return std::min<std::uint64_t>(transferred_bytes(error), requested);
}

bool writable_access(StorageAccess access) {
    return access != StorageAccess::ReadOnly;
}

bool creates_file(StorageAccess access) {
    return access == StorageAccess::Create || access == StorageAccess::CreateNew;
}

Result<void> validate_open_options(const std::filesystem::path& path,
                                   const StorageOpenOptions& options) {
    if (path.empty()) {
        return storage_error(ErrorCode::InvalidArgument, "Storage path must not be empty.");
    }
    if (static_cast<int>(options.access) < static_cast<int>(StorageAccess::ReadOnly) ||
        static_cast<int>(options.access) > static_cast<int>(StorageAccess::Truncate)) {
        return storage_error(ErrorCode::InvalidArgument, "Storage access mode is invalid.");
    }
#if !defined(_WIN32)
    if ((options.permissions & ~static_cast<std::uint32_t>(0777)) != 0) {
        return storage_error(ErrorCode::InvalidArgument,
                             "POSIX file permissions must use only the 0777 permission bits.");
    }
#endif
    if (options.create_directories && !creates_file(options.access)) {
        return storage_error(
            ErrorCode::InvalidArgument,
            "Parent directories can only be created with Create or CreateNew access.");
    }
    return {};
}

std::string system_error_message(int error) {
    return std::error_code(error, std::system_category()).message();
}

std::chrono::system_clock::time_point to_system_time(
    std::filesystem::file_time_type file_time) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
}

} // namespace

class StorageOperationObserverRegistry {
public:
    std::uint64_t add(std::shared_ptr<IStorageObserver> observer) {
        if (!observer) {
            return 0;
        }
        std::lock_guard lock(mutex_);
        const auto id = next_id_++;
        observers_.emplace_back(id, std::move(observer));
        return id;
    }

    bool remove(std::uint64_t id) {
        std::lock_guard lock(mutex_);
        const auto observer = std::find_if(observers_.begin(), observers_.end(),
            [id](const auto& entry) { return entry.first == id; });
        if (observer == observers_.end()) {
            return false;
        }
        observers_.erase(observer);
        return true;
    }

    void notify_started(StorageOperation& operation) const noexcept {
        std::vector<std::shared_ptr<IStorageObserver>> observers;
        try {
            std::lock_guard lock(mutex_);
            if (observers_.empty()) {
                return;
            }
            operation.operation_id = generate_uuid();
            operation.started_at = std::chrono::system_clock::now();
            observers.reserve(observers_.size());
            for (const auto& entry : observers_) {
                observers.push_back(entry.second);
            }
            pending_.insert_or_assign(operation.operation_id, observers);
        } catch (...) {
            return;
        }
        for (const auto& observer : observers) {
            try {
                observer->on_operation_started(operation);
            } catch (...) {
            }
        }
    }

    void notify_completed(const StorageOperation& operation) const noexcept {
        if (operation.operation_id.empty()) {
            return;
        }
        std::vector<std::shared_ptr<IStorageObserver>> observers;
        try {
            std::lock_guard lock(mutex_);
            const auto pending = pending_.find(operation.operation_id);
            if (pending == pending_.end()) {
                return;
            }
            observers = std::move(pending->second);
            pending_.erase(pending);
        } catch (...) {
            return;
        }
        for (const auto& observer : observers) {
            try {
                observer->on_operation_completed(operation);
            } catch (...) {
            }
        }
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<IStorageObserver>>> observers_;
    mutable std::unordered_map<std::string, std::vector<std::shared_ptr<IStorageObserver>>> pending_;
    std::uint64_t next_id_{1};
};

NativeFileBackend::NativeFileBackend(std::filesystem::path path, StorageAccess access)
    : path_(std::move(path)), access_(access) {}

NativeFileBackend::~NativeFileBackend() {
    static_cast<void>(close());
}

Result<std::shared_ptr<NativeFileBackend>> NativeFileBackend::open(
    const std::filesystem::path& path, const StorageOpenOptions& options) {
    auto validation = validate_open_options(path, options);
    if (!validation) {
        return validation.error();
    }
    std::error_code filesystem_error;
    if (options.create_directories) {
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, filesystem_error);
            if (filesystem_error) {
                return io_error("Unable to create parent directories for", path,
                                filesystem_error.message());
            }
        }
    }

    auto backend = std::shared_ptr<NativeFileBackend>(
        new NativeFileBackend(path, options.access));
#if defined(_WIN32)
    DWORD desired_access = GENERIC_READ;
    DWORD creation = OPEN_EXISTING;
    if (writable_access(options.access)) {
        desired_access |= GENERIC_WRITE;
    }
    switch (options.access) {
    case StorageAccess::ReadOnly:
    case StorageAccess::ReadWrite:
        creation = OPEN_EXISTING;
        break;
    case StorageAccess::Create:
        creation = OPEN_ALWAYS;
        break;
    case StorageAccess::CreateNew:
        creation = CREATE_NEW;
        break;
    case StorageAccess::Truncate:
        creation = TRUNCATE_EXISTING;
        break;
    }
    const DWORD share_mode = options.exclusive_access
        ? 0
        : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE handle = CreateFileW(path.c_str(), desired_access, share_mode, nullptr, creation,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        const ErrorCode code = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
            ? ErrorCode::DuplicateRegistration
            : error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? ErrorCode::NotFound
                : ErrorCode::IoError;
        return storage_error(code, "Unable to open '" + path.string() + "': " +
                                       system_error_message(static_cast<int>(error)));
    }
    backend->handle_ = handle;
#else
    int flags = O_CLOEXEC;
    switch (options.access) {
    case StorageAccess::ReadOnly:
        flags |= O_RDONLY;
        break;
    case StorageAccess::ReadWrite:
        flags |= O_RDWR;
        break;
    case StorageAccess::Create:
        flags |= O_RDWR | O_CREAT;
        break;
    case StorageAccess::CreateNew:
        flags |= O_RDWR | O_CREAT | O_EXCL;
        break;
    case StorageAccess::Truncate:
        flags |= O_RDWR | O_TRUNC;
        break;
    }
    const int descriptor = ::open(path.c_str(), flags, static_cast<mode_t>(options.permissions));
    if (descriptor < 0) {
        const int error = errno;
        const ErrorCode code = error == EEXIST
            ? ErrorCode::DuplicateRegistration
            : error == ENOENT ? ErrorCode::NotFound : ErrorCode::IoError;
        return storage_error(code, "Unable to open '" + path.string() + "': " +
                                       system_error_message(error));
    }
    backend->descriptor_ = descriptor;
    if (options.exclusive_access && flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        static_cast<void>(::close(descriptor));
        backend->descriptor_ = -1;
        return storage_error(ErrorCode::InvalidState,
                             "Unable to acquire exclusive access to '" + path.string() +
                                 "': " + system_error_message(error));
    }
#endif
    backend->exclusive_locked_ = options.exclusive_access;
    backend->open_.store(true, std::memory_order_release);
    return backend;
}

Result<std::size_t> NativeFileBackend::read_at(
    std::uint64_t offset, std::span<std::byte> buffer) {
    if (buffer.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
        return storage_error(ErrorCode::InvalidArgument, "Read offset and size overflow.");
    }
#if defined(_WIN32)
    if (offset + buffer.size() >
        static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
#else
    if (offset + buffer.size() >
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
#endif
        return storage_error(ErrorCode::InvalidArgument, "Read range is out of range.");
    }
    std::unique_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot read a closed storage file.");
    }
    std::size_t total = 0;
    while (total < buffer.size()) {
#if defined(_WIN32)
        const auto position = offset + total;
        LARGE_INTEGER file_position{};
        file_position.QuadPart = static_cast<LONGLONG>(position);
        if (!SetFilePointerEx(static_cast<HANDLE>(handle_), file_position, nullptr, FILE_BEGIN)) {
            return transfer_error(ErrorCode::IoError,
                                  "Unable to position file for reading: " +
                                      system_error_message(static_cast<int>(GetLastError())),
                                  total);
        }
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            buffer.size() - total, std::numeric_limits<DWORD>::max()));
        DWORD received = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), buffer.data() + total, request,
                      &received, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) {
                break;
            }
            return transfer_error(ErrorCode::IoError,
                                  "Unable to read storage file: " +
                                      system_error_message(static_cast<int>(error)),
                                  total);
        }
        if (received == 0) {
            break;
        }
        total += received;
#else
        const auto request = std::min<std::size_t>(
            buffer.size() - total,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto received = ::pread(descriptor_, buffer.data() + total, request,
                                      static_cast<off_t>(offset + total));
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error = errno;
            return transfer_error(ErrorCode::IoError,
                                  "Unable to read '" + path_.string() + "': " +
                                      system_error_message(error),
                                  total);
        }
        if (received == 0) {
            break;
        }
        total += static_cast<std::size_t>(received);
#endif
    }
    return total;
}

Result<std::size_t> NativeFileBackend::write_at(
    std::uint64_t offset, std::span<const std::byte> buffer) {
    if (!writable_access(access_)) {
        return storage_error(ErrorCode::InvalidState,
                             "Cannot write to a read-only storage file.");
    }
    if (buffer.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
        return storage_error(ErrorCode::InvalidArgument, "Write offset and size overflow.");
    }
#if defined(_WIN32)
    if (offset + buffer.size() >
        static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
#else
    if (offset + buffer.size() >
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
#endif
        return storage_error(ErrorCode::InvalidArgument, "Write range is out of range.");
    }
    std::unique_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot write to a closed storage file.");
    }
    std::size_t total = 0;
    while (total < buffer.size()) {
#if defined(_WIN32)
        LARGE_INTEGER file_position{};
        file_position.QuadPart = static_cast<LONGLONG>(offset + total);
        if (!SetFilePointerEx(static_cast<HANDLE>(handle_), file_position, nullptr, FILE_BEGIN)) {
            return transfer_error(ErrorCode::IoError,
                                  "Unable to position file for writing: " +
                                      system_error_message(static_cast<int>(GetLastError())),
                                  total);
        }
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            buffer.size() - total, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(handle_), buffer.data() + total, request,
                       &written, nullptr)) {
            return transfer_error(ErrorCode::IoError,
                                  "Unable to write storage file: " +
                                      system_error_message(static_cast<int>(GetLastError())),
                                  total);
        }
        if (written == 0) {
            return transfer_error(ErrorCode::IoError, "Storage write made no progress.", total);
        }
        total += written;
#else
        const auto request = std::min<std::size_t>(
            buffer.size() - total,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto written = ::pwrite(descriptor_, buffer.data() + total, request,
                                      static_cast<off_t>(offset + total));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error = errno;
            return transfer_error(ErrorCode::IoError,
                                  "Unable to write '" + path_.string() + "': " +
                                      system_error_message(error),
                                  total);
        }
        if (written == 0) {
            return transfer_error(ErrorCode::IoError, "Storage write made no progress.", total);
        }
        total += static_cast<std::size_t>(written);
#endif
    }
    return total;
}

Result<std::size_t> NativeFileBackend::append(std::span<const std::byte> buffer) {
    if (!writable_access(access_)) {
        return storage_error(ErrorCode::InvalidState,
                             "Cannot append to a read-only storage file.");
    }
    std::unique_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot append to a closed storage file.");
    }
#if defined(_WIN32)
    LARGE_INTEGER zero{};
    LARGE_INTEGER current{};
    if (!SetFilePointerEx(static_cast<HANDLE>(handle_), zero, &current, FILE_END)) {
        return storage_error(ErrorCode::IoError,
                             "Unable to seek to end of file: " +
                                 system_error_message(static_cast<int>(GetLastError())));
    }
    const auto offset = static_cast<std::uint64_t>(current.QuadPart);
#else
    struct stat status {};
    if (fstat(descriptor_, &status) != 0) {
        return storage_error(ErrorCode::IoError,
                             "Unable to inspect '" + path_.string() + "': " +
                                 system_error_message(errno));
    }
    const auto offset = static_cast<std::uint64_t>(status.st_size);
#endif
    if (buffer.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
        return storage_error(ErrorCode::InvalidArgument, "Append range is out of range.");
    }
#if defined(_WIN32)
    if (offset + buffer.size() >
        static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
#else
    if (offset + buffer.size() >
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
#endif
        return storage_error(ErrorCode::InvalidArgument, "Append range is out of range.");
    }

    std::size_t total = 0;
    while (total < buffer.size()) {
#if defined(_WIN32)
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset + total);
        if (!SetFilePointerEx(static_cast<HANDLE>(handle_), position, nullptr, FILE_BEGIN)) {
            return storage_error(ErrorCode::IoError, "Unable to position file for append.");
        }
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            buffer.size() - total, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(handle_), buffer.data() + total, request,
                       &written, nullptr)) {
            return transfer_error(ErrorCode::IoError,
                                  "Unable to append to storage file: " +
                                      system_error_message(static_cast<int>(GetLastError())),
                                  total);
        }
        if (written == 0) {
            return transfer_error(ErrorCode::IoError, "Storage append made no progress.", total);
        }
        total += written;
#else
        const auto request = std::min<std::size_t>(
            buffer.size() - total,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto written = ::pwrite(descriptor_, buffer.data() + total, request,
                                      static_cast<off_t>(offset + total));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error = errno;
            return transfer_error(ErrorCode::IoError,
                                  "Unable to append to '" + path_.string() + "': " +
                                      system_error_message(error),
                                  total);
        }
        if (written == 0) {
            return transfer_error(ErrorCode::IoError, "Storage append made no progress.", total);
        }
        total += static_cast<std::size_t>(written);
#endif
    }
    return total;
}

Result<void> NativeFileBackend::truncate(std::uint64_t new_size) {
    if (!writable_access(access_)) {
        return storage_error(ErrorCode::InvalidState,
                             "Cannot truncate a read-only storage file.");
    }
#if defined(_WIN32)
    if (new_size > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
#else
    if (new_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
#endif
        return storage_error(ErrorCode::InvalidArgument, "Truncate size is out of range.");
    }
    std::unique_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot truncate a closed storage file.");
    }
#if defined(_WIN32)
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(new_size);
    if (!SetFilePointerEx(static_cast<HANDLE>(handle_), position, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(static_cast<HANDLE>(handle_))) {
        return storage_error(ErrorCode::IoError,
                             "Unable to truncate '" + path_.string() + "': " +
                                 system_error_message(static_cast<int>(GetLastError())));
    }
#else
    if (ftruncate(descriptor_, static_cast<off_t>(new_size)) != 0) {
        return storage_error(ErrorCode::IoError,
                             "Unable to truncate '" + path_.string() + "': " +
                                 system_error_message(errno));
    }
#endif
    return {};
}

Result<void> NativeFileBackend::sync() {
    std::unique_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot sync a closed storage file.");
    }
    if (!writable_access(access_)) {
        return {};
    }
#if defined(_WIN32)
    if (!FlushFileBuffers(static_cast<HANDLE>(handle_))) {
        return storage_error(ErrorCode::IoError,
                             "Unable to flush '" + path_.string() + "': " +
                                 system_error_message(static_cast<int>(GetLastError())));
    }
#else
    int result = 0;
    do {
        result = fsync(descriptor_);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        return storage_error(ErrorCode::IoError,
                             "Unable to synchronize '" + path_.string() + "': " +
                                 system_error_message(errno));
    }
#endif
    return {};
}

Result<std::uint64_t> NativeFileBackend::size() const {
    std::unique_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot inspect a closed storage file.");
    }
#if defined(_WIN32)
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &file_size)) {
        return storage_error(ErrorCode::IoError,
                             "Unable to retrieve file size: " +
                                 system_error_message(static_cast<int>(GetLastError())));
    }
    return static_cast<std::uint64_t>(file_size.QuadPart);
#else
    struct stat status {};
    if (fstat(descriptor_, &status) != 0) {
        return storage_error(ErrorCode::IoError,
                             "Unable to inspect '" + path_.string() + "': " +
                                 system_error_message(errno));
    }
    return static_cast<std::uint64_t>(status.st_size);
#endif
}

Result<StorageMetadata> NativeFileBackend::metadata() const {
    std::unique_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot inspect a closed storage file.");
    }
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path_, error);
    if (error) {
        return io_error("Unable to retrieve metadata for", path_, error.message());
    }
    const auto modified = std::filesystem::last_write_time(path_, error);
    if (error) {
        return io_error("Unable to retrieve modification time for", path_, error.message());
    }
    return StorageMetadata{path_, file_size, true, true, writable_access(access_),
                           to_system_time(modified), "native"};
}

Result<void> NativeFileBackend::close() {
    std::unique_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return {};
    }
#if defined(_WIN32)
    HANDLE handle = static_cast<HANDLE>(handle_);
    handle_ = nullptr;
    if (!CloseHandle(handle)) {
        open_.store(false, std::memory_order_release);
        return storage_error(ErrorCode::IoError,
                             "Unable to close '" + path_.string() + "': " +
                                 system_error_message(static_cast<int>(GetLastError())));
    }
#else
    const int descriptor = descriptor_;
    descriptor_ = -1;
    if (exclusive_locked_) {
        static_cast<void>(flock(descriptor, LOCK_UN));
        exclusive_locked_ = false;
    }
    if (::close(descriptor) != 0) {
        const int error = errno;
        open_.store(false, std::memory_order_release);
        return storage_error(ErrorCode::IoError,
                             "Unable to close '" + path_.string() + "': " +
                                 system_error_message(error));
    }
#endif
    open_.store(false, std::memory_order_release);
    return {};
}

bool NativeFileBackend::is_open() const noexcept {
    return open_.load(std::memory_order_acquire);
}

StorageAccess NativeFileBackend::access() const noexcept {
    return access_;
}

MemoryStorageBackend::MemoryStorageBackend(std::size_t capacity, StorageAccess access)
    : capacity_(capacity), access_(access) {}

Result<void> MemoryStorageBackend::validate_open_and_writable() const {
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Memory storage is closed.");
    }
    if (!writable_access(access_)) {
        return storage_error(ErrorCode::InvalidState,
                             "Cannot modify read-only memory storage.");
    }
    return {};
}

Result<std::size_t> MemoryStorageBackend::read_at(
    std::uint64_t offset, std::span<std::byte> buffer) {
    std::shared_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot read closed memory storage.");
    }
    if (offset >= data_.size() || buffer.empty()) {
        return std::size_t{0};
    }
    const auto available = data_.size() - static_cast<std::size_t>(offset);
    const auto count = std::min(available, buffer.size());
    std::copy_n(data_.data() + static_cast<std::size_t>(offset), count, buffer.data());
    return count;
}

Result<std::size_t> MemoryStorageBackend::write_at(
    std::uint64_t offset, std::span<const std::byte> buffer) {
    std::unique_lock lock(mutex_);
    auto validation = validate_open_and_writable();
    if (!validation) {
        return validation.error();
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    if (offset > std::numeric_limits<std::size_t>::max() ||
        buffer.size() > std::numeric_limits<std::size_t>::max() -
                            static_cast<std::size_t>(offset)) {
        return storage_error(ErrorCode::InvalidArgument, "Memory write range is out of bounds.");
    }
    const auto end = static_cast<std::size_t>(offset) + buffer.size();
    if (end > capacity_) {
        return storage_error(ErrorCode::IoError, "Memory storage capacity exceeded.");
    }
    try {
        if (end > data_.size()) {
            data_.resize(end, std::byte{0});
        }
    } catch (const std::exception& exception) {
        return storage_error(ErrorCode::IoError,
                             "Unable to grow memory storage: " +
                                 std::string(exception.what()));
    }
    std::copy(buffer.begin(), buffer.end(), data_.begin() + static_cast<std::size_t>(offset));
    return buffer.size();
}

Result<std::size_t> MemoryStorageBackend::append(std::span<const std::byte> buffer) {
    std::unique_lock lock(mutex_);
    auto validation = validate_open_and_writable();
    if (!validation) {
        return validation.error();
    }
    if (buffer.size() > capacity_ - data_.size()) {
        return storage_error(ErrorCode::IoError, "Memory storage capacity exceeded.");
    }
    try {
        data_.insert(data_.end(), buffer.begin(), buffer.end());
    } catch (const std::exception& exception) {
        return storage_error(ErrorCode::IoError,
                             "Unable to append to memory storage: " +
                                 std::string(exception.what()));
    }
    return buffer.size();
}

Result<void> MemoryStorageBackend::truncate(std::uint64_t new_size) {
    std::unique_lock lock(mutex_);
    auto validation = validate_open_and_writable();
    if (!validation) {
        return validation.error();
    }
    if (new_size > capacity_ || new_size > std::numeric_limits<std::size_t>::max()) {
        return storage_error(ErrorCode::IoError, "Memory storage capacity exceeded.");
    }
    try {
        data_.resize(static_cast<std::size_t>(new_size), std::byte{0});
    } catch (const std::exception& exception) {
        return storage_error(ErrorCode::IoError,
                             "Unable to resize memory storage: " +
                                 std::string(exception.what()));
    }
    return {};
}

Result<void> MemoryStorageBackend::sync() {
    std::shared_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot sync closed memory storage.");
    }
    return {};
}

Result<std::uint64_t> MemoryStorageBackend::size() const {
    std::shared_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot inspect closed memory storage.");
    }
    return static_cast<std::uint64_t>(data_.size());
}

Result<StorageMetadata> MemoryStorageBackend::metadata() const {
    std::shared_lock lock(mutex_);
    if (!open_.load(std::memory_order_acquire)) {
        return storage_error(ErrorCode::InvalidState, "Cannot inspect closed memory storage.");
    }
    return StorageMetadata{std::nullopt, static_cast<std::uint64_t>(data_.size()), true, true,
                           writable_access(access_), std::nullopt, "memory"};
}

Result<void> MemoryStorageBackend::close() {
    std::unique_lock lock(mutex_);
    open_.store(false, std::memory_order_release);
    return {};
}

bool MemoryStorageBackend::is_open() const noexcept {
    return open_.load(std::memory_order_acquire);
}

StorageAccess MemoryStorageBackend::access() const noexcept {
    return access_;
}

StorageFile::StorageFile() : StorageFile(std::make_shared<StorageOperationObserverRegistry>()) {}

StorageFile::StorageFile(std::shared_ptr<StorageOperationObserverRegistry> observers)
    : id_(generate_uuid()), observers_(std::move(observers)) {}

StorageFile::~StorageFile() {
    static_cast<void>(close());
}

StorageOperation StorageFile::begin_operation(
    StorageOperationType type, std::optional<std::uint64_t> offset,
    std::uint64_t requested_bytes) const {
    return StorageOperation{{}, type, id_, offset, requested_bytes, 0,
                            {}, std::nullopt, false, std::nullopt};
}

void StorageFile::notify_started(StorageOperation& operation) const noexcept {
    observers_->notify_started(operation);
}

void StorageFile::notify_completed(StorageOperation operation) const noexcept {
    if (operation.operation_id.empty()) {
        return;
    }
    operation.completed_at = std::chrono::system_clock::now();
    observers_->notify_completed(operation);
}

Result<void> StorageFile::open(
    const std::filesystem::path& path, const StorageOpenOptions& options) {
    auto operation = begin_operation(StorageOperationType::Open);
    notify_started(operation);
    bool reserved = false;
    {
        std::lock_guard lock(operation_mutex_);
        if (!opening_ && (!backend_ || !backend_->is_open())) {
            opening_ = true;
            reserved = true;
        }
    }
    if (!reserved) {
        auto failure = storage_error(ErrorCode::AlreadyInitialized,
                                     "Storage file is already open or opening.");
        operation.error = failure;
        notify_completed(std::move(operation));
        return failure;
    }

    auto clear_opening = [this] {
        std::lock_guard lock(operation_mutex_);
        opening_ = false;
    };
    auto opening_guard = make_scope_exit(clear_opening);
    auto backend = invoke_backend([&] { return NativeFileBackend::open(path, options); });
    if (!backend) {
        clear_opening();
        opening_guard.release();
        operation.error = backend.error();
        notify_completed(std::move(operation));
        return backend.error();
    }
    {
        std::lock_guard lock(operation_mutex_);
        backend_ = std::move(backend).value();
        cursor_ = 0;
        opening_ = false;
    }
    opening_guard.release();
    operation.succeeded = true;
    notify_completed(std::move(operation));
    return {};
}

Result<void> StorageFile::open_backend(std::shared_ptr<IStorageBackend> backend) {
    if (!backend || !backend->is_open()) {
        return storage_error(ErrorCode::InvalidArgument,
                             "Storage backend must be non-null and open.");
    }
    std::lock_guard lock(operation_mutex_);
    if (opening_ || (backend_ && backend_->is_open())) {
        return storage_error(ErrorCode::AlreadyInitialized,
                             "Storage file is already open.");
    }
    backend_ = std::move(backend);
    cursor_ = 0;
    return {};
}

Result<void> StorageFile::close() {
    {
        std::lock_guard lock(operation_mutex_);
        if (opening_) {
            return storage_error(ErrorCode::InvalidState,
                                 "Cannot close a storage file while it is opening.");
        }
        if (!backend_ || !backend_->is_open()) {
            return {};
        }
    }
    auto operation = begin_operation(StorageOperationType::Close);
    notify_started(operation);
    Result<void> result;
    {
        std::lock_guard lock(operation_mutex_);
        if (opening_) {
            result = storage_error(ErrorCode::InvalidState,
                                   "Cannot close a storage file while it is opening.");
        } else if (backend_ && backend_->is_open()) {
            result = invoke_backend([this] { return backend_->close(); });
        } else {
            result = {};
        }
    }
    operation.succeeded = static_cast<bool>(result);
    if (!result) {
        operation.error = result.error();
    }
    notify_completed(std::move(operation));
    return result;
}

Result<std::size_t> StorageFile::read_at(
    std::uint64_t offset, std::span<std::byte> buffer) {
    auto operation = begin_operation(StorageOperationType::Read, offset, buffer.size());
    notify_started(operation);
    Result<std::size_t> result = storage_error(
        ErrorCode::InvalidState, "Cannot read a closed storage file.");
    {
        std::lock_guard lock(operation_mutex_);
        if (backend_ && backend_->is_open()) {
            result = invoke_backend([&] { return backend_->read_at(offset, buffer); });
        }
    }
    operation.succeeded = static_cast<bool>(result);
    if (result) {
        if (result.value() > buffer.size()) {
            result = internal_storage_error(
                "Storage backend returned an invalid read length.");
            operation.succeeded = false;
        } else {
            operation.actual_bytes = result.value();
        }
    }
    if (!result) {
        operation.error = result.error();
        operation.actual_bytes = bounded_transferred_bytes(result.error(), buffer.size());
    }
    notify_completed(std::move(operation));
    return result;
}

Result<std::size_t> StorageFile::read(std::span<std::byte> buffer) {
    std::uint64_t offset = 0;
    {
        std::lock_guard lock(operation_mutex_);
        offset = cursor_;
    }
    auto operation = begin_operation(StorageOperationType::Read, offset, buffer.size());
    notify_started(operation);
    Result<std::size_t> result = storage_error(
        ErrorCode::InvalidState, "Cannot read a closed storage file.");
    {
        std::lock_guard lock(operation_mutex_);
        if (backend_ && backend_->is_open()) {
            if (buffer.size() > std::numeric_limits<std::uint64_t>::max() - cursor_) {
                result = storage_error(ErrorCode::InvalidArgument, "Sequential read range overflow.");
            } else {
                result = invoke_backend([&] { return backend_->read_at(cursor_, buffer); });
                if (result && result.value() <= buffer.size()) {
                    cursor_ += result.value();
                } else if (!result) {
                    cursor_ += bounded_transferred_bytes(result.error(), buffer.size());
                } else if (result) {
                    result = internal_storage_error(
                        "Storage backend returned an invalid read length.");
                }
            }
        }
    }
    operation.succeeded = static_cast<bool>(result);
    if (result) {
        if (result.value() > buffer.size()) {
            result = internal_storage_error(
                "Storage backend returned an invalid read length.");
            operation.succeeded = false;
        } else {
            operation.actual_bytes = result.value();
        }
    }
    if (!result) {
        operation.error = result.error();
        operation.actual_bytes = bounded_transferred_bytes(result.error(), buffer.size());
    }
    notify_completed(std::move(operation));
    return result;
}

Result<std::size_t> StorageFile::write_at(
    std::uint64_t offset, std::span<const std::byte> buffer) {
    auto operation = begin_operation(StorageOperationType::Write, offset, buffer.size());
    notify_started(operation);
    Result<std::size_t> result = storage_error(
        ErrorCode::InvalidState, "Cannot write to a closed storage file.");
    {
        std::lock_guard lock(operation_mutex_);
        if (backend_ && backend_->is_open()) {
            result = invoke_backend([&] { return backend_->write_at(offset, buffer); });
        }
    }
    operation.succeeded = static_cast<bool>(result);
    if (result) {
        if (result.value() > buffer.size()) {
            result = internal_storage_error(
                "Storage backend returned an invalid write length.");
            operation.succeeded = false;
        } else {
            operation.actual_bytes = result.value();
        }
    }
    if (!result) {
        operation.error = result.error();
        operation.actual_bytes = bounded_transferred_bytes(result.error(), buffer.size());
    }
    notify_completed(std::move(operation));
    return result;
}

Result<std::size_t> StorageFile::write(std::span<const std::byte> buffer) {
    std::uint64_t offset = 0;
    {
        std::lock_guard lock(operation_mutex_);
        offset = cursor_;
    }
    auto operation = begin_operation(StorageOperationType::Write, offset, buffer.size());
    notify_started(operation);
    Result<std::size_t> result = storage_error(
        ErrorCode::InvalidState, "Cannot write to a closed storage file.");
    {
        std::lock_guard lock(operation_mutex_);
        if (backend_ && backend_->is_open()) {
            if (buffer.size() > std::numeric_limits<std::uint64_t>::max() - cursor_) {
                result = storage_error(ErrorCode::InvalidArgument, "Sequential write range overflow.");
            } else {
                result = invoke_backend([&] { return backend_->write_at(cursor_, buffer); });
                if (result && result.value() <= buffer.size()) {
                    cursor_ += result.value();
                } else if (!result) {
                    cursor_ += bounded_transferred_bytes(result.error(), buffer.size());
                } else if (result) {
                    result = internal_storage_error(
                        "Storage backend returned an invalid write length.");
                }
            }
        }
    }
    operation.succeeded = static_cast<bool>(result);
    if (result) {
        if (result.value() > buffer.size()) {
            result = internal_storage_error(
                "Storage backend returned an invalid write length.");
            operation.succeeded = false;
        } else {
            operation.actual_bytes = result.value();
        }
    }
    if (!result) {
        operation.error = result.error();
        operation.actual_bytes = bounded_transferred_bytes(result.error(), buffer.size());
    }
    notify_completed(std::move(operation));
    return result;
}

Result<std::size_t> StorageFile::append(std::span<const std::byte> buffer) {
    auto operation = begin_operation(StorageOperationType::Append, std::nullopt, buffer.size());
    notify_started(operation);
    Result<std::size_t> result = storage_error(
        ErrorCode::InvalidState, "Cannot append to a closed storage file.");
    {
        std::lock_guard lock(operation_mutex_);
        if (backend_ && backend_->is_open()) {
            result = invoke_backend([&] { return backend_->append(buffer); });
        }
    }
    operation.succeeded = static_cast<bool>(result);
    if (result) {
        if (result.value() > buffer.size()) {
            result = internal_storage_error(
                "Storage backend returned an invalid append length.");
            operation.succeeded = false;
        } else {
            operation.actual_bytes = result.value();
        }
    }
    if (!result) {
        operation.error = result.error();
        operation.actual_bytes = bounded_transferred_bytes(result.error(), buffer.size());
    }
    notify_completed(std::move(operation));
    return result;
}

Result<void> StorageFile::seek(std::uint64_t offset) {
    std::lock_guard lock(operation_mutex_);
    if (!backend_ || !backend_->is_open()) {
        return storage_error(ErrorCode::InvalidState, "Cannot seek a closed storage file.");
    }
    cursor_ = offset;
    return {};
}

Result<std::uint64_t> StorageFile::position() const {
    std::lock_guard lock(operation_mutex_);
    if (!backend_ || !backend_->is_open()) {
        return storage_error(ErrorCode::InvalidState,
                             "Cannot retrieve the position of a closed storage file.");
    }
    return cursor_;
}

Result<void> StorageFile::truncate(std::uint64_t new_size) {
    auto operation = begin_operation(StorageOperationType::Truncate, std::nullopt, new_size);
    notify_started(operation);
    Result<void> result = storage_error(
        ErrorCode::InvalidState, "Cannot truncate a closed storage file.");
    {
        std::lock_guard lock(operation_mutex_);
        if (backend_ && backend_->is_open()) {
            result = invoke_backend([&] { return backend_->truncate(new_size); });
        }
    }
    operation.succeeded = static_cast<bool>(result);
    if (!result) {
        operation.error = result.error();
    }
    notify_completed(std::move(operation));
    return result;
}

Result<void> StorageFile::sync() {
    auto operation = begin_operation(StorageOperationType::Sync);
    notify_started(operation);
    Result<void> result = storage_error(
        ErrorCode::InvalidState, "Cannot sync a closed storage file.");
    {
        std::lock_guard lock(operation_mutex_);
        if (backend_ && backend_->is_open()) {
            result = invoke_backend([&] { return backend_->sync(); });
        }
    }
    operation.succeeded = static_cast<bool>(result);
    if (!result) {
        operation.error = result.error();
    }
    notify_completed(std::move(operation));
    return result;
}

Result<std::uint64_t> StorageFile::size() const {
    auto operation = begin_operation(StorageOperationType::Metadata);
    notify_started(operation);
    Result<std::uint64_t> result = storage_error(
        ErrorCode::InvalidState, "Cannot inspect a closed storage file.");
    {
        std::lock_guard lock(operation_mutex_);
        if (backend_ && backend_->is_open()) {
            result = invoke_backend([&] { return backend_->size(); });
        }
    }
    operation.succeeded = static_cast<bool>(result);
    if (result) {
        operation.actual_bytes = result.value();
    } else {
        operation.error = result.error();
    }
    notify_completed(std::move(operation));
    return result;
}

Result<StorageMetadata> StorageFile::metadata() const {
    auto operation = begin_operation(StorageOperationType::Metadata);
    notify_started(operation);
    Result<StorageMetadata> result = storage_error(
        ErrorCode::InvalidState, "Cannot inspect a closed storage file.");
    {
        std::lock_guard lock(operation_mutex_);
        if (backend_ && backend_->is_open()) {
            result = invoke_backend([&] { return backend_->metadata(); });
        }
    }
    operation.succeeded = static_cast<bool>(result);
    if (result) {
        operation.actual_bytes = result.value().size;
    } else {
        operation.error = result.error();
    }
    notify_completed(std::move(operation));
    return result;
}

bool StorageFile::is_open() const noexcept {
    std::lock_guard lock(operation_mutex_);
    return backend_ && backend_->is_open();
}

const std::string& StorageFile::id() const noexcept {
    return id_;
}

StorageAccess StorageFile::access() const noexcept {
    std::lock_guard lock(operation_mutex_);
    return backend_ ? backend_->access() : StorageAccess::ReadOnly;
}

std::uint64_t StorageFile::add_observer(std::shared_ptr<IStorageObserver> observer) {
    return observers_->add(std::move(observer));
}

bool StorageFile::remove_observer(std::uint64_t observer_id) {
    return observers_->remove(observer_id);
}

StorageManager::StorageManager()
    : observers_(std::make_shared<StorageOperationObserverRegistry>()) {}

Result<std::uint64_t> StorageManager::begin_resource_creation() const {
    std::shared_lock lock(lifecycle_mutex_);
    if (closing_ || !accepting_) {
        return storage_error(ErrorCode::InvalidState,
                             "Storage manager is not accepting new resources.");
    }
    return generation_;
}

Result<void> StorageManager::track(
    const std::shared_ptr<StorageFile>& file, std::uint64_t generation) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (closing_ || !accepting_ || generation != generation_) {
        return storage_error(ErrorCode::InvalidState,
                             "Storage manager shut down while the resource was being created.");
    }
    std::lock_guard lock(resources_mutex_);
    resources_.erase(std::remove_if(resources_.begin(), resources_.end(),
        [](const auto& resource) { return resource.expired(); }), resources_.end());
    resources_.push_back(file);
    return {};
}

Result<std::shared_ptr<StorageFile>> StorageManager::open_file(
    const std::filesystem::path& path, const StorageOpenOptions& options) {
    auto generation = begin_resource_creation();
    if (!generation) {
        return generation.error();
    }
    auto file = std::shared_ptr<StorageFile>(new StorageFile(observers_));
    auto result = file->open(path, options);
    if (!result) {
        return result.error();
    }
    auto tracked = track(file, generation.value());
    if (!tracked) {
        auto failure = tracked.error();
        if (options.access == StorageAccess::CreateNew) {
            failure = cleanup_snapshot(file, path, std::move(failure));
        } else {
            auto closed = file->close();
            if (!closed) {
                failure.metadata.emplace("cleanup_close_error", closed.error().message);
            }
        }
        return failure;
    }
    return file;
}

Result<std::shared_ptr<StorageFile>> StorageManager::create_memory_storage(
    std::size_t capacity) {
    auto generation = begin_resource_creation();
    if (!generation) {
        return generation.error();
    }
    auto file = std::shared_ptr<StorageFile>(new StorageFile(observers_));
    auto operation = file->begin_operation(StorageOperationType::Open);
    file->notify_started(operation);
    auto result = file->open_backend(std::make_shared<MemoryStorageBackend>(capacity));
    if (!result) {
        operation.error = result.error();
        file->notify_completed(std::move(operation));
        return result.error();
    }
    operation.succeeded = true;
    file->notify_completed(std::move(operation));
    auto tracked = track(file, generation.value());
    if (!tracked) {
        auto failure = tracked.error();
        auto closed = file->close();
        if (!closed) {
            failure.metadata.emplace("cleanup_close_error", closed.error().message);
        }
        return failure;
    }
    return file;
}

Result<std::shared_ptr<StorageFile>> StorageManager::create_storage(
    std::shared_ptr<IStorageBackend> backend) {
    auto generation = begin_resource_creation();
    if (!generation) {
        return generation.error();
    }
    auto file = std::shared_ptr<StorageFile>(new StorageFile(observers_));
    auto operation = file->begin_operation(StorageOperationType::Open);
    file->notify_started(operation);
    auto result = file->open_backend(std::move(backend));
    if (!result) {
        operation.error = result.error();
        file->notify_completed(std::move(operation));
        return result.error();
    }
    operation.succeeded = true;
    file->notify_completed(std::move(operation));
    auto tracked = track(file, generation.value());
    if (!tracked) {
        auto failure = tracked.error();
        auto closed = file->close();
        if (!closed) {
            failure.metadata.emplace("cleanup_close_error", closed.error().message);
        }
        return failure;
    }
    return file;
}

Result<void> StorageManager::copy_contents(
    const std::shared_ptr<StorageFile>& source,
    const std::shared_ptr<StorageFile>& destination,
    StorageOperation& operation) const {
    if (!source || !destination || source == destination) {
        return storage_error(ErrorCode::InvalidArgument,
                             "Snapshot source and destination must be distinct storage files.");
    }
    if (!destination->is_open() || !writable_access(destination->access())) {
        return storage_error(ErrorCode::InvalidState,
                             "Snapshot destination must be open and writable.");
    }

    std::scoped_lock file_locks(source->operation_mutex_, destination->operation_mutex_);
    if (!source->backend_ || !source->backend_->is_open()) {
        return storage_error(ErrorCode::InvalidState, "Snapshot source is not open.");
    }
    if (source->backend_ == destination->backend_) {
        return storage_error(ErrorCode::InvalidArgument,
                             "Snapshot source and destination share the same backend.");
    }
    auto source_size = invoke_backend([&] { return source->backend_->size(); });
    if (!source_size) {
        return source_size.error();
    }
    operation.requested_bytes = source_size.value();

    std::vector<std::byte> buffer(64 * 1024);
    std::uint64_t offset = 0;
    while (offset < source_size.value()) {
        const auto remaining = source_size.value() - offset;
        const auto request = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        auto read = invoke_backend([&] {
            return source->backend_->read_at(
                offset, std::span<std::byte>(buffer).first(request));
        });
        if (!read) {
            operation.actual_bytes += transferred_bytes(read.error());
            return read.error();
        }
        if (read.value() > request) {
            return internal_storage_error(
                "Snapshot source backend returned an invalid read length.");
        }
        if (read.value() == 0) {
            return storage_error(ErrorCode::IoError,
                                 "Snapshot source ended before its reported size.");
        }
        auto written = invoke_backend([&] {
            return destination->backend_->write_at(
                offset, std::span<const std::byte>(buffer).first(read.value()));
        });
        if (!written) {
            operation.actual_bytes += transferred_bytes(written.error());
            return written.error();
        }
        if (written.value() != read.value()) {
            return storage_error(ErrorCode::IoError, "Snapshot destination write was incomplete.");
        }
        offset += read.value();
        operation.actual_bytes += read.value();
    }
    auto sync_result = invoke_backend([&] { return destination->backend_->sync(); });
    if (!sync_result) {
        return sync_result.error();
    }
    return {};
}

Result<StorageSnapshot> StorageManager::create_snapshot(
    const std::shared_ptr<StorageFile>& source, const std::filesystem::path& destination_path) {
    if (!source || destination_path.empty()) {
        return storage_error(ErrorCode::InvalidArgument,
                             "Snapshot source and destination path are required.");
    }
    auto operation = source->begin_operation(StorageOperationType::Snapshot);
    source->notify_started(operation);

    StorageOpenOptions options;
    options.access = StorageAccess::CreateNew;
    auto destination = open_file(destination_path, options);
    if (!destination) {
        operation.error = destination.error();
        source->notify_completed(std::move(operation));
        return destination.error();
    }

    auto copied = copy_contents(source, destination.value(), operation);
    if (!copied) {
        auto failure = cleanup_snapshot(destination.value(), destination_path, copied.error());
        operation.error = failure;
        source->notify_completed(std::move(operation));
        return failure;
    }
    auto destination_metadata = destination.value()->metadata();
    if (!destination_metadata) {
        auto failure =
            cleanup_snapshot(destination.value(), destination_path, destination_metadata.error());
        operation.error = failure;
        source->notify_completed(std::move(operation));
        return failure;
    }

    operation.succeeded = true;
    source->notify_completed(std::move(operation));
    return StorageSnapshot{generate_uuid(), source->id(), destination.value(),
                           destination_metadata.value(), std::chrono::system_clock::now(),
                           true, false};
}

Result<StorageSnapshot> StorageManager::create_memory_snapshot(
    const std::shared_ptr<StorageFile>& source) {
    if (!source) {
        return storage_error(ErrorCode::InvalidArgument, "Snapshot source is required.");
    }
    auto operation = source->begin_operation(StorageOperationType::Snapshot);
    source->notify_started(operation);

    auto destination = create_memory_storage();
    if (!destination) {
        operation.error = destination.error();
        source->notify_completed(std::move(operation));
        return destination.error();
    }
    auto copied = copy_contents(source, destination.value(), operation);
    if (!copied) {
        auto failure = copied.error();
        auto closed = destination.value()->close();
        if (!closed) {
            failure.metadata.emplace("cleanup_close_error", closed.error().message);
        }
        operation.error = failure;
        source->notify_completed(std::move(operation));
        return failure;
    }
    auto destination_metadata = destination.value()->metadata();
    if (!destination_metadata) {
        auto failure = destination_metadata.error();
        auto closed = destination.value()->close();
        if (!closed) {
            failure.metadata.emplace("cleanup_close_error", closed.error().message);
        }
        operation.error = failure;
        source->notify_completed(std::move(operation));
        return failure;
    }
    operation.succeeded = true;
    source->notify_completed(std::move(operation));
    return StorageSnapshot{generate_uuid(), source->id(), destination.value(),
                           destination_metadata.value(), std::chrono::system_clock::now(),
                           true, false};
}

std::vector<std::shared_ptr<StorageFile>> StorageManager::active_resources() const {
    std::lock_guard lock(resources_mutex_);
    std::vector<std::shared_ptr<StorageFile>> result;
    result.reserve(resources_.size());
    for (const auto& resource : resources_) {
        if (auto file = resource.lock()) {
            if (file->is_open()) {
                result.push_back(std::move(file));
            }
        }
    }
    return result;
}

Result<void> StorageManager::close_all() {
    {
        std::unique_lock lock(lifecycle_mutex_);
        if (closing_) {
            return storage_error(ErrorCode::InvalidState,
                                 "Storage manager shutdown is already in progress.");
        }
        closing_ = true;
        ++generation_;
    }
    auto reset_closing = make_scope_exit([this] {
        std::unique_lock lock(lifecycle_mutex_);
        closing_ = false;
    });

    const auto resources = active_resources();
    std::optional<StorqError> first_error;
    for (const auto& resource : resources) {
        auto result = resource->close();
        if (!result && !first_error) {
            first_error = result.error();
        }
    }
    if (first_error) {
        return *first_error;
    }
    return {};
}

Result<void> StorageManager::activate() {
    std::unique_lock lock(lifecycle_mutex_);
    if (closing_) {
        return storage_error(ErrorCode::InvalidState,
                             "Storage manager shutdown is still in progress.");
    }
    accepting_ = true;
    ++generation_;
    return {};
}

Result<void> StorageManager::shutdown() {
    {
        std::unique_lock lock(lifecycle_mutex_);
        if (closing_) {
            return storage_error(ErrorCode::InvalidState,
                                 "Storage manager shutdown is already in progress.");
        }
        accepting_ = false;
        ++generation_;
    }
    return close_all();
}

std::uint64_t StorageManager::add_observer(std::shared_ptr<IStorageObserver> observer) {
    return observers_->add(std::move(observer));
}

bool StorageManager::remove_observer(std::uint64_t observer_id) {
    return observers_->remove(observer_id);
}

StorageModule::StorageModule(Runtime& runtime) : runtime_(runtime) {}

StorageModule::~StorageModule() {
    static_cast<void>(manager_.close_all());
}

std::string_view StorageModule::name() const noexcept {
    return "Storage";
}

Result<void> StorageModule::initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return storage_error(ErrorCode::AlreadyInitialized, "Storage module is already initialized.");
    }
    auto activated = manager_.activate();
    if (!activated) {
        initialized_.store(false, std::memory_order_release);
        return activated.error();
    }
    for (const auto capability : {"native_storage", "memory_storage", "storage_snapshots",
                                  "storage_observers"}) {
        static_cast<void>(runtime_.capabilities().register_capability(capability));
    }
    return {};
}

Result<void> StorageModule::shutdown() {
    if (!initialized_.load(std::memory_order_acquire)) {
        return {};
    }
    auto result = manager_.shutdown();
    if (!result) {
        return result.error();
    }
    initialized_.store(false, std::memory_order_release);
    return {};
}

bool StorageModule::is_initialized() const noexcept {
    return initialized_.load(std::memory_order_acquire);
}

StorageManager& StorageModule::manager() noexcept {
    return manager_;
}

const StorageManager& StorageModule::manager() const noexcept {
    return manager_;
}

} // namespace storq
