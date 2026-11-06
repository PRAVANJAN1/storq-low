#include <storq/crash/crash_simulator.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace storq {
namespace {
std::string uuid_like(const char* prefix) {
    static std::atomic<std::uint64_t> counter{0};
    std::ostringstream out;
    out << prefix << '-' << std::hex << ++counter;
    return out.str();
}

StorqError invalid(std::string message) {
    return StorqError{ErrorCategory::General, ErrorCode::InvalidArgument, std::move(message), {}};
}
}

void ExecutionTrace::append(TraceRecord record) {
    std::lock_guard lock(mutex_);
    if (records_.size() >= limit_) {
        records_.erase(records_.begin());
    }
    records_.push_back(std::move(record));
}
void ExecutionTrace::set_limit(std::size_t limit) { std::lock_guard lock(mutex_); limit_ = std::max<std::size_t>(1, limit); }
std::vector<TraceRecord> ExecutionTrace::records() const {
    std::lock_guard lock(mutex_);
    auto result = records_;
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
    return result;
}
std::optional<TraceRecord> ExecutionTrace::lookup(std::uint64_t sequence) const {
    std::lock_guard lock(mutex_);
    auto found = std::find_if(records_.begin(), records_.end(), [&](const auto& r) { return r.sequence == sequence; });
    return found == records_.end() ? std::nullopt : std::optional(*found);
}
std::vector<TraceRecord> ExecutionTrace::filter(StorageOperationType operation) const {
    std::lock_guard lock(mutex_);
    std::vector<TraceRecord> result;
    for (const auto& record : records_) if (record.operation == operation) result.push_back(record);
    return result;
}
std::map<StorageOperationType, std::size_t> ExecutionTrace::statistics() const {
    std::lock_guard lock(mutex_);
    std::map<StorageOperationType, std::size_t> result;
    for (const auto& record : records_) ++result[record.operation];
    return result;
}
bool ExecutionTrace::validate() const {
    std::lock_guard lock(mutex_);
    std::uint64_t previous = 0;
    std::unordered_set<std::uint64_t> sequences;
    for (const auto& record : records_) {
        if (!sequences.insert(record.sequence).second || record.sequence < previous) return false;
        previous = record.sequence;
    }
    return true;
}
std::string ExecutionTrace::serialize() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    out << "{\"operations\":[";
    for (std::size_t i = 0; i < records_.size(); ++i) {
        if (i) out << ',';
        const auto& r = records_[i];
        out << "{\"sequence\":" << r.sequence << ",\"successful\":" << (r.successful ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}

OperationTracer::OperationTracer(std::size_t limit) { trace_.set_limit(limit); }
void OperationTracer::observe(const std::shared_ptr<StorageFile>& file) {
    static_cast<void>(file->add_observer(shared_from_this()));
}
void OperationTracer::on_operation_started(const StorageOperation& operation) {
    std::lock_guard lock(mutex_);
    TraceRecord record;
    record.operation = operation.type;
    record.resource_id = operation.resource_id;
    record.offset = operation.offset.value_or(0);
    record.requested_bytes = operation.requested_bytes;
    record.started_at = std::chrono::steady_clock::now();
    record.crash_eligible = operation.type == StorageOperationType::Write ||
                            operation.type == StorageOperationType::Append ||
                            operation.type == StorageOperationType::Sync ||
                            operation.type == StorageOperationType::Truncate;
    pending_[operation.operation_id] = record;
}
void OperationTracer::on_operation_completed(const StorageOperation& operation) {
    std::lock_guard lock(mutex_);
    static std::atomic<std::uint64_t> sequence{0};
    auto found = pending_.find(operation.operation_id);
    TraceRecord record = found == pending_.end() ? TraceRecord{} : found->second;
    record.sequence = ++sequence;
    record.operation = operation.type;
    record.resource_id = operation.resource_id;
    record.offset = operation.offset.value_or(0);
    record.requested_bytes = operation.requested_bytes;
    record.actual_bytes = operation.actual_bytes;
    record.successful = operation.succeeded;
    record.persistence_state = operation.succeeded ? PersistenceState::AcceptedByBackend : PersistenceState::PersistenceUncertain;
    record.completed_at = std::chrono::steady_clock::now();
    if (found != pending_.end()) record.started_at = found->second.started_at;
    trace_.append(record);
    pending_.erase(operation.operation_id);
}
ExecutionTrace OperationTracer::trace() const { return trace_; }
std::vector<std::uint64_t> OperationTracer::pending_sequences() const { return {}; }

void PersistenceModel::submitted(std::uint64_t sequence, const std::string& resource, std::uint64_t offset, std::uint64_t bytes) {
    std::lock_guard lock(mutex_); writes_[sequence] = {sequence, resource, offset, bytes, PersistenceState::Submitted};
}
void PersistenceModel::accepted(std::uint64_t sequence) {
    std::lock_guard lock(mutex_);
    if (writes_.contains(sequence)) writes_[sequence].state = PersistenceState::AcceptedByBackend;
}
void PersistenceModel::synchronized(std::uint64_t sequence) {
    std::lock_guard lock(mutex_);
    if (writes_.contains(sequence)) writes_[sequence].state = PersistenceState::Synchronized;
}
void PersistenceModel::uncertain(std::uint64_t sequence) {
    std::lock_guard lock(mutex_);
    if (writes_.contains(sequence)) writes_[sequence].state = PersistenceState::PersistenceUncertain;
}
void PersistenceModel::lost(std::uint64_t sequence) {
    std::lock_guard lock(mutex_);
    if (writes_.contains(sequence)) writes_[sequence].state = PersistenceState::LostInSimulation;
}
void PersistenceModel::reconstruct_after_crash(bool discard_unsynchronized_native_writes) {
    std::lock_guard lock(mutex_);
    for (auto& [sequence, write] : writes_) {
        if (write.state == PersistenceState::Synchronized) continue;
        write.state = discard_unsynchronized_native_writes ? PersistenceState::LostInSimulation : PersistenceState::PersistenceUncertain;
    }
}
PersistenceState PersistenceModel::state(std::uint64_t sequence) const {
    std::lock_guard lock(mutex_);
    auto found = writes_.find(sequence);
    return found == writes_.end() ? PersistenceState::NotSubmitted : found->second.state;
}
std::vector<PersistenceWrite> PersistenceModel::writes() const {
    std::lock_guard lock(mutex_);
    std::vector<PersistenceWrite> result;
    for (const auto& [sequence, write] : writes_) result.push_back(write);
    return result;
}
std::uint64_t PersistenceModel::count(PersistenceState state) const {
    std::lock_guard lock(mutex_);
    std::uint64_t result = 0;
    for (const auto& [sequence, write] : writes_) if (write.state == state) ++result;
    return result;
}

SimulatedProcessState CrashController::state() const noexcept {
    std::lock_guard lock(mutex_); return state_;
}
Result<void> CrashController::crash(std::uint64_t operation_sequence) {
    std::lock_guard lock(mutex_);
    if (state_ == SimulatedProcessState::Crashed || state_ == SimulatedProcessState::Crashing)
        return invalid("The simulated process has already crashed.");
    state_ = SimulatedProcessState::Crashing;
    interrupted_.push_back(operation_sequence);
    state_ = SimulatedProcessState::Crashed;
    return Result<void>();
}
Result<void> CrashController::restart() {
    std::lock_guard lock(mutex_);
    if (state_ != SimulatedProcessState::Crashed) return invalid("The simulated process is not crashed.");
    state_ = SimulatedProcessState::Restarting;
    state_ = SimulatedProcessState::Running;
    return Result<void>();
}
Result<void> CrashController::recover() {
    std::lock_guard lock(mutex_);
    if (state_ != SimulatedProcessState::Running) return invalid("Recovery may only begin from a running state.");
    state_ = SimulatedProcessState::Recovered;
    return Result<void>();
}
std::vector<std::uint64_t> CrashController::interrupted_operations() const {
    std::lock_guard lock(mutex_); return interrupted_;
}

Result<void> validate_crash_scenario(const CrashScenario& scenario) {
    if (scenario.name.empty()) return invalid("Crash scenario name must not be empty.");
    if (scenario.maximum_triggers == 0) return invalid("maximum_triggers must be positive.");
    const auto trigger = scenario.trigger.type;
    if ((trigger == CrashTriggerType::OperationSequence) && !scenario.trigger.operation_sequence) return invalid("Operation-sequence trigger requires a sequence.");
    if ((trigger == CrashTriggerType::OperationType) && !scenario.trigger.operation) return invalid("Operation-type trigger requires an operation.");
    if ((trigger == CrashTriggerType::ByteOffset) && !scenario.trigger.byte_offset) return invalid("Byte-offset trigger requires an offset.");
    if ((trigger == CrashTriggerType::PageNumber) && !scenario.trigger.page_number) return invalid("Page-number trigger requires a page.");
    if ((trigger == CrashTriggerType::WALFrameNumber) && !scenario.trigger.wal_frame_number) return invalid("WAL-frame trigger requires a frame number.");
    return Result<void>();
}

Result<void> CrashScheduler::register_scenario(const CrashScenario& scenario) {
    if (auto result = validate_crash_scenario(scenario); !result) return result;
    std::lock_guard lock(mutex_);
    auto copy = scenario;
    if (copy.id.empty()) copy.id = uuid_like("scenario");
    if (scenarios_.contains(copy.id)) return invalid("A crash scenario with this ID is already registered.");
    scenarios_[copy.id] = copy;
    statuses_[copy.id] = CrashStatus::Scheduled;
    return Result<void>();
}
Result<void> CrashScheduler::arm(const std::string& scenario_id) {
    std::lock_guard lock(mutex_);
    if (!scenarios_.contains(scenario_id)) return invalid("Unknown crash scenario.");
    statuses_[scenario_id] = CrashStatus::Armed;
    return Result<void>();
}
Result<void> CrashScheduler::cancel(const std::string& scenario_id) {
    std::lock_guard lock(mutex_);
    if (!scenarios_.contains(scenario_id)) return invalid("Unknown crash scenario.");
    statuses_[scenario_id] = CrashStatus::Cancelled;
    return Result<void>();
}
Result<CrashScenario> CrashScheduler::match(const ExecutionTrace& trace) const {
    std::lock_guard lock(mutex_);
    const auto records = trace.records();
    if (records.empty()) return invalid("Execution trace is empty.");
    std::vector<std::pair<int, CrashScenario>> matches;
    for (const auto& [id, scenario] : scenarios_) {
        if (!scenario.enabled || statuses_.at(id) != CrashStatus::Armed) continue;
        if (triggered_.contains(id) && triggered_.at(id) >= scenario.maximum_triggers) continue;
        for (const auto& record : records) {
            bool matched = false;
            switch (scenario.trigger.type) {
                case CrashTriggerType::OperationSequence: matched = record.sequence == scenario.trigger.operation_sequence; break;
                case CrashTriggerType::OperationType: matched = record.operation == scenario.trigger.operation; break;
                case CrashTriggerType::ByteOffset: matched = record.offset == scenario.trigger.byte_offset; break;
                case CrashTriggerType::OperationCount: matched = record.sequence == scenario.trigger.operation_count; break;
                case CrashTriggerType::ExplicitTrigger: matched = true; break;
                case CrashTriggerType::PageNumber: matched = record.offset / 4096 + 1 == scenario.trigger.page_number; break;
                case CrashTriggerType::WALFrameNumber: matched = scenario.target_wal_frame && record.offset / 32 + 1 == scenario.target_wal_frame; break;
                case CrashTriggerType::TransactionBoundary: matched = !scenario.transaction_boundary || record.resource_id.find(*scenario.transaction_boundary) != std::string::npos; break;
                case CrashTriggerType::CheckpointBoundary: matched = !scenario.checkpoint_boundary || record.resource_id.find(*scenario.checkpoint_boundary) != std::string::npos; break;
            }
            if (matched && (scenario.target_resource.empty() || scenario.target_resource == record.resource_id)) {
                matches.emplace_back(scenario.priority, scenario);
                break;
            }
        }
    }
    if (matches.empty()) return invalid("No armed crash scenario matches the execution trace.");
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second.id < b.second.id;
    });
    return matches.front().second;
}
std::vector<CrashScenario> CrashScheduler::scenarios() const {
    std::lock_guard lock(mutex_);
    std::vector<CrashScenario> result;
    for (const auto& [id, scenario] : scenarios_) result.push_back(scenario);
    return result;
}
std::vector<CrashExecution> CrashScheduler::history() const {
    std::lock_guard lock(mutex_); return history_;
}

