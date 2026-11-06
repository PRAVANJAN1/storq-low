#include <storq/storq.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

namespace {

std::vector<std::byte> make_sqlite_fixture() {
    std::vector<std::byte> page(512, std::byte{0});
    constexpr char signature[] = "SQLite format 3";
    for (std::size_t index = 0; index < 15; ++index) {
        page[index] = static_cast<std::byte>(signature[index]);
    }
    page[16] = std::byte{0x02};
    page[18] = std::byte{1};
    page[19] = std::byte{1};
    page[21] = std::byte{64};
    page[22] = std::byte{32};
    page[23] = std::byte{32};
    page[27] = std::byte{1};
    page[31] = std::byte{1};
    page[95] = std::byte{1};
    page[100] = std::byte{0x0d};
    page[106] = std::byte{0xf0};
    return page;
}

storq::Result<std::vector<std::byte>> read_all(storq::StorageFile& file) {
    auto size = file.size();
    if (!size) return size.error();
    std::vector<std::byte> content(static_cast<std::size_t>(size.value()));
    auto read = file.read_at(0, content);
    if (!read) return read.error();
    content.resize(read.value());
    return content;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("storq-fault-example-" + storq::generate_uuid());
    std::error_code filesystem_error;
    if (!std::filesystem::create_directory(root, filesystem_error) || filesystem_error) {
        std::cerr << "Unable to create isolated example directory: "
                  << filesystem_error.message() << '\n';
        return 1;
    }
    const auto source_path = root / "source.sqlite";
    const auto copy_path = root / "working.sqlite";
    int exit_code = 0;
    {
        storq::Runtime runtime;
        storq::StorageManager storage;
        auto injector = std::make_shared<storq::FaultInjector>(runtime);
        auto registered = runtime.modules().register_module(injector);
        if (!registered) {
            std::cerr << registered.error().message << '\n';
            exit_code = 1;
        }
        auto initialized = exit_code == 0 ? runtime.initialize()
                                          : storq::Result<void>(registered.error());
        if (!initialized) {
            std::cerr << initialized.error().message << '\n';
            exit_code = 1;
        }
        if (exit_code == 0) {
            auto environment = injector->configure_environment(
                {root, true, true, true});
            if (!environment) {
                std::cerr << environment.error().message << '\n';
                exit_code = 1;
            }
        }
        storq::StorageOpenOptions create_options;
        create_options.access = storq::StorageAccess::CreateNew;
        std::vector<std::byte> original_before;
        if (exit_code == 0) {
            auto source = storage.open_file(source_path, create_options);
            if (!source) {
                std::cerr << source.error().message << '\n';
                exit_code = 1;
            } else {
                const auto database = make_sqlite_fixture();
                auto written = source.value()->write_at(0, database);
                if (!written || written.value() != database.size()) {
                    std::cerr << (written ? "Incomplete source fixture write."
                                          : written.error().message) << '\n';
                    exit_code = 1;
                }
                auto closed = source.value()->close();
                if (!closed) {
                    std::cerr << closed.error().message << '\n';
                    exit_code = 1;
                }
            }
        }
        if (exit_code == 0) {
            auto copy = injector->create_test_copy(storage, source_path, copy_path);
            if (!copy) {
                std::cerr << copy.error().message << '\n';
                exit_code = 1;
            }
        }
        if (exit_code == 0) {
            storq::StorageOpenOptions read_only;
            read_only.access = storq::StorageAccess::ReadOnly;
            auto source = storage.open_file(source_path, read_only);
            if (!source) {
                std::cerr << source.error().message << '\n';
                exit_code = 1;
            } else {
                auto snapshot = read_all(*source.value());
                if (!snapshot) {
                    std::cerr << snapshot.error().message << '\n';
                    exit_code = 1;
                } else {
                    original_before = std::move(snapshot.value());
                }
                auto closed = source.value()->close();
                if (!closed) {
                    std::cerr << closed.error().message << '\n';
                    exit_code = 1;
                }
            }
        }
        if (exit_code == 0) {
            auto partial = storq::FaultRule{};
            partial.id = "partial-write-once";
            partial.config.type = storq::FaultType::PartialWrite;
            partial.config.partial_write_bytes = 8;
            partial.target_operation = storq::StorageOperationType::Write;
            auto partial_added = injector->add_rule(partial);
            if (!partial_added) {
                std::cerr << partial_added.error().message << '\n';
                exit_code = 1;
            }
            auto sync = storq::FaultRule{};
            sync.id = "sync-failure-once";
            sync.config.type = storq::FaultType::SyncFailure;
            sync.target_operation = storq::StorageOperationType::Sync;
            auto sync_added = exit_code == 0 ? injector->add_rule(sync)
                                              : storq::Result<void>(partial_added.error());
            if (!sync_added) {
                std::cerr << sync_added.error().message << '\n';
                exit_code = 1;
            }
        }
        if (exit_code == 0) {
            storq::StorageOpenOptions writable;
            writable.access = storq::StorageAccess::ReadWrite;
            auto working = injector->open_file(storage, copy_path, writable);
            if (!working) {
                std::cerr << working.error().message << '\n';
                exit_code = 1;
            } else {
                const std::array<std::byte, 16> payload{
                    std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                    std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
                    std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12},
                    std::byte{13}, std::byte{14}, std::byte{15}, std::byte{16}};
                auto write = working.value()->write_at(128, payload);
                if (!write || write.value() != 8) {
                    std::cerr << (write ? "Partial-write count mismatch."
                                        : write.error().message) << '\n';
                    exit_code = 1;
                }
                auto flush = working.value()->sync();
                if (flush) {
                    std::cerr << "Configured synchronization failure did not trigger.\n";
                    exit_code = 1;
                }
                auto closed = working.value()->close();
                if (!closed) {
                    std::cerr << closed.error().message << '\n';
                    exit_code = 1;
                }
            }
        }
        if (exit_code == 0) {
            storq::StorageOpenOptions read_only;
            read_only.access = storq::StorageAccess::ReadOnly;
            auto source = storage.open_file(source_path, read_only);
            if (!source) {
                std::cerr << source.error().message << '\n';
                exit_code = 1;
            } else {
                auto after = read_all(*source.value());
                if (!after || after.value() != original_before) {
                    std::cerr << "Original database changed during fault injection.\n";
                    exit_code = 1;
                }
                auto closed = source.value()->close();
                if (!closed) {
                    std::cerr << closed.error().message << '\n';
                    exit_code = 1;
                }
            }
        }
        for (const auto& entry : injector->history()) {
            std::cout << entry.fault_id << " [" << storq::fault_type_name(entry.type)
                      << "] " << storq::fault_status_name(entry.status)
                      << ": " << entry.description << '\n';
        }
        const auto statistics = injector->statistics();
        std::cout << "Configured: " << statistics.total_configured
                  << ", triggered: " << statistics.total_triggered
                  << ", executed: " << statistics.total_executed
                  << ", failed: " << statistics.total_failed
                  << ", partial bytes: " << statistics.total_partial_bytes << '\n';
        auto cleaned = injector->cleanup_file(copy_path);
        if (!cleaned && std::filesystem::exists(copy_path)) {
            std::cerr << cleaned.error().message << '\n';
            exit_code = 1;
        }
        auto shutdown = runtime.shutdown();
        if (!shutdown) {
            std::cerr << shutdown.error().message << '\n';
            exit_code = 1;
        }
    }
    std::filesystem::remove(source_path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::remove(root, filesystem_error);
    return exit_code;
}
