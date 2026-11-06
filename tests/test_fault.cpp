#include <storq/storq.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
T require_value(storq::Result<T> result, const char* message) {
    if (!result) throw std::runtime_error(std::string(message) + ": " + result.error().message);
    return std::move(result.value());
}

void require_ok(storq::Result<void> result, const char* message) {
    if (!result) throw std::runtime_error(std::string(message) + ": " + result.error().message);
}

std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::filesystem::path make_directory() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("storq-fault-" + storq::generate_uuid());
    std::filesystem::create_directories(path);
    return path;
}

struct Fixture {
    storq::Runtime runtime;
    std::shared_ptr<storq::FaultInjector> injector;
    storq::StorageManager storage;
    std::filesystem::path root{make_directory()};

    Fixture() : injector(std::make_shared<storq::FaultInjector>(runtime)) {
        require_ok(runtime.modules().register_module(injector), "Register injector");
        require_ok(runtime.initialize(), "Initialize runtime");
        require_ok(injector->configure_environment({root, true, true, true}),
                   "Configure disposable test environment");
    }

    ~Fixture() {
        static_cast<void>(runtime.shutdown());
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

class CountingObserver final : public storq::IStorageObserver {
public:
    void on_operation_started(const storq::StorageOperation&) override {
        ++started;
    }

    void on_operation_completed(const storq::StorageOperation&) override {
        ++completed;
    }

    std::atomic<std::uint64_t> started{0};
    std::atomic<std::uint64_t> completed{0};
};

std::vector<std::byte> sqlite_page() {
    std::vector<std::byte> page(512, std::byte{0});
    constexpr char signature[] = "SQLite format 3";
    for (std::size_t i = 0; i < 15; ++i) {
        page[i] = static_cast<std::byte>(signature[i]);
    }
    page[15] = std::byte{0};
    page[16] = std::byte{0x02};
    page[17] = std::byte{0x00};
    page[18] = std::byte{1};
    page[19] = std::byte{1};
    page[21] = std::byte{64};
    page[22] = std::byte{32};
    page[23] = std::byte{32};
    page[27] = std::byte{1};
    page[31] = std::byte{1};
    page[95] = std::byte{1};
    page[100] = std::byte{0x0d};
    page[105] = std::byte{1};
    page[106] = std::byte{0xf0};
    return page;
}

storq::FaultRule one_shot(std::string id, storq::FaultType type) {
    storq::FaultRule rule;
    rule.id = std::move(id);
    rule.config.type = type;
    rule.maximum_triggers = 1;
    return rule;
}

void test_configuration() {
    storq::FaultConfig config;
    require_ok(storq::FaultPolicy::validate(config), "Validate default fault config");
    config.probability = 1.1;
    require(!storq::FaultPolicy::validate(config), "Reject invalid probability");
    config = {};
    config.delay = std::chrono::nanoseconds(-1);
    require(!storq::FaultPolicy::validate(config), "Reject negative delay");
    config = {};
    config.type = storq::FaultType::TargetedCorruption;
    config.target_offset = std::numeric_limits<std::uint64_t>::max();
    config.corruption_length = 2;
    require(!storq::FaultPolicy::validate(config), "Reject overflowing corruption range");
    config = {};
    config.type = storq::FaultType::TruncatedWrite;
    require(!storq::FaultPolicy::validate(config), "Require truncated-write length");
    config = {};
    config.type = storq::FaultType::PageHeaderCorruption;
    config.target_page = std::numeric_limits<std::uint32_t>::max() + 1ULL;
    require(!storq::FaultPolicy::validate(config), "Reject impossible page number");
}

void test_partial_sync_and_history() {
    Fixture fixture;
    auto partial = one_shot("partial", storq::FaultType::PartialWrite);
    partial.config.partial_write_bytes = 4;
    require_ok(fixture.injector->add_rule(partial), "Add partial-write rule");
    require_ok(fixture.injector->add_rule(one_shot("sync", storq::FaultType::SyncFailure)),
               "Add sync-failure rule");
    auto file = require_value(fixture.injector->create_memory_storage(fixture.storage),
                              "Create fault-injected memory storage");
    auto written = require_value(file->write_at(0, bytes("abcdefgh")), "Partial write");
    require(written == 4, "Partial write should persist the configured prefix.");
    auto readback = std::array<std::byte, 8>{};
    auto read = require_value(file->read_at(0, readback), "Read partial write");
    require(read == 4 && readback[3] == std::byte{'d'},
            "Partial write must report and persist only the prefix.");
    auto sync = file->sync();
    require(!sync && sync.error().category == storq::ErrorCategory::FaultInjection,
            "Sync failure should be surfaced as a fault-injection error.");
    require_ok(file->close(), "Close partial-write storage");
    const auto stats = fixture.injector->statistics();
    require(stats.total_triggered == 2 && stats.total_executed == 1 &&
                stats.total_failed == 1 && stats.total_partial_bytes == 4,
            "Fault history should account for partial and failed sync lifecycles.");
    const auto trace = fixture.injector->operation_trace();
    require(trace.size() >= 3 &&
                std::is_sorted(trace.begin(), trace.end(),
                    [](const auto& left, const auto& right) {
                        return left.operation_sequence < right.operation_sequence;
                    }),
            "The operation trace must preserve the observed operation sequence.");
}

void test_disk_full_and_truncation() {
    Fixture fixture;
    auto disk_full = storq::FaultRule{};
    disk_full.id = "capacity";
    disk_full.config.type = storq::FaultType::DiskFull;
    disk_full.config.maximum_capacity = 5;
    require_ok(fixture.injector->add_rule(disk_full), "Add disk-full rule");
    auto file = require_value(fixture.injector->create_memory_storage(fixture.storage),
                              "Create capacity-limited test storage");
    require(require_value(file->write_at(0, bytes("abc")), "Write within capacity") == 3,
            "In-capacity write should proceed.");
    auto partial_capacity = require_value(
        fixture.injector->capacity_status(*file, "capacity"),
        "Read remaining virtual capacity");
    require(partial_capacity.maximum_capacity == 5 &&
                partial_capacity.current_usage == 3 &&
                partial_capacity.remaining_capacity == 2 &&
                !partial_capacity.full,
            "Capacity status should expose current usage and remaining virtual capacity.");
    require(require_value(file->write_at(3, bytes("wxyz")), "Write partial capacity") == 2,
            "Disk-full simulation should persist the remaining virtual capacity.");
    require(require_value(fixture.injector->capacity_status(*file, "capacity"),
                          "Read exhausted virtual capacity").full,
            "Capacity status should report exhaustion after the prefix write.");
    auto full = file->write_at(5, bytes("z"));
    require(!full, "Writes after virtual capacity exhaustion must fail.");
    require(require_value(file->size(), "Read virtual storage size") == 5,
            "Virtual disk capacity must not be exceeded.");
    require_ok(file->close(), "Close capacity test storage");

    Fixture truncation_fixture;
    require_ok(truncation_fixture.injector->add_rule(
                   [&] {
                       auto rule = one_shot("truncate", storq::FaultType::FileTruncation);
                       rule.config.target_size = 2;
                       return rule;
                   }()),
               "Add truncation rule");
    auto truncated = require_value(
        truncation_fixture.injector->create_memory_storage(truncation_fixture.storage),
        "Create truncation storage");
    require_value(truncated->write_at(0, bytes("12345")), "Seed truncation storage");
    require_ok(truncated->truncate(4), "Apply configured truncation");
    require(require_value(truncated->size(), "Read truncation result") == 2,
            "File truncation must use the explicitly configured target size.");
    require_ok(truncated->close(), "Close truncation storage");
}

void test_rejection_and_cancellation() {
    Fixture fixture;
    require_ok(fixture.injector->add_rule(
                   one_shot("create-failure", storq::FaultType::FileCreationFailure)),
               "Add create-failure rule");
    storq::StorageOpenOptions options;
    options.access = storq::StorageAccess::CreateNew;
    const auto rejected_path = fixture.root / "rejected.db";
    auto rejected = fixture.injector->open_file(fixture.storage, rejected_path, options);
    require(!rejected && !std::filesystem::exists(rejected_path),
            "Creation failure must occur before a test file is created.");

    auto duplicate = one_shot("create-failure", storq::FaultType::FileCreationFailure);
    require(!fixture.injector->add_rule(duplicate),
            "Duplicate rule identifiers must be rejected.");

    auto disabled = one_shot("disabled", storq::FaultType::WriteFailure);
    disabled.enabled = false;
    require_ok(fixture.injector->add_rule(disabled), "Add disabled fault");
    auto file = require_value(fixture.injector->create_memory_storage(fixture.storage),
                              "Create storage for disabled rule");
    require_value(file->write_at(0, bytes("ok")), "Write with disabled rule");
    require_ok(file->close(), "Close disabled-rule storage");
    require(fixture.injector->remove_rule("disabled"),
            "A scheduled rule should be cancellable.");
}

void test_environment_authorization_and_concurrent_operations() {
    storq::Runtime runtime;
    auto injector = std::make_shared<storq::FaultInjector>(runtime);
    require_ok(runtime.modules().register_module(injector), "Register protected injector");
    require_ok(runtime.initialize(), "Initialize protected runtime");
    const auto root = make_directory();
    require_ok(injector->configure_environment({root, true, true, false}),
               "Configure non-destructive environment");
    auto forbidden = one_shot("unauthorized", storq::FaultType::TargetedCorruption);
    forbidden.config.target_offset = 0;
    require(!injector->add_rule(forbidden),
            "Destructive rules must require explicit destructive authorization.");

    storq::StorageManager storage;
    auto observer = std::make_shared<CountingObserver>();
    require(storage.add_observer(observer) != 0, "Register storage observer.");
    auto file = require_value(injector->create_memory_storage(storage),
                              "Create concurrent memory storage");
    std::atomic<bool> writes_succeeded{true};
    std::vector<std::thread> writers;
    for (std::size_t worker = 0; worker < 4; ++worker) {
        writers.emplace_back([&, worker] {
            std::array<std::byte, 32> content{};
            content.fill(static_cast<std::byte>(worker + 1));
            auto result = file->write_at(worker * content.size(), content);
            if (!result || result.value() != content.size()) {
                writes_succeeded.store(false);
            }
        });
    }
    for (auto& writer : writers) writer.join();
    require(writes_succeeded.load(), "Concurrent storage writes must complete successfully.");
    require(require_value(file->size(), "Read concurrent storage size") == 128,
            "Concurrent writes must preserve backend size.");
    require(observer->started.load() >= 5 && observer->completed.load() >= 5,
            "The fault decorator must preserve StorageManager observer events.");
    require_ok(file->close(), "Close concurrent storage");
    require_ok(runtime.shutdown(), "Shutdown protected runtime");
    std::error_code ignored;
    std::filesystem::remove(root, ignored);
}

void test_rule_priority_triggers_and_probability() {
    auto history = std::make_shared<storq::FaultHistory>();
    storq::FaultScheduler scheduler(history, 0x55aa);
    auto lower = one_shot("lower", storq::FaultType::WriteFailure);
    lower.priority = 1;
    lower.maximum_triggers = 10;
    require_ok(scheduler.add_rule(lower), "Add lower-priority rule");
    auto higher = one_shot("higher", storq::FaultType::RejectedWrite);
    higher.priority = 5;
    higher.maximum_triggers = 10;
    require_ok(scheduler.add_rule(higher), "Add higher-priority rule");
    storq::FaultContext context;
    context.operation = storq::StorageOperationType::Write;
    context.resource_id = "memory";
    context.requested_bytes = 4;
    context.offset = 0;
    context.operation_sequence = 1;
    auto selected = require_value(scheduler.evaluate(context), "Evaluate priority rules");
    require(selected && selected->rule.id == "higher",
            "The highest-priority matching rule must execute exclusively.");

    auto probability_run = [](std::uint64_t seed) {
        auto run_history = std::make_shared<storq::FaultHistory>();
        storq::FaultScheduler run_scheduler(run_history, seed);
        storq::FaultContext probability_context;
        probability_context.operation = storq::StorageOperationType::Write;
        probability_context.resource_id = "memory";
        probability_context.requested_bytes = 4;
        probability_context.offset = 0;
        auto probabilistic = one_shot("probabilistic", storq::FaultType::WriteFailure);
        probabilistic.config.probability = 0.4;
        probabilistic.maximum_triggers = 64;
        require_ok(run_scheduler.add_rule(probabilistic), "Add probabilistic rule");
        std::vector<std::uint64_t> triggered;
        for (std::uint64_t sequence = 1; sequence <= 32; ++sequence) {
            probability_context.operation_sequence = sequence;
            auto decision = require_value(
                run_scheduler.evaluate(probability_context), "Evaluate probabilistic rule");
            if (decision) triggered.push_back(sequence);
        }
        return triggered;
    };
    require(probability_run(1337) == probability_run(1337),
            "Probability decisions must repeat for an identical seed and operation trace.");

    auto exact = one_shot("exact", storq::FaultType::WriteFailure);
    exact.trigger_count = 2;
    exact.maximum_triggers = 1;
    storq::FaultScheduler exact_scheduler(std::make_shared<storq::FaultHistory>(), 1);
    require_ok(exact_scheduler.add_rule(exact), "Add exact-trigger rule");
    context.operation_sequence = 1;
    require(!require_value(exact_scheduler.evaluate(context), "Evaluate first operation"),
            "Exact trigger must not fire before the selected occurrence.");
    context.operation_sequence = 2;
    require(require_value(exact_scheduler.evaluate(context), "Evaluate second operation")
                .has_value(),
            "Exact trigger must fire on the selected matching occurrence.");
}

void test_write_strategies_and_sync_interruption() {
    Fixture fixture;
    auto rejected_rule = one_shot("reject", storq::FaultType::RejectedWrite);
    rejected_rule.maximum_triggers = 1;
    require_ok(fixture.injector->add_rule(rejected_rule), "Add rejected-write rule");
    auto file = require_value(fixture.injector->create_memory_storage(fixture.storage),
                              "Create rejected-write storage");
    auto rejected = file->write_at(0, bytes("reject"));
    require(!rejected && rejected.error().message.find("rejected") != std::string::npos,
            "Rejected write must have semantics distinct from generic write failure.");
    require_ok(file->close(), "Close rejected-write storage");

    Fixture truncated_fixture;
    auto truncated_rule = one_shot("short", storq::FaultType::TruncatedWrite);
    truncated_rule.config.truncation_length = 2;
    require_ok(truncated_fixture.injector->add_rule(truncated_rule),
               "Add truncated-write rule");
    auto truncated_file = require_value(
        truncated_fixture.injector->create_memory_storage(truncated_fixture.storage),
        "Create truncated-write storage");
    require(require_value(truncated_file->write_at(0, bytes("four")), "Truncated write") == 2,
            "Truncated write must persist its configured fixed prefix.");
    require_ok(truncated_file->close(), "Close truncated-write storage");

    Fixture interrupted_fixture;
    require_ok(interrupted_fixture.injector->add_rule(
                   one_shot("interrupt", storq::FaultType::InterruptedSync)),
               "Add interrupted-sync rule");
    auto interrupted_file = require_value(
        interrupted_fixture.injector->create_memory_storage(interrupted_fixture.storage),
        "Create interrupted-sync storage");
    auto interrupted = interrupted_file->sync();
    require(!interrupted && interrupted.error().metadata.contains("interrupted"),
            "Interrupted sync must be distinguishable from ordinary sync failure.");
    require_ok(interrupted_file->close(), "Close interrupted-sync storage");

    Fixture delayed_fixture;
    auto delayed_rule = one_shot("delay", storq::FaultType::DelayedWrite);
    delayed_rule.config.delay = std::chrono::milliseconds(2);
    require_ok(delayed_fixture.injector->add_rule(delayed_rule), "Add delayed-write rule");
    auto delayed_file = require_value(
        delayed_fixture.injector->create_memory_storage(delayed_fixture.storage),
        "Create delayed-write storage");
    require_value(delayed_file->write_at(0, bytes("d")), "Execute delayed write");
    require_ok(delayed_file->close(), "Close delayed-write storage");
    require(delayed_fixture.injector->statistics().total_injected_delay.count() > 0,
            "Delayed-write history must retain the observed delay.");

    Fixture cancellation_fixture;
    auto cancellable_rule = one_shot("cancellable-delay", storq::FaultType::DelayedWrite);
    cancellable_rule.config.delay = std::chrono::seconds(2);
    require_ok(cancellation_fixture.injector->add_rule(cancellable_rule),
               "Add cancellable delay");
    auto cancellable_file = require_value(
        cancellation_fixture.injector->create_memory_storage(cancellation_fixture.storage),
        "Create cancellable storage");
    storq::Result<std::size_t> delayed_result(storq::StorqError{});
    std::thread writer([&] {
        delayed_result = cancellable_file->write_at(0, bytes("c"));
    });
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto entries = cancellation_fixture.injector->history();
        if (std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
                return entry.status == storq::FaultStatus::Triggered;
            })) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    cancellation_fixture.injector->cancel_pending_delays();
    writer.join();
    require(delayed_result.has_value(),
            "Cancelling a pending delay should allow the underlying write to proceed.");
    require(cancellation_fixture.injector->statistics().total_cancelled == 1,
            "Cancelled delays must be reflected in the fault lifecycle.");
    require_ok(cancellable_file->close(), "Close cancellable storage");
}

