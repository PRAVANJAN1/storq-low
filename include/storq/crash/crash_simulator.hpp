#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <storq/common/result.hpp>
#include <storq/core/module.hpp>
#include <storq/core/runtime.hpp>
#include <storq/storage/storage.hpp>

namespace storq {

enum class CrashType { ProcessTermination, ImmediateTermination, WriteInterruption, WALWriteInterruption,
                       SyncInterruption, CheckpointInterruption, CommitInterruption,
                       RecoveryInterruption, ScheduledCrash };
enum class CrashStatus { Scheduled, Armed, Triggered, Executing, Completed, Failed, Cancelled };
enum class CrashTriggerType { OperationCount, OperationType, OperationSequence, ByteOffset, PageNumber,
                              WALFrameNumber, TransactionBoundary, CheckpointBoundary, ExplicitTrigger };
enum class PersistenceState { NotSubmitted, Submitted, AcceptedByBackend, Synchronized,
                              PersistenceUncertain, LostInSimulation };
enum class SimulatedProcessState { Running, Crashing, Crashed, Restarting, Recovered, Failed };

struct CrashTrigger {
    CrashTriggerType type{CrashTriggerType::ExplicitTrigger};
    std::optional<StorageOperationType> operation;
    std::optional<std::uint64_t> operation_count;
    std::optional<std::uint64_t> operation_sequence;
    std::optional<std::uint64_t> byte_offset;
    std::optional<std::uint64_t> page_number;
    std::optional<std::uint64_t> wal_frame_number;
    std::optional<std::string> transaction_boundary;
    std::optional<std::string> checkpoint_boundary;
};

struct CrashScenario {
    std::string id;
    std::string name;
    CrashType type{CrashType::ScheduledCrash};
    CrashTrigger trigger;
    std::string target_resource;
    std::uint64_t target_sequence{0};
    std::optional<std::uint64_t> target_page;
    std::optional<std::uint64_t> target_wal_frame;
    std::optional<std::string> transaction_boundary;
    std::optional<std::string> checkpoint_boundary;
    std::uint64_t maximum_triggers{1};
    std::uint64_t execution_seed{0};
    bool enabled{true};
    int priority{0};
};

struct TraceRecord {
    std::uint64_t sequence{0};
    StorageOperationType operation{StorageOperationType::Read};
    std::string resource_id;
    std::uint64_t offset{0};
    std::uint64_t requested_bytes{0};
    std::uint64_t actual_bytes{0};
    bool successful{false};
    PersistenceState persistence_state{PersistenceState::NotSubmitted};
    bool crash_eligible{false};
    std::vector<std::string> fault_ids;
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::steady_clock::time_point completed_at{};
};

class ExecutionTrace {
public:
    ExecutionTrace() = default;
    ExecutionTrace(const ExecutionTrace& other) {
        std::lock_guard lock(other.mutex_);
        records_ = other.records_;
        limit_ = other.limit_;
    }
    ExecutionTrace& operator=(const ExecutionTrace& other) {
        if (this != &other) {
            std::lock_guard lhs(mutex_);
            std::lock_guard rhs(other.mutex_);
            records_ = other.records_;
            limit_ = other.limit_;
        }
        return *this;
    }
    void append(TraceRecord record);
    void set_limit(std::size_t limit);
    [[nodiscard]] std::vector<TraceRecord> records() const;
    [[nodiscard]] std::optional<TraceRecord> lookup(std::uint64_t sequence) const;
    [[nodiscard]] std::vector<TraceRecord> filter(StorageOperationType operation) const;
    [[nodiscard]] std::map<StorageOperationType, std::size_t> statistics() const;
    [[nodiscard]] bool validate() const;
    [[nodiscard]] std::string serialize() const;

private:
    mutable std::mutex mutex_;
    std::vector<TraceRecord> records_;
    std::size_t limit_{4096};
};

class OperationTracer final : public IStorageObserver, public std::enable_shared_from_this<OperationTracer> {
public:
    explicit OperationTracer(std::size_t limit = 4096);
    void observe(const std::shared_ptr<StorageFile>& file);
    void on_operation_started(const StorageOperation& operation) override;
    void on_operation_completed(const StorageOperation& operation) override;
    [[nodiscard]] ExecutionTrace trace() const;
    [[nodiscard]] std::vector<std::uint64_t> pending_sequences() const;

private:
    mutable std::mutex mutex_;
    ExecutionTrace trace_;
    std::map<std::string, TraceRecord> pending_;
};

struct PersistenceWrite {
    std::uint64_t operation_sequence{0};
    std::string resource_id;
    std::uint64_t offset{0};
    std::uint64_t bytes{0};
    PersistenceState state{PersistenceState::Submitted};
};

class PersistenceModel {
public:
    void submitted(std::uint64_t sequence, const std::string& resource, std::uint64_t offset, std::uint64_t bytes);
    void accepted(std::uint64_t sequence);
    void synchronized(std::uint64_t sequence);
    void uncertain(std::uint64_t sequence);
    void lost(std::uint64_t sequence);
    void reconstruct_after_crash(bool discard_unsynchronized_native_writes);
    [[nodiscard]] PersistenceState state(std::uint64_t sequence) const;
    [[nodiscard]] std::vector<PersistenceWrite> writes() const;
    [[nodiscard]] std::uint64_t count(PersistenceState state) const;

private:
    mutable std::mutex mutex_;
    std::map<std::uint64_t, PersistenceWrite> writes_;
};

struct CrashExecution {
    std::string crash_id;
    std::string scenario_id;
    CrashStatus status{CrashStatus::Scheduled};
    CrashType type{CrashType::ScheduledCrash};
    std::uint64_t trigger_sequence{0};
    std::chrono::system_clock::time_point started_at{};
    std::chrono::system_clock::time_point completed_at{};
};

struct CrashStateSnapshot {
    std::string snapshot_id;
    std::string crash_id;
    std::string scenario_id;
    std::uint64_t operation_sequence{0};
    std::string resource_id;
    SimulatedProcessState process_state{SimulatedProcessState::Crashed};
    std::chrono::system_clock::time_point timestamp{};
    std::string integrity;
    bool complete{false};
};

class CrashController {
public:
    [[nodiscard]] SimulatedProcessState state() const noexcept;
    Result<void> crash(std::uint64_t operation_sequence);
    Result<void> restart();
    Result<void> recover();
    [[nodiscard]] std::vector<std::uint64_t> interrupted_operations() const;

private:
    mutable std::mutex mutex_;
    SimulatedProcessState state_{SimulatedProcessState::Running};
    std::vector<std::uint64_t> interrupted_;
};

class CrashScheduler {
public:
    Result<void> register_scenario(const CrashScenario& scenario);
    Result<void> arm(const std::string& scenario_id);
    Result<void> cancel(const std::string& scenario_id);
    [[nodiscard]] Result<CrashScenario> match(const ExecutionTrace& trace) const;
    [[nodiscard]] std::vector<CrashScenario> scenarios() const;
    [[nodiscard]] std::vector<CrashExecution> history() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, CrashScenario> scenarios_;
    std::map<std::string, CrashStatus> statuses_;
    std::map<std::string, std::uint64_t> triggered_;
    std::vector<CrashExecution> history_;
};

class CrashReplayEngine {
public:
    Result<void> validate(const ExecutionTrace& initial, const ExecutionTrace& replay) const;
    Result<void> replay(const CrashScenario& scenario, ExecutionTrace& trace) const;
    [[nodiscard]] std::vector<std::string> divergence() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> divergence_;
};

struct CrashReport {
    std::string crash_id;
    std::string scenario_id;
    CrashType crash_type{CrashType::ScheduledCrash};
    std::uint64_t trigger_sequence{0};
    StorageOperationType trigger_operation{StorageOperationType::Write};
    SimulatedProcessState process_state{SimulatedProcessState::Crashed};
    std::uint64_t total_operations{0};
    std::uint64_t completed_operations{0};
    std::uint64_t interrupted_operations{0};
    std::uint64_t synchronized_writes{0};
    std::uint64_t uncertain_writes{0};
    std::uint64_t lost_in_simulation_writes{0};
    bool snapshot_created{false};
    std::vector<TraceRecord> relevant_operations;
};

class CrashHistory {
public:
    void record(CrashExecution execution);
    void record_snapshot(const CrashStateSnapshot& snapshot);
    [[nodiscard]] std::vector<CrashExecution> executions() const;
    [[nodiscard]] std::vector<CrashStateSnapshot> snapshots() const;
    [[nodiscard]] std::map<CrashType, std::uint64_t> statistics() const;
    [[nodiscard]] std::string serialize() const;

private:
    mutable std::mutex mutex_;
    std::vector<CrashExecution> executions_;
    std::vector<CrashStateSnapshot> snapshots_;
};

class CrashSimulator final : public IModule {
public:
    explicit CrashSimulator(Runtime& runtime);
    ~CrashSimulator() override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Result<void> shutdown() override;
    [[nodiscard]] bool is_initialized() const noexcept override;

