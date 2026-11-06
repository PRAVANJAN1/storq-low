#include <storq/storq.hpp>

#include <iostream>

int main() {
    storq::Runtime runtime;
    if (!runtime.initialize()) return 1;
    storq::ForensicsEngine engine(runtime);
    if (!engine.initialize()) return 2;

    storq::Evidence partial_write = storq::make_evidence(
        storq::EvidenceType::InjectedFault, "Module6", "partial-write-1",
        "Injected partial write fault.");
    partial_write.operation_sequence = 20;
    partial_write.page_number = 12;

    storq::Evidence wal = storq::make_evidence(
        storq::EvidenceType::WalAnalysis, "Module5", "wal-frame-3",
        "Incomplete WAL frame observed.");
    wal.operation_sequence = 21;
    wal.wal_frame_number = 3;

    storq::Evidence crash = storq::make_evidence(
        storq::EvidenceType::CrashEvent, "Module7", "crash-1",
        "Simulated crash occurred immediately after the WAL write.");
    crash.operation_sequence = 22;
    crash.crash_id = "crash-1";

    storq::ForensicsRequest request;
    request.initial_evidence = {partial_write, wal, crash};
    request.crash_id = "crash-1";
    auto result = engine.investigate(request);
    if (!result) return 3;
    std::cout << result.value().to_json() << '\n';

    static_cast<void>(engine.shutdown());
    static_cast<void>(runtime.shutdown());
    return 0;
}
