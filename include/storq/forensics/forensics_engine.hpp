#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <storq/common/result.hpp>
#include <storq/core/module.hpp>
#include <storq/core/runtime.hpp>
#include <storq/crash/crash_simulator.hpp>
#include <storq/fault/fault_injector.hpp>
#include <storq/recovery/recovery_engine.hpp>

namespace storq {

enum class EvidenceType {
    StorageOperation, InjectedFault, CrashEvent, PersistenceTransition, PageAnalysis,
    WalAnalysis, TransactionOutcome, RecoveryFinding, StructuralAnomaly,
    ExecutionDiagnostic, SnapshotReference
};

struct Evidence {
    std::string evidence_id;
    EvidenceType type{EvidenceType::ExecutionDiagnostic};
    std::string source_module;
    std::string source_reference;
    std::chrono::system_clock::time_point timestamp{};
    std::optional<std::uint64_t> operation_sequence;
    std::optional<std::uint64_t> page_number;
    std::optional<std::uint64_t> wal_frame_number;
    std::string transaction_id;
    std::string crash_id;
    std::string description;
    std::unordered_map<std::string, std::string> attributes;
};

struct EvidenceQuery {
    std::optional<std::string> evidence_id;
    std::optional<EvidenceType> type;
    std::optional<std::string> source_module;
    std::optional<std::uint64_t> operation_sequence;
    std::optional<std::uint64_t> page_number;
    std::optional<std::uint64_t> wal_frame_number;
    std::optional<std::string> transaction_id;
    std::optional<std::string> crash_id;
    std::size_t limit{1000};
};

class IEvidenceRepository {
public:
    virtual ~IEvidenceRepository() = default;
    virtual Result<void> add(const Evidence& evidence) = 0;
    virtual Result<std::vector<Evidence>> query(const EvidenceQuery& query) const = 0;
    virtual Result<Evidence> get(const std::string& evidence_id) const = 0;
};

class EvidenceRepository final : public IEvidenceRepository {
public:
    Result<void> add(const Evidence& evidence) override;
    Result<std::vector<Evidence>> query(const EvidenceQuery& query) const override;
    Result<Evidence> get(const std::string& evidence_id) const override;

private:
    mutable std::mutex mutex_;
    std::vector<Evidence> evidence_;
    std::unordered_map<std::string, std::size_t> identifiers_;
};

enum class FailureCategory {
    StorageFailure, SynchronizationFailure, WalCorruption, PageCorruption,
    TransactionAtomicityFailure, TransactionDurabilityFailure, CheckpointFailure,
    RecoveryFailure, StructuralFailure, LogicalConsistencyFailure, UnknownFailure
};

struct TimelineEvent {
    std::string event_id;
    std::string event_type;
    std::chrono::system_clock::time_point timestamp{};
    std::optional<std::uint64_t> operation_sequence;
    std::string source_module;
    std::string description;
    std::vector<std::string> evidence_ids;
    bool ordering_uncertain{false};
};

struct TimelineResult {
    std::vector<TimelineEvent> events;
};

class TimelineBuilder {
public:
    [[nodiscard]] TimelineResult build(const std::vector<Evidence>& evidence) const;
};

struct EvidenceSet {
    std::vector<Evidence> evidence;
};

struct CorrelationResult {
    std::string rule_name;
    std::vector<std::string> evidence_ids;
    double confidence{0.0};
    std::string explanation;
    bool causal_certainty{false};
    std::vector<std::string> missing_support;
};

class ICorrelationRule {
public:
    virtual ~ICorrelationRule() = default;
    virtual std::string name() const = 0;
    virtual CorrelationResult evaluate(const EvidenceSet& evidence) const = 0;
};

class EvidenceCorrelationEngine {
public:
    void register_rule(std::shared_ptr<ICorrelationRule> rule);
    [[nodiscard]] std::vector<CorrelationResult> correlate(const EvidenceSet& evidence) const;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ICorrelationRule>> rules_;
};

struct FailureClassification {
    FailureCategory category{FailureCategory::UnknownFailure};
    std::string description;
    std::vector<std::string> supporting_evidence_ids;
    std::vector<std::string> contradicting_evidence_ids;
    double confidence{0.0};
};

class FailureClassificationEngine {
public:
    [[nodiscard]] std::vector<FailureClassification> classify(const EvidenceSet& evidence) const;
};

struct ForensicFinding {
    std::string finding_id;
    std::string title;
    std::string category;
    int severity{1};
    std::string description;
    std::vector<std::string> evidence_ids;
    std::vector<std::string> timeline_event_ids;
    std::optional<std::uint64_t> page_number;
    std::optional<std::uint64_t> wal_frame_number;
    std::string transaction_id;
    double confidence{0.0};
    std::string recommended_action;
};

class FindingsManager {
public:
    void add(ForensicFinding finding);
    [[nodiscard]] std::vector<ForensicFinding> findings() const;
    [[nodiscard]] std::map<int, std::vector<ForensicFinding>> grouped() const;

private:
    mutable std::mutex mutex_;
    std::vector<ForensicFinding> findings_;
};

struct ConfidenceRationale {
    double score{0.0};
    std::vector<std::string> reasons;
};

class EvidenceConfidenceEvaluator {
public:
    [[nodiscard]] ConfidenceRationale evaluate(const EvidenceSet& evidence,
                                               const CorrelationResult* correlation = nullptr) const;
};

struct RootCauseHypothesis {
    std::string hypothesis_id;
    FailureCategory category{FailureCategory::UnknownFailure};
    std::string title;
    std::string explanation;
    std::vector<std::string> supporting_evidence_ids;
    std::vector<std::string> contradicting_evidence_ids;
    double confidence_score{0.0};
    bool requires_manual_confirmation{true};
};

class RootCauseHypothesisEngine {
public:
    [[nodiscard]] std::vector<RootCauseHypothesis> generate(
        const EvidenceSet& evidence, const std::vector<CorrelationResult>& correlations) const;
};

class PageForensicsAnalyzer {
public:
    [[nodiscard]] std::vector<ForensicFinding> analyze(const EvidenceSet& evidence) const;
};

class WALForensicsAnalyzer {
public:
    [[nodiscard]] std::vector<ForensicFinding> analyze(const EvidenceSet& evidence) const;
};

class TransactionFailureAnalyzer {
public:
    [[nodiscard]] std::vector<ForensicFinding> analyze(const EvidenceSet& evidence) const;
};

struct Anomaly {
    std::string name;
    int severity{1};
    std::string explanation;
    std::vector<std::string> evidence_ids;
};

class AnomalyDetector {
public:
    void register_rule(std::string name, std::function<bool(const EvidenceSet&)> rule,
                       int severity, std::string explanation);
    [[nodiscard]] std::vector<Anomaly> detect(const EvidenceSet& evidence) const;

private:
    struct Rule { std::string name; std::function<bool(const EvidenceSet&)> predicate; int severity; std::string explanation; };
    mutable std::mutex mutex_;
    std::vector<Rule> rules_;
};

struct ForensicsConfig { std::size_t evidence_limit{10000}; bool require_complete_evidence{false}; };

struct ForensicsRequest {
    std::vector<Evidence> initial_evidence;
    std::optional<std::string> crash_id;
    const FaultHistory* fault_history{nullptr};
    const CrashHistory* crash_history{nullptr};
    const CrashSimulator* crash_simulator{nullptr};
    const RecoveryVerificationReport* recovery_report{nullptr};
};

struct ForensicsResult {
    std::string investigation_id;
    bool complete{false};
    TimelineResult timeline;
    std::vector<CorrelationResult> correlations;
    std::vector<FailureClassification> classifications;
    std::vector<Anomaly> anomalies;
    std::vector<RootCauseHypothesis> hypotheses;
    std::vector<ForensicFinding> findings;
    std::vector<std::string> available_sources;
    std::vector<std::string> missing_sources;
    std::vector<std::string> limitations;
    std::vector<std::string> stage_errors;
    [[nodiscard]] std::string to_json() const;
};

class ForensicsEngine final : public IModule {
public:
    explicit ForensicsEngine(Runtime& runtime, ForensicsConfig config = {});
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Result<void> shutdown() override;
    [[nodiscard]] bool is_initialized() const noexcept override;
    [[nodiscard]] Result<ForensicsResult> investigate(const ForensicsRequest& request);
    [[nodiscard]] EvidenceRepository& repository() noexcept;

private:
    Runtime& runtime_;
    ForensicsConfig config_;
    std::atomic<bool> initialized_{false};
    EvidenceRepository repository_;
};

[[nodiscard]] std::string evidence_type_name(EvidenceType type) noexcept;
[[nodiscard]] Evidence make_evidence(EvidenceType type, std::string source_module,
                                     std::string source_reference, std::string description);

}