    [[nodiscard]] Result<void> register_scenario(const CrashScenario& scenario);
    [[nodiscard]] Result<void> arm(const std::string& scenario_id);
    [[nodiscard]] Result<void> cancel(const std::string& scenario_id);
    void observe(const std::shared_ptr<StorageFile>& file);
    [[nodiscard]] Result<CrashReport> execute(const std::shared_ptr<StorageFile>& file);
    [[nodiscard]] Result<CrashReport> execute(const CrashScenario& scenario,
                                              const std::shared_ptr<StorageFile>& file);
    [[nodiscard]] const ExecutionTrace& trace() const noexcept;
    [[nodiscard]] OperationTracer& tracer() noexcept;
    [[nodiscard]] CrashController& controller() noexcept;
    [[nodiscard]] PersistenceModel& persistence() noexcept;
    [[nodiscard]] CrashScheduler& scheduler() noexcept;
    [[nodiscard]] CrashHistory& history() noexcept;
    [[nodiscard]] Result<CrashStateSnapshot> snapshot(const CrashReport& report);

private:
    Runtime& runtime_;
    std::atomic<bool> initialized_{false};
    std::shared_ptr<OperationTracer> tracer_{std::make_shared<OperationTracer>(4096)};
    ExecutionTrace simulation_trace_;
    mutable std::mutex mutex_;
    CrashController controller_;
    PersistenceModel persistence_;
    CrashScheduler scheduler_;
    CrashHistory history_;
    std::map<std::string, CrashStateSnapshot> snapshots_;
};

[[nodiscard]] Result<void> validate_crash_scenario(const CrashScenario& scenario);

}