void test_corruption_and_page_metadata() {
    Fixture fixture;
    auto backend = std::make_shared<storq::MemoryStorageBackend>(512);
    const auto page = sqlite_page();
    require_value(backend->write_at(0, page), "Seed SQLite page");
    auto rule = one_shot("page-header", storq::FaultType::PageHeaderCorruption);
    rule.config.target_page = 1;
    rule.config.page_header_field = storq::SQLitePageHeaderField::PageType;
    rule.config.corruption_length = 1;
    rule.config.corruption_pattern = {std::byte{0x02}};
    require_ok(fixture.injector->add_rule(rule), "Add page-header corruption");
    auto file = require_value(fixture.injector->wrap_storage(fixture.storage, backend),
                              "Wrap SQLite test backend");
    require_value(file->write_at(0, page), "Apply page-header corruption");
    auto header_byte = std::array<std::byte, 1>{};
    require_value(file->read_at(100, header_byte), "Read corrupted page header");
    require(header_byte[0] == std::byte{0x02},
            "Page-header strategy should alter only the recognized field.");
    require_ok(file->close(), "Close page-header backend");
    require(fixture.injector->statistics().total_corrupted_bytes == 1,
            "Corruption history should count changed bytes without storing contents.");
}

void test_targeted_corruption_and_access_failures() {
    Fixture fixture;
    auto backend = std::make_shared<storq::MemoryStorageBackend>(32);
    auto targeted = one_shot("target-byte", storq::FaultType::TargetedCorruption);
    targeted.config.target_offset = 3;
    targeted.config.corruption_length = 2;
    targeted.config.corruption_pattern = {std::byte{0xa5}};
    require_ok(fixture.injector->add_rule(targeted), "Add targeted-corruption rule");
    auto file = require_value(fixture.injector->wrap_storage(fixture.storage, backend),
                              "Wrap targeted-corruption backend");
    std::array<std::byte, 8> original{};
    original.fill(std::byte{0x11});
    require_value(file->write_at(0, original), "Write across targeted byte range");
    std::array<std::byte, 8> result{};
    require_value(file->read_at(0, result), "Read targeted corruption");
    require(result[2] == std::byte{0x11} && result[3] == std::byte{0xa5} &&
                result[4] == std::byte{0xa5} && result[5] == std::byte{0x11},
            "Targeted byte corruption must be limited to the configured interval.");
    require_ok(file->close(), "Close targeted-corruption backend");

    Fixture access_fixture;
    auto access_rule = one_shot("access-denied", storq::FaultType::FileAccessFailure);
    access_rule.target_operation = storq::StorageOperationType::Read;
    require_ok(access_fixture.injector->add_rule(access_rule),
               "Add access-failure rule");
    auto accessed = require_value(
        access_fixture.injector->create_memory_storage(access_fixture.storage),
        "Create access-failure storage");
    std::array<std::byte, 1> byte{};
    auto failed_read = accessed->read_at(0, byte);
    require(!failed_read, "File-access rule should reject a selected read.");
    require_ok(accessed->close(), "Close access-failure storage");

    Fixture offset_fixture;
    auto invalid_offset = one_shot("invalid-offset", storq::FaultType::InvalidOffset);
    invalid_offset.config.target_offset = std::numeric_limits<std::uint64_t>::max();
    require_ok(offset_fixture.injector->add_rule(invalid_offset), "Add invalid-offset rule");
    auto offset_file = require_value(
        offset_fixture.injector->create_memory_storage(offset_fixture.storage),
        "Create invalid-offset storage");
    auto invalid_write = offset_file->write_at(
        std::numeric_limits<std::uint64_t>::max() - 1, bytes("too long"));
    require(!invalid_write && invalid_write.error().code == storq::ErrorCode::InvalidArgument,
            "Invalid offset rule must reject overflow without reaching the OS backend.");
    require_ok(offset_file->close(), "Close invalid-offset storage");
}

