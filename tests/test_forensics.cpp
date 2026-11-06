#include <storq/storq.hpp>

#include <cassert>
#include <iostream>

int main() {
    storq::Runtime runtime;
    assert(runtime.initialize());
    storq::ForensicsEngine engine(runtime);
    assert(engine.initialize());

    storq::Evidence fault = storq::make_evidence(
        storq::EvidenceType::InjectedFault, "Module6", "fault-sync-1",
        "Injected synchronization failure.");
    fault.operation_sequence = 12;
    assert(engine.repository().add(fault));

    storq::Evidence crash = storq::make_evidence(
        storq::EvidenceType::CrashEvent, "Module7", "crash-1",
        "Crash occurred after failed synchronization.");
    crash.operation_sequence = 13;
    crash.crash_id = "crash-1";
    assert(engine.repository().add(crash));

    storq::EvidenceQuery query;
    query.type = storq::EvidenceType::InjectedFault;
    auto fault_evidence = engine.repository().query(query);
    assert(fault_evidence && fault_evidence.value().size() == 1);

    storq::ForensicsRequest request;
    request.initial_evidence = {fault, crash};
    auto investigation = engine.investigate(request);
    assert(investigation);
    assert(investigation.value().timeline.events.size() == 2);
    assert(!investigation.value().classifications.empty());
    assert(!investigation.value().to_json().empty());

    storq::EvidenceQuery missing;
    missing.evidence_id = "missing";
    assert(engine.repository().query(missing));

    assert(engine.shutdown());
    assert(runtime.shutdown());
    std::cout << "Forensics tests passed\n";
    return 0;
}
