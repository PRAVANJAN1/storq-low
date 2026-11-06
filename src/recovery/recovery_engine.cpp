#include <storq/recovery/recovery_engine.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace storq {
namespace {
StorqError recovery_error(std::string message) {
    return StorqError{ErrorCategory::Recovery, ErrorCode::InvalidArgument, std::move(message), {}};
}

std::string unique_id(const char* prefix) {
    static std::atomic<std::uint64_t> sequence{0};
    std::ostringstream stream;
    stream << prefix << '-' << ++sequence;
    return stream.str();
}

bool within(const std::filesystem::path& child, const std::filesystem::path& parent) {
    auto a = std::filesystem::weakly_canonical(child);
    auto b = std::filesystem::weakly_canonical(parent);
    auto ai = a.begin(), bi = b.begin();
    while (ai != a.end() && bi != b.end() && *ai == *bi) ++ai, ++bi;
    return bi == b.end();
}
}

void ConsistencyInvariantRegistry::register_invariant(std::shared_ptr<IConsistencyInvariant> invariant) {
    if (!invariant) return;
    std::lock_guard lock(mutex_);
    invariants_.erase(std::remove_if(invariants_.begin(), invariants_.end(),
        [&](const auto& item) { return item->name() == invariant->name(); }), invariants_.end());
    invariants_.push_back(std::move(invariant));
}
std::vector<InvariantResult> ConsistencyInvariantRegistry::evaluate(const InvariantContext& context) const {
    std::lock_guard lock(mutex_);
    std::vector<InvariantResult> result;
    for (const auto& invariant : invariants_) result.push_back(invariant->evaluate(context));
    return result;
}
std::size_t ConsistencyInvariantRegistry::size() const {
    std::lock_guard lock(mutex_);
    return invariants_.size();
}

std::vector<TransactionComparison> TransactionOutcomeComparator::compare(
    const std::vector<ExpectedTransaction>& expected,
    const RecoveredDatabaseState& recovered) const {
    std::vector<TransactionComparison> result;
    for (const auto& transaction : expected) {
        TransactionComparison comparison;
        comparison.expected = transaction;
        auto observed = std::find_if(recovered.transactions.begin(), recovered.transactions.end(),
            [&](const auto& item) { return item.transaction_id == transaction.transaction_id; });
        if (observed != recovered.transactions.end()) {
            comparison.observed = *observed;
            comparison.matching_transaction_found = true;
        }
        if (!recovered.logical_inspection_completed) {
            comparison.status = TransactionVerificationStatus::Inconclusive;
            comparison.evidence.push_back("Logical inspection did not complete.");
        } else if (transaction.expected_state == ExpectedTransactionState::Unknown) {
            comparison.status = TransactionVerificationStatus::Uncertain;
            comparison.evidence.push_back("Expected outcome is unknown.");
        } else if (observed == recovered.transactions.end()) {
            comparison.status = transaction.expected_state == ExpectedTransactionState::Committed &&
                               transaction.durability_expected
                ? TransactionVerificationStatus::Missing : TransactionVerificationStatus::RolledBackCorrectly;
            comparison.evidence.push_back("No recovered effects matched this transaction ID.");
        } else if (!observed->transaction_effects_present) {
            comparison.status = transaction.expected_state == ExpectedTransactionState::Committed
                ? TransactionVerificationStatus::Missing : TransactionVerificationStatus::RolledBackCorrectly;
        } else if (!observed->transaction_effects_complete) {
            comparison.status = TransactionVerificationStatus::PartiallyApplied;
        } else {
            comparison.status = transaction.expected_state == ExpectedTransactionState::Committed ||
                                transaction.expected_state == ExpectedTransactionState::Incomplete
                ? TransactionVerificationStatus::Survived : TransactionVerificationStatus::UnexpectedlyPersisted;
        }
        result.push_back(std::move(comparison));
    }
    return result;
}

std::vector<AtomicityResult> AtomicityVerifier::verify(
    const std::vector<ExpectedTransaction>& expected, const RecoveredDatabaseState& recovered) const {
    std::vector<AtomicityResult> result;
    for (const auto& transaction : expected) {
        AtomicityResult item;
        item.transaction_id = transaction.transaction_id;
        auto observed = std::find_if(recovered.transactions.begin(), recovered.transactions.end(),
            [&](const auto& value) { return value.transaction_id == transaction.transaction_id; });
        if (!recovered.logical_inspection_completed) {
            item.status = InvariantStatus::Inconclusive;
            item.details = "Logical transaction effects were not available.";
        } else if (observed == recovered.transactions.end()) {
            item.status = transaction.expected_state == ExpectedTransactionState::Committed
                ? InvariantStatus::Failed : InvariantStatus::Passed;
            item.details = "Transaction effects are absent.";
        } else {
            item.status = observed->transaction_effects_complete ? InvariantStatus::Passed : InvariantStatus::Failed;
            item.details = observed->transaction_effects_complete
                ? "All observed transaction effects were complete."
                : "Transaction effects were incomplete.";
        }
        item.passed = item.status == InvariantStatus::Passed;
        result.push_back(std::move(item));
    }
    return result;
}

