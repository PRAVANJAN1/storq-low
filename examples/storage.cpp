#include <iostream>
#include <string>

#include <storq/storq.hpp>

int main() {
    storq::Runtime runtime;
    auto storage_module = std::make_shared<storq::StorageModule>(runtime);
    auto registered = runtime.modules().register_module(storage_module);
    if (!registered) {
        std::cerr << registered.error().message << '\n';
        return 1;
    }

    auto initialized = runtime.initialize();
    if (!initialized) {
        std::cerr << initialized.error().message << '\n';
        return 1;
    }

    const auto directory =
        std::filesystem::temp_directory_path() / ("storq-example-" + storq::generate_uuid());
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        std::cerr << "Unable to create example directory: " << filesystem_error.message() << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    auto cleanup = storq::make_scope_exit([&directory] {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    });

    auto& manager = storage_module->manager();
    storq::StorageOpenOptions options;
    options.access = storq::StorageAccess::CreateNew;
    auto file_result = manager.open_file(directory / "database.storq", options);
    if (!file_result) {
        std::cerr << file_result.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    auto file = file_result.value();

    const std::string payload = "Storq storage abstraction";
    auto write = file->write_at(0, std::as_bytes(std::span(payload)));
    if (!write) {
        std::cerr << write.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    auto synchronized = file->sync();
    if (!synchronized) {
        std::cerr << synchronized.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }

    std::string readback(payload.size(), '\0');
    auto read = file->read_at(0, std::as_writable_bytes(std::span(readback)));
    if (!read) {
        std::cerr << read.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    readback.resize(read.value());
    auto metadata = file->metadata();
    if (!metadata) {
        std::cerr << metadata.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    std::cout << "Read back: " << readback << '\n'
              << "Backend: " << metadata.value().backend << '\n'
              << "Size: " << metadata.value().size << " bytes\n"
              << "Path: " << metadata.value().path->string() << '\n';

    auto snapshot = manager.create_snapshot(file, directory / "database.snapshot");
    if (!snapshot) {
        std::cerr << snapshot.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    std::cout << "Snapshot: " << snapshot.value().metadata.path->string()
              << " (" << snapshot.value().metadata.size << " bytes)\n";

    auto memory = manager.create_memory_storage(1024);
    if (!memory) {
        std::cerr << memory.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    auto memory_write = memory.value()->append(std::as_bytes(std::span(payload)));
    if (!memory_write) {
        std::cerr << memory_write.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    const auto memory_metadata = memory.value()->metadata();
    if (!memory_metadata) {
        std::cerr << memory_metadata.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    std::cout << "In-memory backend: " << memory_metadata.value().size << " bytes\n";

    auto closed = manager.close_all();
    if (!closed) {
        std::cerr << closed.error().message << '\n';
        static_cast<void>(runtime.shutdown());
        return 1;
    }
    auto shutdown = runtime.shutdown();
    if (!shutdown) {
        std::cerr << shutdown.error().message << '\n';
        return 1;
    }
    return 0;
}