std::vector<std::byte> run_random_corruption(std::uint64_t seed) {
    Fixture fixture;
    require_ok(fixture.injector->set_random_seed(seed), "Set deterministic seed");
    auto rule = one_shot("random", storq::FaultType::RandomCorruption);
    rule.config.corruption_length = 5;
    rule.config.random_seed = 77;
    require_ok(fixture.injector->add_rule(rule), "Add random corruption");
    auto file = require_value(fixture.injector->create_memory_storage(fixture.storage),
                              "Create deterministic storage");
    std::vector<std::byte> source(128, std::byte{0x7f});
    require_value(file->write_at(0, source), "Write deterministic source");
    std::vector<std::byte> result(source.size());
    require_value(file->read_at(0, result), "Read deterministic corruption");
    require_ok(file->close(), "Close deterministic storage");
    return result;
}

void test_determinism() {
    const auto first = run_random_corruption(0x1234);
    const auto second = run_random_corruption(0x1234);
    const auto rule_seeded = run_random_corruption(0xabcd);
    require(first == second, "Identical seeds and sequential traces must reproduce corruption.");
    require(first == rule_seeded,
            "A per-rule seed must remain reproducible independently of the global seed.");
}

void test_wal_corruption_and_safety() {
    Fixture fixture;
    const auto source_path = fixture.root.parent_path() /
                             ("storq-source-" + storq::generate_uuid() + ".wal");
    const auto copy_path = fixture.root / "working.wal";
    std::vector<std::byte> wal(32 + 24 + 512, std::byte{0});
    wal[0] = std::byte{0x37};
    wal[1] = std::byte{0x7f};
    wal[2] = std::byte{0x06};
    wal[3] = std::byte{0x82};
    wal[4] = std::byte{0x00};
    wal[5] = std::byte{0x2d};
    wal[6] = std::byte{0xe2};
    wal[7] = std::byte{0x18};
    wal[8] = std::byte{0x00};
    wal[9] = std::byte{0x00};
    wal[10] = std::byte{0x02};
    wal[11] = std::byte{0x00};
    wal[32] = std::byte{0};
    wal[35] = std::byte{1};
    {
        std::ofstream output(source_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(wal.data()),
                     static_cast<std::streamsize>(wal.size()));
        require(static_cast<bool>(output), "Create source WAL fixture.");
    }
    auto copy = require_value(
        fixture.injector->create_test_copy(fixture.storage, source_path, copy_path),
        "Copy WAL into disposable environment");
    auto corruption = one_shot("wal-page", storq::FaultType::WALRecordCorruption);
    corruption.config.wal_frame_number = 1;
    corruption.config.wal_region = storq::WALCorruptionRegion::PageContent;
    corruption.config.corruption_length = 2;
    corruption.config.corruption_pattern = {std::byte{0xfe}, std::byte{0xed}};
    require_ok(fixture.injector->add_rule(corruption), "Add WAL corruption rule");
    storq::StorageOpenOptions options;
    options.access = storq::StorageAccess::ReadWrite;
    auto file = require_value(fixture.injector->open_file(
        fixture.storage, copy, options), "Open disposable WAL copy");
    std::vector<std::byte> content(512, std::byte{0x11});
    require_value(file->write_at(56, content), "Corrupt selected WAL frame content");
    auto checksum_rule = one_shot("wal-checksum", storq::FaultType::ChecksumCorruption);
    checksum_rule.config.wal_frame_number = 1;
    checksum_rule.config.corruption_length = 4;
    checksum_rule.config.corruption_pattern = {std::byte{0x99}};
    require_ok(fixture.injector->add_rule(checksum_rule), "Add WAL checksum corruption");
    std::array<std::byte, 4> checksum_bytes{std::byte{1}, std::byte{2},
                                            std::byte{3}, std::byte{4}};
    require_value(file->write_at(48, checksum_bytes), "Corrupt selected WAL checksum field");
    std::array<std::byte, 2> changed{};
    require_value(file->read_at(56, changed), "Read corrupted WAL content");
    require(changed[0] == std::byte{0xfe} && changed[1] == std::byte{0xed},
            "WAL corruption should stay within the selected frame page content.");
    std::array<std::byte, 4> changed_checksum{};
    require_value(file->read_at(48, changed_checksum), "Read corrupted WAL checksum");
    require(std::all_of(changed_checksum.begin(), changed_checksum.end(),
                        [](std::byte value) { return value == std::byte{0x99}; }),
            "Checksum corruption must target the parsed frame checksum field.");
    require_ok(file->close(), "Close disposable WAL copy");
    std::ifstream source_input(source_path, std::ios::binary);
    std::vector<char> source_after{std::istreambuf_iterator<char>(source_input), {}};
    require(source_after.size() == wal.size() &&
                std::equal(source_after.begin(), source_after.end(),
                           reinterpret_cast<const char*>(wal.data())),
            "Original WAL source must remain unchanged.");
    require_ok(fixture.injector->cleanup_file(copy), "Remove registered test copy");
    std::error_code ignored;
    std::filesystem::remove(source_path, ignored);
}

void test_environment_path_rejection() {
    Fixture fixture;
    storq::StorageOpenOptions options;
    options.access = storq::StorageAccess::CreateNew;
    auto unsafe = fixture.injector->open_file(
        fixture.storage, fixture.root.parent_path() / "outside.db", options);
    require(!unsafe, "Writable storage outside the explicit test root must be rejected.");
}

} // namespace

int main() {
    try {
        test_configuration();
        test_partial_sync_and_history();
        test_disk_full_and_truncation();
        test_rejection_and_cancellation();
        test_environment_authorization_and_concurrent_operations();
        test_rule_priority_triggers_and_probability();
        test_write_strategies_and_sync_interruption();
        test_corruption_and_page_metadata();
        test_targeted_corruption_and_access_failures();
        test_determinism();
        test_wal_corruption_and_safety();
        test_environment_path_rejection();
        std::cout << "All fault-injection tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Fault-injection test failure: " << exception.what() << '\n';
        return 1;
    }
}