Result<void> CrashReplayEngine::validate(const ExecutionTrace& initial, const ExecutionTrace& replay) const {
    if (!initial.validate() || !replay.validate()) return invalid("Replay trace validation failed.");
    const auto a = initial.records(), b = replay.records();
    if (a.size() != b.size()) return invalid("Replay operation count differs from the initial trace.");
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].sequence != b[i].sequence || a[i].operation != b[i].operation || a[i].resource_id != b[i].resource_id)
            return invalid("Replay trace diverged from the initial operation sequence.");
    }
    return Result<void>();
}
Result<void> CrashReplayEngine::replay(const CrashScenario& scenario, ExecutionTrace& trace) const {
    if (trace.records().empty()) return invalid("Cannot replay an empty crash trace.");
    if (!trace.validate() || !scenario.enabled) return invalid("Replay prerequisites are invalid.");
    return Result<void>();
}
std::vector<std::string> CrashReplayEngine::divergence() const {
    std::lock_guard lock(mutex_); return divergence_;
}

void CrashHistory::record(CrashExecution execution) {
    std::lock_guard lock(mutex_); executions_.push_back(std::move(execution));
}
void CrashHistory::record_snapshot(const CrashStateSnapshot& snapshot) {
    std::lock_guard lock(mutex_); snapshots_.push_back(snapshot);
}
std::vector<CrashExecution> CrashHistory::executions() const {
    std::lock_guard lock(mutex_); return executions_;
}
std::vector<CrashStateSnapshot> CrashHistory::snapshots() const {
    std::lock_guard lock(mutex_); return snapshots_;
}
std::map<CrashType, std::uint64_t> CrashHistory::statistics() const {
    std::lock_guard lock(mutex_);
    std::map<CrashType, std::uint64_t> result;
    for (const auto& execution : executions_) ++result[execution.type];
    return result;
}
std::string CrashHistory::serialize() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    out << "{\"crashes\":[";
    for (std::size_t i = 0; i < executions_.size(); ++i) {
        if (i) out << ',';
        out << "{\"crash_id\":\"" << executions_[i].crash_id << "\",\"status\":" << static_cast<int>(executions_[i].status) << '}';
    }
    return out.str() + "]}";
}

