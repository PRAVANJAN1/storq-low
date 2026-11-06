#include <storq/storq.hpp>

#include <iostream>

int main() {
    storq::Runtime runtime;
    auto initialized = runtime.initialize();
    if (!initialized) return 1;

    storq::CrashSimulator simulator(runtime);
    if (!simulator.initialize()) return 2;

    storq::StorageManager storage;
    auto file_result = storage.create_memory_storage();
    if (!file_result) return 3;
    auto file = file_result.value();

    simulator.observe(file);
    const std::vector<std::byte> data(16, static_cast<std::byte>(0x2a));
    if (!file->write_at(0, data)) return 4;

    storq::CrashScenario scenario;
    scenario.id = "wal-crash-example";
    scenario.name = "Interrupted WAL write";
    scenario.type = storq::CrashType::WALWriteInterruption;
    scenario.trigger.type = storq::CrashTriggerType::OperationSequence;
    scenario.trigger.operation_sequence = 1;
    if (!simulator.register_scenario(scenario)) return 5;
    if (!simulator.arm(scenario.id)) return 6;

    auto report = simulator.execute(file);
    if (!report) return 7;
    auto snapshot = simulator.snapshot(report.value());
    if (!snapshot) return 8;

    std::cout << "Crash: " << report.value().crash_id << '\n';
    std::cout << "Interrupted operations: " << report.value().interrupted_operations << '\n';
    std::cout << "Snapshot: " << snapshot.value().snapshot_id << '\n';

    simulator.shutdown();
    runtime.shutdown();
    return 0;
}