std::vector<DurabilityResult> DurabilityVerifier::verify(
    const std::vector<ExpectedTransaction>& expected, const RecoveredDatabaseState& recovered) const {
    std::vector<DurabilityResult> result;
    for (const auto& transaction : expected) {
        DurabilityResult item;
        item.transaction_id = transaction.transaction_id;
        auto observed = std::find_if(recovered.transactions.begin(), recovered.transactions.end(),
            [&](const auto& value) { return value.transaction_id == transaction.transaction_id; });
        if (transaction.expected_state != ExpectedTransactionState::Committed) {
            item.status = DurabilityStatus::NotExpectedToSurvive;
        } else if (!transaction.commit_sequence.has_value()) {
            item.status = DurabilityStatus::Uncertain;
        } else if (!recovered.logical_inspection_completed) {
            item.status = DurabilityStatus::Inconclusive;
        } else if (observed == recovered.transactions.end() || !observed->transaction_effects_present) {
            item.status = transaction.durability_expected
                ? DurabilityStatus::ExpectedButMissing : DurabilityStatus::Uncertain;
        } else {
            item.status = transaction.commit_was_synchronized
                ? DurabilityStatus::DurableAndPreserved : DurabilityStatus::PreservedWithoutDurabilityGuarantee;
        }
        item.details = "Transaction durability evaluated from independent history and recovered observations.";
        result.push_back(std::move(item));
    }
    return result;
}

StructuralRecoveryValidator::StructuralRecoveryValidator(Runtime& runtime) : runtime_(runtime) {}
StructuralValidationResult StructuralRecoveryValidator::validate(const std::filesystem::path& database_path) const {
    StructuralValidationResult result;
    if (!std::filesystem::exists(database_path)) {
        result.status = StructuralValidationStatus::Error;
        result.evidence.push_back("Recovery database does not exist.");
        return result;
    }
    StorageManager storage;
    FormatInspector inspector(runtime_, storage);
    auto initialized = inspector.initialize();
    if (!initialized) {
        result.status = StructuralValidationStatus::Error;
        result.evidence.push_back("Format inspector could not initialize.");
        return result;
    }
    auto inspection = inspector.inspect(database_path);
    if (!inspection) {
        result.status = StructuralValidationStatus::Error;
        result.evidence.push_back("Format inspection failed.");
        return result;
    }
    result.header_valid = inspection.value().header_valid;
    result.structure_valid = inspection.value().structure_valid;
    result.status = result.header_valid && result.structure_valid
        ? StructuralValidationStatus::Passed : StructuralValidationStatus::Failed;
    result.evidence.push_back(result.header_valid ? "Database header is valid." : "Database header is invalid.");
    static_cast<void>(inspector.shutdown());
    return result;
}

void FindingManager::add(RecoveryFinding finding) {
    std::lock_guard lock(mutex_);
    findings_.push_back(std::move(finding));
}
std::vector<RecoveryFinding> FindingManager::findings() const {
    std::lock_guard lock(mutex_);
    return findings_;
}
void FindingManager::clear() {
    std::lock_guard lock(mutex_);
    findings_.clear();
}
std::map<RecoverySeverity, std::vector<RecoveryFinding>> FindingManager::grouped() const {
    std::lock_guard lock(mutex_);
    std::map<RecoverySeverity, std::vector<RecoveryFinding>> result;
    for (const auto& finding : findings_) result[finding.severity].push_back(finding);
    return result;
}

RecoveryExecutionResult SQLiteRecoveryExecutor::execute(const RecoveryRequest& request) {
    RecoveryExecutionResult result;
    result.database_path = request.database_path;
    const auto start = std::chrono::steady_clock::now();
    std::error_code error;
    if (!std::filesystem::exists(request.database_path, error) || error) {
        result.status = RecoveryExecutionStatus::Failed;
        result.errors.push_back("Source database does not exist.");
        return result;
    }
    if (request.isolated_workspace.empty() ||
        !within(request.database_path, request.isolated_workspace)) {
        result.status = RecoveryExecutionStatus::Failed;
        result.errors.push_back("Database must be located inside the isolated recovery workspace.");
        return result;
    }
    if (!std::filesystem::is_directory(request.isolated_workspace, error)) {
        std::filesystem::create_directories(request.isolated_workspace, error);
    }
    if (error) {
        result.status = RecoveryExecutionStatus::Failed;
        result.errors.push_back(error.message());
        return result;
    }
    auto evidence = request.isolated_workspace / "pre-recovery.db";
    std::filesystem::copy_file(request.database_path, evidence,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        result.status = RecoveryExecutionStatus::Failed;
        result.errors.push_back("Could not preserve recovery evidence: " + error.message());
        return result;
    }
    result.database_path = evidence;
    result.completed = true;
    result.database_opened = true;
    result.suitable_for_inspection = true;
    result.status = RecoveryExecutionStatus::Completed;
    result.diagnostics.push_back("SQLite recovery evidence copied to isolated workspace.");
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}

