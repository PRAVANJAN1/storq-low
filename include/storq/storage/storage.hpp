#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

#include <storq/core/module.hpp>

namespace storq {

class Runtime;

enum class StorageAccess {
    ReadOnly,
    ReadWrite,
    Create,
    CreateNew,
    Truncate
};

struct StorageOpenOptions {
    StorageAccess access{StorageAccess::Create};
    bool create_directories{false};
    bool exclusive_access{false};
    std::uint32_t permissions{0644};
};

struct StorageMetadata {
    std::optional<std::filesystem::path> path;
    std::uint64_t size{0};
    bool exists{true};
    bool readable{true};
    bool writable{true};
    std::optional<std::chrono::system_clock::time_point> modified_at;
    std::string backend;
};

enum class StorageOperationType {
    Open,
    Read,
    Write,
    Append,
    Truncate,
    Sync,
    Metadata,
    Snapshot,
    Close
};

struct StorageOperation {
    std::string operation_id;
    StorageOperationType type{StorageOperationType::Read};
    std::string resource_id;
    std::optional<std::uint64_t> offset;
    std::uint64_t requested_bytes{0};
    std::uint64_t actual_bytes{0};
    std::chrono::system_clock::time_point started_at;
    std::optional<std::chrono::system_clock::time_point> completed_at;
    bool succeeded{false};
    std::optional<StorqError> error;
};

class IStorageObserver {
public:
    virtual ~IStorageObserver() = default;
    virtual void on_operation_started(const StorageOperation& operation) = 0;
    virtual void on_operation_completed(const StorageOperation& operation) = 0;
};

class IStorageBackend {
public:
    virtual ~IStorageBackend() = default;

    [[nodiscard]] virtual Result<std::size_t> read_at(
        std::uint64_t offset, std::span<std::byte> buffer) = 0;
    [[nodiscard]] virtual Result<std::size_t> write_at(
        std::uint64_t offset, std::span<const std::byte> buffer) = 0;
    [[nodiscard]] virtual Result<std::size_t> append(
        std::span<const std::byte> buffer) = 0;
    [[nodiscard]] virtual Result<void> truncate(std::uint64_t size) = 0;
    [[nodiscard]] virtual Result<void> sync() = 0;
    [[nodiscard]] virtual Result<std::uint64_t> size() const = 0;
    [[nodiscard]] virtual Result<StorageMetadata> metadata() const = 0;
    [[nodiscard]] virtual Result<void> close() = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    [[nodiscard]] virtual StorageAccess access() const noexcept = 0;
};

class NativeFileBackend final : public IStorageBackend {
public:
    [[nodiscard]] static Result<std::shared_ptr<NativeFileBackend>> open(
        const std::filesystem::path& path, const StorageOpenOptions& options = {});
    ~NativeFileBackend() override;

    NativeFileBackend(const NativeFileBackend&) = delete;
    NativeFileBackend& operator=(const NativeFileBackend&) = delete;

    [[nodiscard]] Result<std::size_t> read_at(
        std::uint64_t offset, std::span<std::byte> buffer) override;
    [[nodiscard]] Result<std::size_t> write_at(
        std::uint64_t offset, std::span<const std::byte> buffer) override;
    [[nodiscard]] Result<std::size_t> append(
        std::span<const std::byte> buffer) override;
    [[nodiscard]] Result<void> truncate(std::uint64_t size) override;
    [[nodiscard]] Result<void> sync() override;
    [[nodiscard]] Result<std::uint64_t> size() const override;
    [[nodiscard]] Result<StorageMetadata> metadata() const override;
    [[nodiscard]] Result<void> close() override;
    [[nodiscard]] bool is_open() const noexcept override;
    [[nodiscard]] StorageAccess access() const noexcept override;

private:
    NativeFileBackend(std::filesystem::path path, StorageAccess access);

    std::filesystem::path path_;
    StorageAccess access_;
    mutable std::shared_mutex mutex_;
    std::atomic<bool> open_{false};
#if defined(_WIN32)
    void* handle_{nullptr};
    bool exclusive_locked_{false};
#else
    int descriptor_{-1};
    bool exclusive_locked_{false};
#endif
};

class MemoryStorageBackend final : public IStorageBackend {
public:
    explicit MemoryStorageBackend(
        std::size_t capacity = static_cast<std::size_t>(-1),
        StorageAccess access = StorageAccess::Create);

    [[nodiscard]] Result<std::size_t> read_at(
        std::uint64_t offset, std::span<std::byte> buffer) override;
    [[nodiscard]] Result<std::size_t> write_at(
        std::uint64_t offset, std::span<const std::byte> buffer) override;
    [[nodiscard]] Result<std::size_t> append(
        std::span<const std::byte> buffer) override;
    [[nodiscard]] Result<void> truncate(std::uint64_t size) override;
    [[nodiscard]] Result<void> sync() override;
    [[nodiscard]] Result<std::uint64_t> size() const override;
    [[nodiscard]] Result<StorageMetadata> metadata() const override;
    [[nodiscard]] Result<void> close() override;
    [[nodiscard]] bool is_open() const noexcept override;
    [[nodiscard]] StorageAccess access() const noexcept override;

private:
    Result<void> validate_open_and_writable() const;

