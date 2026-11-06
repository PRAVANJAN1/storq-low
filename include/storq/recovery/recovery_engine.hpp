#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <storq/common/result.hpp>
#include <storq/core/module.hpp>
#include <storq/core/runtime.hpp>
#include <storq/crash/crash_simulator.hpp>
#include <storq/format/format_inspector.hpp>

namespace storq {

enum class RecoveryExecutionMode { OpenAndInspect, RecoverReadOnly, RepeatRecovery };
enum class RecoveryExecutionStatus { NotStarted, Running, Completed, Failed, TimedOut, Cancelled };
enum class ExpectedTransactionState { Began, Committed, RolledBack, Incomplete, Unknown };
enum class TransactionVerificationStatus { Survived, Missing, UnexpectedlyPersisted, PartiallyApplied,
                                           RolledBackCorrectly, Uncertain, Inconclusive, Error };
enum class DurabilityStatus { DurableAndPreserved, ExpectedButMissing, PreservedWithoutDurabilityGuarantee,
                              NotExpectedToSurvive, Uncertain, Inconclusive };
enum class RecoverySeverity { Info, Warning, Error, Critical };
enum class InvariantStatus { Passed, Failed, Inconclusive };
enum class StructuralValidationStatus { Passed, Failed, Inconclusive, Error };
enum class RepeatabilityStatus { Passed, Failed, Inconclusive, Error };
enum class RecoveryVerificationStatus { Passed, Failed, Inconclusive, Error };

struct RecoveryRequest {
    std::filesystem::path database_path;
    std::filesystem::path isolated_workspace;
    RecoveryExecutionMode mode{RecoveryExecutionMode::OpenAndInspect};
    bool repeated_verification{false};
    std::chrono::milliseconds timeout{30000};
    bool read_only_inspection{true};
};

struct RecoveryExecutionResult {
    RecoveryExecutionStatus status{RecoveryExecutionStatus::NotStarted};
    bool completed{false};
    bool database_opened{false};
    bool suitable_for_inspection{false};
    std::chrono::milliseconds duration{0};
    std::filesystem::path database_path;
    std::vector<std::string> diagnostics;
    std::vector<std::string> errors;
};

class IRecoveryExecutor {
public:
    virtual ~IRecoveryExecutor() = default;
    virtual RecoveryExecutionResult execute(const RecoveryRequest& request) = 0;
};

struct ExpectedTransaction {
    std::string transaction_id;
    ExpectedTransactionState expected_state{ExpectedTransactionState::Unknown};
    std::uint64_t begin_sequence{0};
    std::optional<std::uint64_t> commit_sequence;
    std::vector<std::string> affected_entities;
    bool commit_was_synchronized{false};
    bool durability_expected{false};
    std::unordered_map<std::string, std::string> expected_values;
    std::unordered_map<std::string, std::string> metadata;
};

class IExpectedTransactionHistory {
public:
    virtual ~IExpectedTransactionHistory() = default;
    virtual std::vector<ExpectedTransaction> get_transactions() const = 0;
};

struct StaticTransactionHistory final : IExpectedTransactionHistory {
    std::vector<ExpectedTransaction> transactions;
    [[nodiscard]] std::vector<ExpectedTransaction> get_transactions() const override { return transactions; }
};

struct RecoveredTransaction {
    std::string transaction_id;
    bool transaction_effects_present{false};
    bool transaction_effects_complete{false};
    std::vector<std::string> observed_entities;
    std::unordered_map<std::string, std::string> observed_values;
};

struct RecoveredDatabaseState {
    bool database_opened{false};
    bool logical_inspection_completed{false};
    bool structural_validation_completed{false};
    std::vector<RecoveredTransaction> transactions;
    std::uint64_t observed_record_count{0};
    std::unordered_map<std::string, std::string> metadata;
};

class IRecoveredStateInspector {
public:
    virtual ~IRecoveredStateInspector() = default;
    virtual RecoveredDatabaseState inspect(const std::filesystem::path& database_path) = 0;
};

struct StructuralValidationResult {
    StructuralValidationStatus status{StructuralValidationStatus::Inconclusive};
    bool header_valid{false};
    bool structure_valid{false};
    bool wal_valid{true};
    std::vector<std::string> evidence;
};

struct RepeatabilityResult {
    RepeatabilityStatus status{RepeatabilityStatus::Inconclusive};
    std::vector<std::string> differences;
    std::vector<std::string> errors;
};

struct InvariantContext {
    const RecoveredDatabaseState* state{nullptr};
    std::unordered_map<std::string, std::string> metadata;
};

struct InvariantResult {
    std::string name;
    InvariantStatus status{InvariantStatus::Inconclusive};
    std::string description;
    std::string expected;
    std::string observed;
    std::vector<std::string> evidence;
    std::vector<std::string> errors;
};

class IConsistencyInvariant {
public:
    virtual ~IConsistencyInvariant() = default;
    virtual std::string name() const = 0;
    virtual InvariantResult evaluate(const InvariantContext& context) const = 0;
};

class ConsistencyInvariantRegistry {
public:
    void register_invariant(std::shared_ptr<IConsistencyInvariant> invariant);
    [[nodiscard]] std::vector<InvariantResult> evaluate(const InvariantContext& context) const;
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IConsistencyInvariant>> invariants_;
};

struct TransactionComparison {
    ExpectedTransaction expected;
    RecoveredTransaction observed;
    TransactionVerificationStatus status{TransactionVerificationStatus::Inconclusive};
    std::vector<std::string> evidence;
    bool matching_transaction_found{false};
};

class TransactionOutcomeComparator {
public:
    [[nodiscard]] std::vector<TransactionComparison> compare(
        const std::vector<ExpectedTransaction>& expected,
        const RecoveredDatabaseState& recovered) const;
};

struct AtomicityResult {
    std::string transaction_id;
    bool passed{false};
    InvariantStatus status{InvariantStatus::Inconclusive};
    std::string details;
};

class AtomicityVerifier {
public:
    [[nodiscard]] std::vector<AtomicityResult> verify(
        const std::vector<ExpectedTransaction>& expected,
        const RecoveredDatabaseState& recovered) const;
};

struct DurabilityResult {
    std::string transaction_id;
    DurabilityStatus status{DurabilityStatus::Inconclusive};
    std::string details;
};

class DurabilityVerifier {
public:
    [[nodiscard]] std::vector<DurabilityResult> verify(
        const std::vector<ExpectedTransaction>& expected,
        const RecoveredDatabaseState& recovered) const;
};

class StructuralRecoveryValidator {
public:
    explicit StructuralRecoveryValidator(Runtime& runtime);
    [[nodiscard]] StructuralValidationResult validate(const std::filesystem::path& database_path) const;

private:
    Runtime& runtime_;
};

class RecoveryRepeatabilityValidator {
public:
    [[nodiscard]] RepeatabilityResult validate(
        const std::filesystem::path& immutable_snapshot,
        std::size_t runs,
        IRecoveryExecutor& executor,
        IRecoveredStateInspector& inspector) const;
};

struct RecoveryFinding {
    std::string id;
    RecoverySeverity severity{RecoverySeverity::Warning};
    std::string category;
    std::string title;
    std::string description;
    std::string transaction_id;
    std::optional<std::uint64_t> page_number;
    std::optional<std::uint64_t> wal_frame_number;
    std::string expected;
    std::string observed;
    std::vector<std::string> evidence;
    std::string recommended_action;
};

class FindingManager {
public:
    void add(RecoveryFinding finding);
    [[nodiscard]] std::vector<RecoveryFinding> findings() const;
    void clear();
    [[nodiscard]] std::map<RecoverySeverity, std::vector<RecoveryFinding>> grouped() const;

private:
    mutable std::mutex mutex_;
    std::vector<RecoveryFinding> findings_;
};

struct RecoveryVerificationRequest {
    RecoveryRequest recovery;
    std::shared_ptr<IExpectedTransactionHistory> history;
    std::shared_ptr<IRecoveredStateInspector> inspector;
    std::shared_ptr<IRecoveryExecutor> executor;
    bool validate_repeatability{false};
    std::size_t repeatability_runs{3};
};

struct RecoveryVerificationReport {
    std::string verification_id;
    RecoveryVerificationStatus status{RecoveryVerificationStatus::Inconclusive};
    RecoveryExecutionStatus recovery_status{RecoveryExecutionStatus::NotStarted};
    DatabaseFormat database_format{DatabaseFormat::Unknown};
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point completed_at;
    std::chrono::milliseconds recovery_duration{0};
    std::size_t expected_transaction_count{0};
    std::size_t recovered_transaction_count{0};
    std::size_t missing_committed_count{0};
    std::size_t unexpected_transaction_count{0};
    std::size_t partially_applied_count{0};
    std::size_t rolled_back_count{0};
    std::size_t uncertain_transaction_count{0};
    std::vector<TransactionComparison> transactions;
    std::vector<AtomicityResult> atomicity;
    std::vector<DurabilityResult> durability;
    std::vector<InvariantResult> invariants;
    StructuralValidationResult structure;
    RepeatabilityResult repeatability;
    std::vector<RecoveryFinding> findings;
    std::vector<std::string> limitations;
    [[nodiscard]] std::string to_json() const;
};

struct RecoveryVerificationResult {
    RecoveryVerificationStatus status{RecoveryVerificationStatus::Inconclusive};
    RecoveryVerificationReport report;
    std::vector<std::string> stage_errors;
};

class RecoveryVerificationEngine final : public IModule {
public:
    explicit RecoveryVerificationEngine(Runtime& runtime);
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Result<void> shutdown() override;
    [[nodiscard]] bool is_initialized() const noexcept override;
    [[nodiscard]] Result<RecoveryVerificationResult> verify(const RecoveryVerificationRequest& request);
    void register_invariant(std::shared_ptr<IConsistencyInvariant> invariant);
    [[nodiscard]] ConsistencyInvariantRegistry& invariants() noexcept;

private:
    Runtime& runtime_;
    std::atomic<bool> initialized_{false};
    ConsistencyInvariantRegistry invariants_;
};

class SQLiteRecoveryExecutor final : public IRecoveryExecutor {
public:
    [[nodiscard]] RecoveryExecutionResult execute(const RecoveryRequest& request) override;
};

class SQLiteRecoveredStateInspector final : public IRecoveredStateInspector {
public:
    explicit SQLiteRecoveredStateInspector(std::map<std::string, std::string> transaction_value_queries = {});
    [[nodiscard]] RecoveredDatabaseState inspect(const std::filesystem::path& database_path) override;

private:
    std::map<std::string, std::string> transaction_value_queries_;
};

}