SQLiteRecoveredStateInspector::SQLiteRecoveredStateInspector(
    std::map<std::string, std::string> transaction_value_queries)
    : transaction_value_queries_(std::move(transaction_value_queries)) {}

RecoveredDatabaseState SQLiteRecoveredStateInspector::inspect(const std::filesystem::path& database_path) {
    RecoveredDatabaseState state;
    state.database_opened = std::filesystem::exists(database_path);
    state.logical_inspection_completed = false;
    state.metadata["inspection"] = "No SQL transaction-ID inspection adapter was configured.";
    if (state.database_opened && transaction_value_queries_.empty()) {
        state.metadata["limitation"] = "Arbitrary SQLite schemas cannot be mapped to transaction IDs.";
    } else if (!transaction_value_queries_.empty()) {
        state.logical_inspection_completed = true;
    }
    return state;
}

RepeatabilityResult RecoveryRepeatabilityValidator::validate(
    const std::filesystem::path& immutable_snapshot, std::size_t runs,
    IRecoveryExecutor& executor, IRecoveredStateInspector& inspector) const {
    RepeatabilityResult result;
    result.status = RepeatabilityStatus::Inconclusive;
    if (!std::filesystem::exists(immutable_snapshot) || runs < 2) {
        result.errors.push_back("An existing snapshot and at least two runs are required.");
        result.status = RepeatabilityStatus::Error;
        return result;
    }
    std::optional<RecoveredDatabaseState> baseline;
    for (std::size_t run = 0; run < runs; ++run) {
        std::filesystem::path workspace = immutable_snapshot.parent_path() /
            ("repeatability-" + std::to_string(run));
        std::error_code error;
        std::filesystem::create_directories(workspace, error);
        RecoveryRequest request;
        request.database_path = immutable_snapshot;
        request.isolated_workspace = workspace;
        request.mode = RecoveryExecutionMode::RepeatRecovery;
        auto execution = executor.execute(request);
        if (!execution.completed) {
            result.status = RepeatabilityStatus::Error;
            result.errors.push_back("Recovery execution failed.");
            continue;
        }
        auto current = inspector.inspect(execution.database_path);
        if (!baseline) baseline = current;
        else if (baseline->observed_record_count != current.observed_record_count ||
                 baseline->metadata != current.metadata) {
            result.status = RepeatabilityStatus::Failed;
            result.differences.push_back("Logical state differed in run " + std::to_string(run));
        }
    }
    if (result.status == RepeatabilityStatus::Inconclusive && baseline) result.status = RepeatabilityStatus::Passed;
    return result;
}

std::string RecoveryVerificationReport::to_json() const {
    std::ostringstream output;
    output << "{\"verification_id\":\"" << verification_id << "\","
           << "\"status\":" << static_cast<int>(status) << ","
           << "\"expected_transactions\":" << expected_transaction_count << ","
           << "\"missing_committed\":" << missing_committed_count << ","
           << "\"findings\":" << findings.size() << "}";
    return output.str();
}

RecoveryVerificationEngine::RecoveryVerificationEngine(Runtime& runtime) : runtime_(runtime) {}
std::string_view RecoveryVerificationEngine::name() const noexcept { return "RecoveryVerificationEngine"; }
Result<void> RecoveryVerificationEngine::initialize() {
    static_cast<void>(runtime_.capabilities().register_capability("recovery-verification"));
    initialized_ = true;
    return Result<void>();
}
Result<void> RecoveryVerificationEngine::shutdown() { initialized_ = false; return Result<void>(); }
bool RecoveryVerificationEngine::is_initialized() const noexcept { return initialized_.load(); }
void RecoveryVerificationEngine::register_invariant(std::shared_ptr<IConsistencyInvariant> invariant) {
    invariants_.register_invariant(std::move(invariant));
}
ConsistencyInvariantRegistry& RecoveryVerificationEngine::invariants() noexcept { return invariants_; }

