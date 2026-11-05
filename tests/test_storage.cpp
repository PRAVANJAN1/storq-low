#include <storq/storq.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

storq::Result<void> require_ok(storq::Result<void> result, const char* message) {
    if (!result) {
        throw std::runtime_error(std::string(message) + ": " + result.error().message);
    }
    return result;
}

std::span<const std::byte> bytes(std::string_view value) {
    return std::as_bytes(std::span(value.data(), value.size()));
}

std::string string_from(std::span<const std::byte> value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path temporary_directory() {
    auto directory = std::filesystem::temp_directory_path() /
                    ("storq-storage-" + storq::generate_uuid());
    std::filesystem::create_directories(directory);
    return directory;
}

class RecordingObserver final : public storq::IStorageObserver {
public:
    void on_operation_started(const storq::StorageOperation& operation) override {
        std::lock_guard lock(mutex_);
        started_.push_back(operation);
        if (throw_on_callback_) {
            throw std::runtime_error("observer start failure");
        }
    }

    void on_operation_completed(const storq::StorageOperation& operation) override {
        std::lock_guard lock(mutex_);
        completed_.push_back(operation);
        if (throw_on_callback_) {
            throw std::runtime_error("observer completion failure");
        }
    }

    void set_throw_on_callback(bool value) {
        throw_on_callback_ = value;
    }

    [[nodiscard]] std::vector<storq::StorageOperation> completed() const {
        std::lock_guard lock(mutex_);
        return completed_;
    }

    [[nodiscard]] std::size_t started_count() const {
        std::lock_guard lock(mutex_);
        return started_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<storq::StorageOperation> started_;
    std::vector<storq::StorageOperation> completed_;
    bool throw_on_callback_{false};
};

class SelfRemovingObserver final : public storq::IStorageObserver {
public:
    explicit SelfRemovingObserver(storq::StorageManager& manager) : manager_(manager) {}

    void set_id(std::uint64_t id) {
        id_ = id;
    }

    void on_operation_started(const storq::StorageOperation&) override {
        started_.fetch_add(1, std::memory_order_relaxed);
        static_cast<void>(manager_.remove_observer(id_));
    }

    void on_operation_completed(const storq::StorageOperation&) override {
        completed_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t started() const {
        return started_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t completed() const {
        return completed_.load(std::memory_order_relaxed);
    }

private:
    storq::StorageManager& manager_;
    std::uint64_t id_{0};
    std::atomic<std::size_t> started_{0};
    std::atomic<std::size_t> completed_{0};
};

class BlockingOpenObserver final : public storq::IStorageObserver {
public:
    void on_operation_started(const storq::StorageOperation& operation) override {
        if (operation.type != storq::StorageOperationType::Open) {
            return;
        }
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    void on_operation_completed(const storq::StorageOperation&) override {}

    bool wait_until_entered() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(5), [this] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{false};
    bool released_{false};
};

void test_native_backend() {
    const auto directory = temporary_directory();
    const auto path = directory / "native.bin";
    storq::StorageOpenOptions options;
    options.access = storq::StorageAccess::CreateNew;
    auto backend_result = storq::NativeFileBackend::open(path, options);
    require(static_cast<bool>(backend_result), "Could not create native file.");
    auto backend = std::move(backend_result).value();

    const auto first = bytes("native");
    auto written = backend->write_at(4, first);
    require(written && written.value() == first.size(), "Native positional write failed.");
    auto size = backend->size();
    require(size && size.value() == 10, "Native size after gapped write is incorrect.");
    std::array<std::byte, 12> buffer{};
    auto read = backend->read_at(0, buffer);
    require(read && read.value() == 10, "Native read did not return available bytes.");
    require(buffer[0] == std::byte{0} && string_from(
                std::span<const std::byte>(buffer).subspan(4, first.size())) == "native",
            "Native file contents differ.");

    const auto suffix = bytes("!");
    auto appended = backend->append(suffix);
    require(appended && appended.value() == 1, "Native append failed.");
    require(!backend->read_at(std::numeric_limits<std::uint64_t>::max(), buffer),
            "Native read accepted an overflowing range.");
    require(!backend->write_at(std::numeric_limits<std::uint64_t>::max(), first),
            "Native write accepted an overflowing range.");
    require_ok(backend->sync(), "Native sync failed.");
    require_ok(backend->truncate(3), "Native truncate failed.");
    size = backend->size();
    require(size && size.value() == 3, "Native truncate size is incorrect.");
    require_ok(backend->close(), "Native close failed.");
    require_ok(backend->close(), "Repeated native close failed.");
    require(!backend->read_at(0, buffer), "Closed native backend allowed a read.");

    options.access = storq::StorageAccess::ReadOnly;
    backend_result = storq::NativeFileBackend::open(path, options);
    require(static_cast<bool>(backend_result), "Could not reopen native file read-only.");
    backend = std::move(backend_result).value();
    require(!backend->write_at(0, bytes("x")), "Read-only native backend allowed writes.");
    auto metadata = backend->metadata();
    require(metadata && metadata.value().exists && !metadata.value().writable &&
                metadata.value().path == path && metadata.value().backend == "native",
            "Native metadata is incorrect.");
    require_ok(backend->sync(), "Read-only native sync failed.");
    require_ok(backend->close(), "Read-only native close failed.");

    require(!storq::NativeFileBackend::open(directory / "missing.bin",
                                             storq::StorageOpenOptions{storq::StorageAccess::ReadOnly}),
            "Opening a missing file succeeded.");
    options.access = storq::StorageAccess::ReadWrite;
    require(!storq::NativeFileBackend::open(directory / "missing-readwrite.bin", options),
            "ReadWrite created a missing file.");
    backend_result = storq::NativeFileBackend::open(path, options);
    require(static_cast<bool>(backend_result), "ReadWrite failed on an existing file.");
    require_ok(backend_result.value()->close(), "ReadWrite close failed.");
    options.access = storq::StorageAccess::Create;
    backend_result = storq::NativeFileBackend::open(path, options);
    require(static_cast<bool>(backend_result) && backend_result.value()->size().value() == 3,
            "Create mode truncated an existing file.");
    require_ok(backend_result.value()->close(), "Create-mode close failed.");
    options.access = storq::StorageAccess::CreateNew;
    require(!storq::NativeFileBackend::open(path, options),
            "CreateNew accepted an existing file.");
    options.access = storq::StorageAccess::ReadOnly;
    options.create_directories = true;
    require(!storq::NativeFileBackend::open(path, options),
            "ReadOnly accepted the create_directories option.");
    options.access = storq::StorageAccess::Truncate;
    options.create_directories = false;
    backend_result = storq::NativeFileBackend::open(path, options);
    require(static_cast<bool>(backend_result), "Truncate open failed.");
    require(backend_result.value()->size().value() == 0, "Truncate open did not clear the file.");
    require_ok(backend_result.value()->close(), "Truncate-open close failed.");

    const auto nested_path = directory / "nested" / "created.bin";
    options.access = storq::StorageAccess::CreateNew;
    options.create_directories = true;
    backend_result = storq::NativeFileBackend::open(nested_path, options);
    require(static_cast<bool>(backend_result), "Parent directory creation failed.");
    require_ok(backend_result.value()->close(), "Nested file close failed.");
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    require(!cleanup_error, "Native backend test cleanup failed.");
}

void test_memory_backend() {
    storq::MemoryStorageBackend backend(128);
    auto written = backend.write_at(4, bytes("memory"));
    require(written && written.value() == 6, "Memory positional write failed.");
    auto size = backend.size();
    require(size && size.value() == 10, "Memory write did not zero-fill the gap.");

    std::array<std::byte, 20> data{};
    auto read = backend.read_at(8, data);
    require(read && read.value() == 2, "Read beyond memory end returned incorrect length.");
    require(!backend.write_at(127, bytes("too long")), "Memory capacity limit was ignored.");
    require_ok(backend.truncate(20), "Memory truncate extension failed.");
    size = backend.size();
    require(size && size.value() == 20, "Memory truncate did not resize.");
    auto appended = backend.append(bytes("tail"));
    require(appended && appended.value() == 4, "Memory append failed.");

    std::atomic<bool> write_failed{false};
    std::vector<std::thread> writers;
    for (std::size_t index = 0; index < 8; ++index) {
        writers.emplace_back([&backend, &write_failed, index] {
            const auto payload = bytes("block");
            auto result = backend.write_at(64 + index * payload.size(), payload);
            if (!result) {
                write_failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    require(!write_failed.load(std::memory_order_relaxed), "Concurrent memory writes failed.");
    require(backend.size().value() == 104, "Concurrent memory writes corrupted the size.");
    std::atomic<bool> read_failed{false};
    std::vector<std::thread> readers;
    for (std::size_t index = 0; index < 8; ++index) {
        readers.emplace_back([&backend, &read_failed, index] {
            std::array<std::byte, 5> payload{};
            auto result = backend.read_at(64 + index * payload.size(), payload);
            if (!result || result.value() != payload.size() ||
                string_from(payload) != "block") {
                read_failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (auto& reader : readers) {
        reader.join();
    }
    require(!read_failed.load(std::memory_order_relaxed), "Concurrent memory reads failed.");
    const auto metadata = backend.metadata();
    require(metadata && !metadata.value().path && metadata.value().backend == "memory",
            "Memory backend reported filesystem metadata.");
    require_ok(backend.close(), "Memory backend close failed.");
    require(!backend.read_at(0, data), "Closed memory backend allowed a read.");
    require(!backend.write_at(0, bytes("x")), "Closed memory backend allowed a write.");

    storq::MemoryStorageBackend readonly(8, storq::StorageAccess::ReadOnly);
    require(!readonly.write_at(0, bytes("x")), "Read-only memory backend allowed writes.");
}

void test_storage_file_manager_observers_and_snapshots() {
    const auto directory = temporary_directory();
    const auto source_path = directory / "source.bin";
    const auto snapshot_path = directory / "snapshot.bin";
    storq::StorageManager manager;
    auto observer = std::make_shared<RecordingObserver>();
    observer->set_throw_on_callback(true);
    const auto observer_id = manager.add_observer(observer);
    require(observer_id != 0, "Observer registration failed.");
    require(manager.add_observer(nullptr) == 0, "Null observer registration succeeded.");

    storq::StorageOpenOptions options;
    options.access = storq::StorageAccess::CreateNew;
    auto opened = manager.open_file(source_path, options);
    require(static_cast<bool>(opened), "Storage manager failed to create a file.");
    auto file = opened.value();
    require(file->is_open() && file->access() == storq::StorageAccess::CreateNew,
            "StorageFile open state is incorrect.");
    const auto rejected_path = directory / "rejected-reopen.bin";
    require(!file->open(rejected_path, options), "An already-open StorageFile was reopened.");
    require(!std::filesystem::exists(rejected_path),
            "Rejected StorageFile reopen created a file as a side effect.");
    auto written = file->write_at(0, bytes("snapshot data"));
    require(written && written.value() == 13, "StorageFile write forwarding failed.");
    require_ok(file->sync(), "StorageFile sync forwarding failed.");
    auto metadata = file->metadata();
    require(metadata && metadata.value().size == 13 && metadata.value().writable,
            "StorageFile metadata retrieval failed.");

    auto snapshot = manager.create_snapshot(file, snapshot_path);
    require(static_cast<bool>(snapshot), "Native snapshot failed.");
    require(snapshot.value().metadata.size == 13 &&
                snapshot.value().metadata.path == snapshot_path &&
                !snapshot.value().externally_atomic &&
                snapshot.value().consistent_with_storage_file_operations,
            "Snapshot metadata or consistency flags are incorrect.");
    std::array<std::byte, 16> read_buffer{};
    auto read = snapshot.value().file->read_at(0, read_buffer);
    require(read && string_from(std::span<const std::byte>(read_buffer).first(read.value())) ==
                        "snapshot data",
            "Snapshot contents differ from the source.");
    require(!manager.create_snapshot(file, snapshot_path),
            "Snapshot overwrote an existing destination.");
    require(!manager.create_snapshot(file, directory / "missing" / "snapshot"),
            "Snapshot ignored a missing destination directory.");
    require(manager.active_resources().size() == 2,
            "Storage manager did not track active files.");

    auto memory = manager.create_memory_storage(64);
    require(static_cast<bool>(memory), "Memory StorageFile creation failed.");
    require(static_cast<bool>(memory.value()->write_at(0, bytes("in memory"))),
            "Memory StorageFile write failed.");
    auto memory_snapshot = manager.create_memory_snapshot(memory.value());
    require(static_cast<bool>(memory_snapshot), "Memory snapshot failed.");
    require(!memory_snapshot.value().metadata.path &&
                memory_snapshot.value().metadata.size == 9,
            "Memory snapshot metadata is incorrect.");
    std::array<std::byte, 16> memory_buffer{};
    read = memory_snapshot.value().file->read_at(0, memory_buffer);
    require(read && string_from(std::span<const std::byte>(memory_buffer).first(read.value())) ==
                        "in memory",
            "Memory snapshot content differs.");

    auto sequential = manager.create_memory_storage();
    require(static_cast<bool>(sequential), "Sequential memory file creation failed.");
    require(static_cast<bool>(sequential.value()->write(bytes("sequential"))),
            "Sequential storage write failed.");
    require(sequential.value()->position().value() == 10,
            "Sequential write did not advance the file position.");
    require_ok(sequential.value()->seek(0), "StorageFile seek failed.");
    std::array<std::byte, 10> sequential_buffer{};
    read = sequential.value()->read(sequential_buffer);
    require(read && read.value() == 10 && string_from(sequential_buffer) == "sequential",
            "Sequential storage read failed.");
    require(sequential.value()->position().value() == 10,
            "Sequential read did not advance the file position.");

    const auto observed = observer->completed();
    require(!observed.empty(), "Storage observer received no operation callbacks.");
    require(std::all_of(observed.begin(), observed.end(), [](const auto& operation) {
                return operation.completed_at.has_value();
            }),
            "Observer received an incomplete operation.");
    require(std::any_of(observed.begin(), observed.end(), [&file](const auto& operation) {
                return operation.type == storq::StorageOperationType::Write &&
                       operation.resource_id == file->id() && operation.offset == 0 &&
                       operation.requested_bytes == 13 && operation.actual_bytes == 13 &&
                       operation.succeeded;
            }),
            "Observer received incomplete storage operation details.");
    require(manager.remove_observer(observer_id), "Observer removal failed.");
    require(!manager.remove_observer(observer_id), "Observer was removed twice.");

    auto self_removing = std::make_shared<SelfRemovingObserver>(manager);
    const auto self_removing_id = manager.add_observer(self_removing);
    self_removing->set_id(self_removing_id);
    require(static_cast<bool>(file->write_at(30, bytes("y"))),
            "Storage operation failed with a self-removing observer.");
    require(self_removing->started() == 1 && self_removing->completed() == 1,
            "Observer removal during an operation disrupted callback delivery.");

    std::atomic<bool> observer_failure{false};
    std::vector<std::thread> observer_workers;
    for (std::size_t index = 0; index < 8; ++index) {
        observer_workers.emplace_back([&, index] {
            auto concurrent_observer = std::make_shared<RecordingObserver>();
            const auto id = manager.add_observer(concurrent_observer);
            if (id == 0) {
                observer_failure.store(true, std::memory_order_relaxed);
                return;
            }
            auto result = file->write_at(32 + index, bytes("x"));
            const bool removed = manager.remove_observer(id);
            if (!result || !removed || concurrent_observer->started_count() == 0 ||
                concurrent_observer->completed().empty()) {
                observer_failure.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : observer_workers) {
        worker.join();
    }
    require(!observer_failure.load(std::memory_order_relaxed),
            "Concurrent observer registration/removal failed.");

    require_ok(manager.close_all(), "Storage manager close_all failed.");
    require(!file->is_open() && !snapshot.value().file->is_open() &&
                !memory.value()->is_open() && !memory_snapshot.value().file->is_open() &&
                !sequential.value()->is_open(),
            "close_all did not close every managed resource.");
    require_ok(file->close(), "Repeated StorageFile close failed.");
    require(!file->read_at(0, read_buffer), "Closed StorageFile allowed reads.");
    require(!manager.create_memory_snapshot(file), "Snapshot of a closed source succeeded.");
    auto reused_manager_file = manager.create_memory_storage(8);
    require(static_cast<bool>(reused_manager_file),
            "Storage manager could not be reused after close_all.");
    require_ok(manager.close_all(), "Reused storage manager cleanup failed.");

    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    require(!cleanup_error, "Storage manager test cleanup failed.");
}

void test_storage_module_and_custom_backend() {
    storq::Runtime runtime;
    auto module = std::make_shared<storq::StorageModule>(runtime);
    require(static_cast<bool>(runtime.modules().register_module(module)),
            "Storage module registration failed.");
    require(static_cast<bool>(runtime.initialize()), "Runtime with storage module failed.");
    require(module->is_initialized() && runtime.capabilities().contains("native_storage") &&
                runtime.capabilities().contains("memory_storage") &&
                runtime.capabilities().contains("storage_snapshots") &&
                runtime.capabilities().contains("storage_observers"),
            "Storage module failed to register its capabilities.");
    auto memory = module->manager().create_memory_storage();
    require(static_cast<bool>(memory), "Storage module manager is unavailable.");
    auto custom_storage = std::make_shared<storq::MemoryStorageBackend>(32);
    auto adopted = module->manager().create_storage(custom_storage);
    require(static_cast<bool>(adopted), "Custom storage backend registration failed.");
    require(static_cast<bool>(adopted.value()->append(bytes("custom"))),
            "Custom storage backend did not work through StorageFile.");
    require(static_cast<bool>(runtime.shutdown()), "Runtime storage shutdown failed.");
    require(!memory.value()->is_open() && !adopted.value()->is_open() &&
                !module->is_initialized(),
            "Storage module did not close its resources during shutdown.");
    require(!module->manager().create_memory_storage(),
            "Storage module manager accepted resources after runtime shutdown.");
}

void test_manager_shutdown_race() {
    const auto directory = temporary_directory();
    const auto path = directory / "racing-open.bin";
    storq::StorageManager manager;
    auto observer = std::make_shared<BlockingOpenObserver>();
    const auto observer_id = manager.add_observer(observer);
    std::optional<storq::Result<std::shared_ptr<storq::StorageFile>>> open_result;
    std::thread opener([&] {
        storq::StorageOpenOptions options;
        options.access = storq::StorageAccess::CreateNew;
        open_result = manager.open_file(path, options);
    });

    if (!observer->wait_until_entered()) {
        observer->release();
        opener.join();
        throw std::runtime_error("Resource-open observer did not start.");
    }
    require_ok(manager.close_all(), "Concurrent manager shutdown failed.");
    require(manager.remove_observer(observer_id), "Blocking observer removal failed.");
    observer->release();
    opener.join();
    require(open_result.has_value() && !*open_result,
            "A resource opened during shutdown was not rejected.");
    require(!std::filesystem::exists(path),
            "Rejected CreateNew resource was not cleaned up.");

    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    require(!cleanup_error, "Shutdown race test cleanup failed.");
}

} // namespace

int main() {
    try {
        test_native_backend();
        test_memory_backend();
        test_storage_file_manager_observers_and_snapshots();
        test_storage_module_and_custom_backend();
        test_manager_shutdown_race();
        std::cout << "All Storq storage tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Storage test failure: " << exception.what() << '\n';
        return 1;
    }
}