    const std::size_t capacity_;
    const StorageAccess access_;
    mutable std::shared_mutex mutex_;
    std::vector<std::byte> data_;
    std::atomic<bool> open_{true};
};

class StorageOperationObserverRegistry;

class StorageFile : public std::enable_shared_from_this<StorageFile> {
public:
    StorageFile();
    ~StorageFile();

    StorageFile(const StorageFile&) = delete;
    StorageFile& operator=(const StorageFile&) = delete;

    [[nodiscard]] Result<void> open(
        const std::filesystem::path& path, const StorageOpenOptions& options = {});
    [[nodiscard]] Result<void> close();
    [[nodiscard]] Result<std::size_t> read_at(
        std::uint64_t offset, std::span<std::byte> buffer);
    [[nodiscard]] Result<std::size_t> read(std::span<std::byte> buffer);
    [[nodiscard]] Result<std::size_t> write_at(
        std::uint64_t offset, std::span<const std::byte> buffer);
    [[nodiscard]] Result<std::size_t> write(std::span<const std::byte> buffer);
    [[nodiscard]] Result<std::size_t> append(std::span<const std::byte> buffer);
    [[nodiscard]] Result<void> seek(std::uint64_t offset);
    [[nodiscard]] Result<std::uint64_t> position() const;
    [[nodiscard]] Result<void> truncate(std::uint64_t size);
    [[nodiscard]] Result<void> sync();
    [[nodiscard]] Result<std::uint64_t> size() const;
    [[nodiscard]] Result<StorageMetadata> metadata() const;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] StorageAccess access() const noexcept;

    [[nodiscard]] std::uint64_t add_observer(std::shared_ptr<IStorageObserver> observer);
    [[nodiscard]] bool remove_observer(std::uint64_t observer_id);

private:
    friend class StorageManager;

    explicit StorageFile(std::shared_ptr<StorageOperationObserverRegistry> observers);
    [[nodiscard]] Result<void> open_backend(std::shared_ptr<IStorageBackend> backend);
    [[nodiscard]] StorageOperation begin_operation(
        StorageOperationType type, std::optional<std::uint64_t> offset = std::nullopt,
        std::uint64_t requested_bytes = 0) const;
    void notify_started(StorageOperation& operation) const noexcept;
    void notify_completed(StorageOperation operation) const noexcept;

    std::string id_;
    std::shared_ptr<StorageOperationObserverRegistry> observers_;
    mutable std::mutex operation_mutex_;
    std::shared_ptr<IStorageBackend> backend_;
    bool opening_{false};
    std::uint64_t cursor_{0};
};

struct StorageSnapshot {
    std::string snapshot_id;
    std::string source_resource_id;
    std::shared_ptr<StorageFile> file;
    StorageMetadata metadata;
    std::chrono::system_clock::time_point created_at;
    bool consistent_with_storage_file_operations{true};
    bool externally_atomic{false};
};

class StorageManager {
public:
    StorageManager();

    [[nodiscard]] Result<std::shared_ptr<StorageFile>> open_file(
        const std::filesystem::path& path, const StorageOpenOptions& options = {});
    [[nodiscard]] Result<std::shared_ptr<StorageFile>> create_memory_storage(
        std::size_t capacity = static_cast<std::size_t>(-1));
    [[nodiscard]] Result<std::shared_ptr<StorageFile>> create_storage(
        std::shared_ptr<IStorageBackend> backend);
    [[nodiscard]] Result<StorageSnapshot> create_snapshot(
        const std::shared_ptr<StorageFile>& source, const std::filesystem::path& destination);
    [[nodiscard]] Result<StorageSnapshot> create_memory_snapshot(
        const std::shared_ptr<StorageFile>& source);

    [[nodiscard]] std::vector<std::shared_ptr<StorageFile>> active_resources() const;
    [[nodiscard]] Result<void> close_all();
    [[nodiscard]] std::uint64_t add_observer(std::shared_ptr<IStorageObserver> observer);
    [[nodiscard]] bool remove_observer(std::uint64_t observer_id);

private:
    friend class StorageModule;

    [[nodiscard]] Result<void> activate();
    [[nodiscard]] Result<void> shutdown();
    [[nodiscard]] Result<std::uint64_t> begin_resource_creation() const;
    [[nodiscard]] Result<void> track(
        const std::shared_ptr<StorageFile>& file, std::uint64_t generation);
    [[nodiscard]] Result<void> copy_contents(
        const std::shared_ptr<StorageFile>& source,
        const std::shared_ptr<StorageFile>& destination,
        StorageOperation& operation) const;

    std::shared_ptr<StorageOperationObserverRegistry> observers_;
    mutable std::shared_mutex lifecycle_mutex_;
    bool closing_{false};
    bool accepting_{true};
    std::uint64_t generation_{0};
    mutable std::mutex resources_mutex_;
    std::vector<std::weak_ptr<StorageFile>> resources_;
};

class StorageModule final : public IModule {
public:
    explicit StorageModule(Runtime& runtime);
    ~StorageModule() override;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Result<void> shutdown() override;
    [[nodiscard]] bool is_initialized() const noexcept override;
    [[nodiscard]] StorageManager& manager() noexcept;
    [[nodiscard]] const StorageManager& manager() const noexcept;

private:
    Runtime& runtime_;
    StorageManager manager_;
    std::atomic<bool> initialized_{false};
};

} // namespace storq