Result<RecoveryVerificationResult> RecoveryVerificationEngine::verify(const RecoveryVerificationRequest& request) {
    if (!initialized_.load()) return recovery_error("Recovery verification engine is not initialized.");
    if (!request.executor || !request.inspector || !request.history)
        return recovery_error("Recovery executor, inspector, and history are required.");
    if (!std::filesystem::exists(request.recovery.database_path))
        return recovery_error("Recovery database does not exist.");
    if (request.recovery.isolated_workspace.empty() ||
        !within(request.recovery.database_path, request.recovery.isolated_workspace))
        return recovery_error("Recovery database must be inside the isolated workspace.");

    RecoveryVerificationResult result;
    result.report.verification_id = unique_id("verification");
    result.report.started_at = std::chrono::system_clock::now();
    const auto expected = request.history->get_transactions();
    result.report.expected_transaction_count = expected.size();

    auto execution = request.executor->execute(request.recovery);
    result.report.recovery_status = execution.status;
    result.report.recovery_duration = execution.duration;

    RecoveredDatabaseState recovered;
    if (execution.completed && execution.suitable_for_inspection)
        recovered = request.inspector->inspect(execution.database_path);
    result.report.recovered_transaction_count = recovered.transactions.size();

    TransactionOutcomeComparator comparator;
    result.report.transactions = comparator.compare(expected, recovered);
    result.report.atomicity = AtomicityVerifier().verify(expected, recovered);
    result.report.durability = DurabilityVerifier().verify(expected, recovered);
    InvariantContext context{&recovered, recovered.metadata};
    result.report.invariants = invariants_.evaluate(context);

    StructuralRecoveryValidator structure(runtime_);
    result.report.structure = structure.validate(execution.database_path.empty()
        ? request.recovery.database_path : execution.database_path);

    FindingManager findings;
    for (const auto& comparison : result.report.transactions) {
        RecoveryFinding finding;
        finding.transaction_id = comparison.expected.transaction_id;
        finding.category = "Transaction verification";
        finding.expected = std::to_string(static_cast<int>(comparison.expected.expected_state));
        finding.observed = std::to_string(static_cast<int>(comparison.status));
        finding.evidence = comparison.evidence;
        if (comparison.status == TransactionVerificationStatus::Missing ||
            comparison.status == TransactionVerificationStatus::PartiallyApplied ||
            comparison.status == TransactionVerificationStatus::UnexpectedlyPersisted) {
            finding.severity = RecoverySeverity::Critical;
            finding.title = "Transaction verification violation";
            finding.description = "Recovered transaction state differs from independent expected history.";
            finding.recommended_action = "Inspect the transaction effects and preserved crash evidence.";
            findings.add(std::move(finding));
        } else if (comparison.status == TransactionVerificationStatus::Inconclusive ||
                   comparison.status == TransactionVerificationStatus::Uncertain) {
            finding.severity = RecoverySeverity::Warning;
            finding.title = "Transaction outcome uncertain";
            finding.description = "Available evidence is insufficient to classify the transaction outcome.";
            finding.recommended_action = "Provide an explicit transaction inspection adapter.";
            findings.add(std::move(finding));
        }
    }
    result.report.findings = findings.findings();
    if (!execution.completed) {
        result.report.status = RecoveryVerificationStatus::Error;
        result.stage_errors.push_back("Recovery execution did not complete.");
    } else if (!result.report.findings.empty()) {
        result.report.status = RecoveryVerificationStatus::Failed;
    } else if (!recovered.logical_inspection_completed ||
               result.report.structure.status == StructuralValidationStatus::Inconclusive) {
        result.report.status = RecoveryVerificationStatus::Inconclusive;
        result.report.limitations.push_back("Logical transaction or structural inspection evidence was incomplete.");
    } else {
        result.report.status = RecoveryVerificationStatus::Passed;
    }

    if (request.validate_repeatability) {
        RecoveryRepeatabilityValidator repeatability;
        result.report.repeatability = repeatability.validate(
            execution.database_path, request.repeatability_runs, *request.executor, *request.inspector);
    }
    result.report.completed_at = std::chrono::system_clock::now();
    result.report.rolled_back_count = 0;
    result.report.missing_committed_count = 0;
    result.report.partially_applied_count = 0;
    result.report.unexpected_transaction_count = 0;
    result.report.uncertain_transaction_count = 0;
    for (const auto& comparison : result.report.transactions) {
        if (comparison.status == TransactionVerificationStatus::Missing) ++result.report.missing_committed_count;
        if (comparison.status == TransactionVerificationStatus::UnexpectedlyPersisted) ++result.report.unexpected_transaction_count;
        if (comparison.status == TransactionVerificationStatus::PartiallyApplied) ++result.report.partially_applied_count;
        if (comparison.status == TransactionVerificationStatus::RolledBackCorrectly) ++result.report.rolled_back_count;
        if (comparison.status == TransactionVerificationStatus::Uncertain) ++result.report.uncertain_transaction_count;
    }
    result.status = result.report.status;
    return result;
}

}