CrashSimulator::CrashSimulator(Runtime& runtime) : runtime_(runtime), tracer_(std::make_shared<OperationTracer>(4096)) {}
CrashSimulator::~CrashSimulator() { if (initialized_.load()) static_cast<void>(shutdown()); }
std::string_view CrashSimulator::name() const noexcept { return "CrashSimulator"; }
Result<void> CrashSimulator::initialize() {
    static_cast<void>(runtime_.capabilities().register_capability("crash-simulation"));
    initialized_ = true;
    return Result<void>();
}
Result<void> CrashSimulator::shutdown() { initialized_ = false; return Result<void>(); }
bool CrashSimulator::is_initialized() const noexcept { return initialized_.load(); }
Result<void> CrashSimulator::register_scenario(const CrashScenario& scenario) { return scheduler_.register_scenario(scenario); }
Result<void> CrashSimulator::arm(const std::string& scenario_id) { return scheduler_.arm(scenario_id); }
Result<void> CrashSimulator::cancel(const std::string& scenario_id) { return scheduler_.cancel(scenario_id); }
void CrashSimulator::observe(const std::shared_ptr<StorageFile>& file) { tracer_->observe(file); }

Result<CrashReport> CrashSimulator::execute(const CrashScenario& scenario, const std::shared_ptr<StorageFile>& file) {
    auto copy = scenario;
    if (copy.id.empty()) copy.id = uuid_like("scenario");
    if (auto result = register_scenario(copy); !result) return result.error();
    if (auto result = arm(copy.id); !result) return result.error();
    if (file) observe(file);
    return execute(file);
}
Result<CrashReport> CrashSimulator::execute(const std::shared_ptr<StorageFile>&) {
    if (!initialized_.load()) return invalid("CrashSimulator is not initialized.");
    ExecutionTrace records = tracer_->trace();
    simulation_trace_ = records;
    auto matched = scheduler_.match(records);
    if (!matched) return matched.error();
    CrashReport report;
    report.scenario_id = matched.value().id;
    report.crash_id = uuid_like("crash");
    report.crash_type = matched.value().type;
    report.process_state = SimulatedProcessState::Crashed;
    report.total_operations = records.records().size();
    report.relevant_operations = records.records();
    for (const auto& record : report.relevant_operations) if (record.successful) ++report.completed_operations;
    auto trigger_records = records.records();
    report.trigger_sequence = trigger_records.empty() ? 0 : trigger_records.back().sequence;
    report.trigger_operation = trigger_records.empty() ? StorageOperationType::Write : trigger_records.back().operation;
    report.interrupted_operations = 1;
    controller_.crash(report.trigger_sequence);
    persistence_.reconstruct_after_crash(false);
    report.uncertain_writes = persistence_.count(PersistenceState::PersistenceUncertain);
    report.lost_in_simulation_writes = persistence_.count(PersistenceState::LostInSimulation);
    report.synchronized_writes = persistence_.count(PersistenceState::Synchronized);
    CrashExecution execution;
    execution.crash_id = report.crash_id;
    execution.scenario_id = report.scenario_id;
    execution.status = CrashStatus::Completed;
    execution.type = report.crash_type;
    execution.trigger_sequence = report.trigger_sequence;
    execution.started_at = execution.completed_at = std::chrono::system_clock::now();
    history_.record(execution);
    return report;
}
const ExecutionTrace& CrashSimulator::trace() const noexcept { return simulation_trace_; }
OperationTracer& CrashSimulator::tracer() noexcept { return *tracer_; }
CrashController& CrashSimulator::controller() noexcept { return controller_; }
PersistenceModel& CrashSimulator::persistence() noexcept { return persistence_; }
CrashScheduler& CrashSimulator::scheduler() noexcept { return scheduler_; }
CrashHistory& CrashSimulator::history() noexcept { return history_; }
Result<CrashStateSnapshot> CrashSimulator::snapshot(const CrashReport& report) {
    CrashStateSnapshot snapshot;
    snapshot.snapshot_id = uuid_like("snapshot");
    snapshot.crash_id = report.crash_id;
    snapshot.scenario_id = report.scenario_id;
    snapshot.operation_sequence = report.trigger_sequence;
    snapshot.process_state = controller_.state();
    snapshot.timestamp = std::chrono::system_clock::now();
    snapshot.integrity = uuid_like("sha256");
    snapshot.complete = report.total_operations > 0;
    history_.record_snapshot(snapshot);
    snapshots_[snapshot.snapshot_id] = snapshot;
    return snapshot;
}

}
