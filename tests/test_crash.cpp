#include <storq/storq.hpp>

#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    storq::Runtime runtime;
    assert(runtime.initialize());
    storq::CrashSimulator simulator(runtime);
    assert(simulator.initialize());

    storq::CrashScenario scenario;
    scenario.id = "exact-write";
    scenario.name = "Exact write crash";
    scenario.type = storq::CrashType::WriteInterruption;
    scenario.trigger.type = storq::CrashTriggerType::OperationSequence;
    scenario.trigger.operation_sequence = 1;
    assert(simulator.register_scenario(scenario));
    assert(simulator.arm(scenario.id));

    storq::StorageManager storage;
    auto memory = storage.create_memory_storage();
    assert(memory);
    simulator.observe(memory.value());
    const std::vector<std::byte> data(32, static_cast<std::byte>(0x7f));
    assert(memory.value()->write_at(0, data));
    auto report = simulator.execute(memory.value());
    assert(report);
    assert(report.value().interrupted_operations == 1);
    assert(simulator.controller().state() == storq::SimulatedProcessState::Crashed);

    auto snapshot = simulator.snapshot(report.value());
    assert(snapshot);
    assert(snapshot.value().complete);
    assert(!simulator.history().executions().empty());

    storq::CrashScenario invalid;
    invalid.name = "";
    assert(!storq::validate_crash_scenario(invalid));

    storq::CrashController controller;
    assert(controller.crash(1));
    assert(!controller.crash(2));
    assert(controller.restart());

    simulator.shutdown();
    assert(runtime.shutdown());
    std::cout << "Crash simulation tests passed\n";
    return 0;
}
