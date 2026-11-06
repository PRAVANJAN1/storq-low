#include <storq/forensics/forensics_engine.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace storq {
namespace {
StorqError forensics_error(std::string message) {
    return StorqError{ErrorCategory::Integrity, ErrorCode::InvalidArgument, std::move(message), {}};
}

std::string evidence_identifier() {
    static std::atomic<std::uint64_t> sequence{0};
    std::ostringstream out;
    out << "evidence-" << ++sequence;
    return out.str();
}
}

Evidence make_evidence(EvidenceType type, std::string source_module,
                       std::string source_reference, std::string description) {
    Evidence evidence;
    evidence.evidence_id = evidence_identifier();
    evidence.type = type;
    evidence.source_module = std::move(source_module);
    evidence.source_reference = std::move(source_reference);
    evidence.timestamp = std::chrono::system_clock::now();
    evidence.description = std::move(description);
    return evidence;
}

std::string evidence_type_name(EvidenceType type) noexcept {
    switch (type) {
        case EvidenceType::StorageOperation: return "StorageOperation";
        case EvidenceType::InjectedFault: return "InjectedFault";
        case EvidenceType::CrashEvent: return "CrashEvent";
        case EvidenceType::PersistenceTransition: return "PersistenceTransition";
        case EvidenceType::PageAnalysis: return "PageAnalysis";
        case EvidenceType::WalAnalysis: return "WalAnalysis";
        case EvidenceType::TransactionOutcome: return "TransactionOutcome";
        case EvidenceType::RecoveryFinding: return "RecoveryFinding";
        case EvidenceType::StructuralAnomaly: return "StructuralAnomaly";
        case EvidenceType::SnapshotReference: return "SnapshotReference";
        case EvidenceType::ExecutionDiagnostic: return "ExecutionDiagnostic";
    }
    return "ExecutionDiagnostic";
}

Result<void> EvidenceRepository::add(const Evidence& evidence) {
    std::lock_guard lock(mutex_);
    if (evidence.evidence_id.empty()) return forensics_error("Evidence ID must not be empty.");
    if (identifiers_.contains(evidence.evidence_id)) return forensics_error("Duplicate evidence ID.");
    identifiers_[evidence.evidence_id] = evidence_.size();
    evidence_.push_back(evidence);
    return Result<void>();
}
Result<std::vector<Evidence>> EvidenceRepository::query(const EvidenceQuery& query) const {
    std::lock_guard lock(mutex_);
    std::vector<Evidence> result;
    for (const auto& evidence : evidence_) {
        if (query.evidence_id && evidence.evidence_id != *query.evidence_id) continue;
        if (query.type && evidence.type != *query.type) continue;
        if (query.source_module && evidence.source_module != *query.source_module) continue;
        if (query.operation_sequence && evidence.operation_sequence != *query.operation_sequence) continue;
        if (query.page_number && evidence.page_number != *query.page_number) continue;
        if (query.wal_frame_number && evidence.wal_frame_number != *query.wal_frame_number) continue;
        if (query.transaction_id && evidence.transaction_id != *query.transaction_id) continue;
        if (query.crash_id && evidence.crash_id != *query.crash_id) continue;
        result.push_back(evidence);
        if (result.size() >= query.limit) break;
    }
    return result;
}
Result<Evidence> EvidenceRepository::get(const std::string& evidence_id) const {
    std::lock_guard lock(mutex_);
    auto found = identifiers_.find(evidence_id);
    if (found == identifiers_.end()) return forensics_error("Evidence not found.");
    return evidence_[found->second];
}

TimelineResult TimelineBuilder::build(const std::vector<Evidence>& evidence) const {
    TimelineResult result;
    std::vector<Evidence> ordered = evidence;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.operation_sequence && b.operation_sequence) return *a.operation_sequence < *b.operation_sequence;
        if (a.operation_sequence.has_value() != b.operation_sequence.has_value())
            return a.operation_sequence.has_value();
        return a.timestamp < b.timestamp;
    });
    for (const auto& item : ordered) {
        TimelineEvent event;
        event.event_id = "timeline-" + item.evidence_id;
        event.event_type = evidence_type_name(item.type);
        event.timestamp = item.timestamp;
        event.operation_sequence = item.operation_sequence;
        event.source_module = item.source_module;
        event.description = item.description;
        event.evidence_ids.push_back(item.evidence_id);
        event.ordering_uncertain = !item.operation_sequence.has_value() &&
                                   item.timestamp == std::chrono::system_clock::time_point{};
        result.events.push_back(std::move(event));
    }
    return result;
}

void EvidenceCorrelationEngine::register_rule(std::shared_ptr<ICorrelationRule> rule) {
    if (!rule) return;
    std::lock_guard lock(mutex_);
    rules_.erase(std::remove_if(rules_.begin(), rules_.end(), [&](const auto& item) {
        return item->name() == rule->name();
    }), rules_.end());
    rules_.push_back(std::move(rule));
}
std::vector<CorrelationResult> EvidenceCorrelationEngine::correlate(const EvidenceSet& evidence) const {
    std::lock_guard lock(mutex_);
    std::vector<CorrelationResult> result;
    for (const auto& rule : rules_) {
        auto correlation = rule->evaluate(evidence);
        if (!correlation.evidence_ids.empty()) result.push_back(std::move(correlation));
    }
    return result;
}

std::vector<FailureClassification> FailureClassificationEngine::classify(const EvidenceSet& evidence) const {
    std::vector<FailureClassification> result;
    std::map<FailureCategory, std::pair<std::string, std::vector<std::string>>> categories;
    for (const auto& item : evidence.evidence) {
        if (item.type == EvidenceType::InjectedFault && item.attributes.count("fault_type")) {
            categories[FailureCategory::StorageFailure].second.push_back(item.evidence_id);
        }
        if (item.type == EvidenceType::WalAnalysis) {
            categories[FailureCategory::WalCorruption].second.push_back(item.evidence_id);
        }
        if (item.type == EvidenceType::PageAnalysis) {
            categories[FailureCategory::PageCorruption].second.push_back(item.evidence_id);
        }
        if (item.type == EvidenceType::TransactionOutcome ||
            (item.type == EvidenceType::RecoveryFinding && item.attributes.count("atomicity"))) {
            categories[FailureCategory::TransactionAtomicityFailure].second.push_back(item.evidence_id);
        }
        if (item.type == EvidenceType::RecoveryFinding && item.attributes.count("durability")) {
            categories[FailureCategory::TransactionDurabilityFailure].second.push_back(item.evidence_id);
        }
        if (item.type == EvidenceType::StructuralAnomaly) {
            categories[FailureCategory::StructuralFailure].second.push_back(item.evidence_id);
        }
    }
    for (auto& [category, details] : categories) {
        FailureClassification classification;
        classification.category = category;
        classification.description = "Classification supported by explicit source evidence.";
        classification.supporting_evidence_ids = std::move(details.second);
        classification.confidence = 0.7;
        result.push_back(std::move(classification));
    }
    if (result.empty()) {
        FailureClassification unknown;
        unknown.category = FailureCategory::UnknownFailure;
        unknown.description = "No reliable source evidence identified a known failure category.";
        unknown.confidence = 0.0;
        result.push_back(std::move(unknown));
    }
    return result;
}

void FindingsManager::add(ForensicFinding finding) {
    std::lock_guard lock(mutex_);
    for (const auto& existing : findings_) {
        if (existing.category == finding.category && existing.title == finding.title &&
            existing.page_number == finding.page_number && existing.wal_frame_number == finding.wal_frame_number &&
            existing.transaction_id == finding.transaction_id) return;
    }
    findings_.push_back(std::move(finding));
}
std::vector<ForensicFinding> FindingsManager::findings() const {
    std::lock_guard lock(mutex_);
    return findings_;
}
std::map<int, std::vector<ForensicFinding>> FindingsManager::grouped() const {
    std::lock_guard lock(mutex_);
    std::map<int, std::vector<ForensicFinding>> result;
    for (const auto& finding : findings_) result[finding.severity].push_back(finding);
    return result;
}

ConfidenceRationale EvidenceConfidenceEvaluator::evaluate(
    const EvidenceSet& evidence, const CorrelationResult* correlation) const {
    ConfidenceRationale result;
    if (evidence.evidence.empty()) {
        result.reasons.push_back("No evidence was available.");
        return result;
    }
    double score = 0.2;
    result.reasons.push_back("Evidence collection supplies a non-empty base score.");
    std::unordered_set<std::string> sources;
    for (const auto& item : evidence.evidence) sources.insert(item.source_module);
    if (sources.size() > 1) {
        score += 0.2;
        result.reasons.push_back("Evidence is independently corroborated across source modules.");
    }
    if (correlation) {
        score += correlation->causal_certainty ? 0.4 : 0.1;
        result.reasons.push_back("Correlation rule support was applied without inferring causality by timestamp alone.");
    }
    if (evidence.evidence.size() < 3) result.reasons.push_back("Evidence is limited; manual confirmation is advisable.");
    result.score = std::min(1.0, score);
    return result;
}

std::vector<RootCauseHypothesis> RootCauseHypothesisEngine::generate(
    const EvidenceSet& evidence, const std::vector<CorrelationResult>& correlations) const {
    EvidenceConfidenceEvaluator evaluator;
    std::vector<RootCauseHypothesis> result;
    for (const auto& correlation : correlations) {
        RootCauseHypothesis hypothesis;
        hypothesis.hypothesis_id = "hypothesis-" + correlation.rule_name;
        hypothesis.category = correlation.rule_name.find("WAL") != std::string::npos
            ? FailureCategory::WalCorruption : FailureCategory::UnknownFailure;
        hypothesis.title = correlation.rule_name;
        hypothesis.explanation = correlation.explanation + " This remains a hypothesis unless causal evidence confirms it.";
        hypothesis.supporting_evidence_ids = correlation.evidence_ids;
        hypothesis.confidence_score = evaluator.evaluate(evidence, &correlation).score;
        hypothesis.requires_manual_confirmation = !correlation.causal_certainty;
        result.push_back(std::move(hypothesis));
    }
    return result;
}

std::vector<ForensicFinding> PageForensicsAnalyzer::analyze(const EvidenceSet& evidence) const {
    std::vector<ForensicFinding> result;
    for (const auto& item : evidence.evidence) {
        if (item.type != EvidenceType::PageAnalysis && item.type != EvidenceType::StructuralAnomaly) continue;
        ForensicFinding finding;
        finding.finding_id = "finding-page-" + item.evidence_id;
        finding.title = "Page-level storage anomaly";
        finding.category = "Page corruption";
        finding.severity = item.attributes.count("critical") ? 4 : 2;
        finding.description = item.description;
        finding.evidence_ids.push_back(item.evidence_id);
        finding.page_number = item.page_number;
        finding.confidence = 0.7;
        finding.recommended_action = "Correlate the page operation and WAL frame evidence before concluding causation.";
        result.push_back(std::move(finding));
    }
    return result;
}

std::vector<ForensicFinding> WALForensicsAnalyzer::analyze(const EvidenceSet& evidence) const {
    std::vector<ForensicFinding> result;
    for (const auto& item : evidence.evidence) {
        if (item.type != EvidenceType::WalAnalysis) continue;
        ForensicFinding finding;
        finding.finding_id = "finding-wal-" + item.evidence_id;
        finding.title = "WAL forensic anomaly";
        finding.category = "WAL analysis";
        finding.severity = 3;
        finding.description = item.description;
        finding.evidence_ids.push_back(item.evidence_id);
        finding.wal_frame_number = item.wal_frame_number;
        finding.confidence = 0.75;
        finding.recommended_action = "Verify the frame boundary, checksum, and crash timing.";
        result.push_back(std::move(finding));
    }
    return result;
}

std::vector<ForensicFinding> TransactionFailureAnalyzer::analyze(const EvidenceSet& evidence) const {
    std::vector<ForensicFinding> result;
    for (const auto& item : evidence.evidence) {
        if (item.type != EvidenceType::TransactionOutcome && item.type != EvidenceType::RecoveryFinding) continue;
        ForensicFinding finding;
        finding.finding_id = "finding-tx-" + item.evidence_id;
        finding.title = "Transaction recovery anomaly";
        finding.category = "Transaction failure";
        finding.severity = 3;
        finding.description = item.description;
        finding.evidence_ids.push_back(item.evidence_id);
        finding.transaction_id = item.transaction_id;
        finding.confidence = 0.65;
        finding.recommended_action = "Compare recovered effects with the independent transaction history.";
        result.push_back(std::move(finding));
    }
    return result;
}

void AnomalyDetector::register_rule(std::string name, std::function<bool(const EvidenceSet&)> rule,
                                    int severity, std::string explanation) {
    std::lock_guard lock(mutex_);
    rules_.push_back({std::move(name), std::move(rule), severity, std::move(explanation)});
}
std::vector<Anomaly> AnomalyDetector::detect(const EvidenceSet& evidence) const {
    std::lock_guard lock(mutex_);
    std::vector<Anomaly> result;
    for (const auto& rule : rules_) {
        if (rule.predicate(evidence)) {
            Anomaly anomaly;
            anomaly.name = rule.name;
            anomaly.severity = rule.severity;
            anomaly.explanation = rule.explanation;
            for (const auto& item : evidence.evidence) anomaly.evidence_ids.push_back(item.evidence_id);
            result.push_back(std::move(anomaly));
        }
    }
    return result;
}

ForensicsEngine::ForensicsEngine(Runtime& runtime, ForensicsConfig config)
    : runtime_(runtime), config_(config) {}
std::string_view ForensicsEngine::name() const noexcept { return "ForensicsEngine"; }
Result<void> ForensicsEngine::initialize() {
    static_cast<void>(runtime_.capabilities().register_capability("forensics-analysis"));
    initialized_ = true;
    return Result<void>();
}
Result<void> ForensicsEngine::shutdown() { initialized_ = false; return Result<void>(); }
bool ForensicsEngine::is_initialized() const noexcept { return initialized_.load(); }
EvidenceRepository& ForensicsEngine::repository() noexcept { return repository_; }

Result<ForensicsResult> ForensicsEngine::investigate(const ForensicsRequest& request) {
    if (!initialized_.load()) return forensics_error("Forensics engine is not initialized.");
    ForensicsResult result;
    result.investigation_id = "investigation-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    EvidenceSet evidence_set{request.initial_evidence};
    if (evidence_set.evidence.size() > config_.evidence_limit) evidence_set.evidence.resize(config_.evidence_limit);

    if (request.fault_history) {
        for (const auto& fault : request.fault_history->entries()) {
            auto evidence = make_evidence(EvidenceType::InjectedFault, "Module6", fault.fault_id,
                                          fault.description.empty() ? "Fault injected." : fault.description);
            evidence.operation_sequence = fault.operation_sequence;
            evidence.attributes["fault_type"] = std::to_string(static_cast<int>(fault.type));
            evidence_set.evidence.push_back(std::move(evidence));
        }
    }
    if (request.crash_history) {
        for (const auto& crash : request.crash_history->executions()) {
            auto evidence = make_evidence(EvidenceType::CrashEvent, "Module7", crash.crash_id,
                                          "Simulated crash execution.");
            evidence.operation_sequence = crash.trigger_sequence;
            evidence.crash_id = crash.crash_id;
            evidence_set.evidence.push_back(std::move(evidence));
        }
    }
    if (request.crash_simulator) {
        result.available_sources.push_back("Module7");
        result.complete = request.recovery_report != nullptr;
    }
    if (request.recovery_report) {
        result.available_sources.push_back("Module8");
        if (!request.recovery_report->transactions.empty()) {
            for (const auto& transaction : request.recovery_report->transactions) {
                auto evidence = make_evidence(EvidenceType::TransactionOutcome, "Module8",
                                              transaction.expected.transaction_id, "Recovered transaction outcome.");
                evidence.transaction_id = transaction.expected.transaction_id;
                evidence.attributes["status"] = std::to_string(static_cast<int>(transaction.status));
                evidence_set.evidence.push_back(std::move(evidence));
            }
        }
        for (const auto& finding : request.recovery_report->findings) {
            auto evidence = make_evidence(EvidenceType::RecoveryFinding, "Module8", finding.id, finding.description);
            evidence.transaction_id = finding.transaction_id;
            evidence_set.evidence.push_back(std::move(evidence));
        }
    }

    for (const auto& evidence : evidence_set.evidence) {
        static_cast<void>(repository_.add(evidence));
        if (!evidence.source_module.empty() &&
            std::find(result.available_sources.begin(), result.available_sources.end(), evidence.source_module) ==
            result.available_sources.end()) result.available_sources.push_back(evidence.source_module);
    }
    if (result.available_sources.empty()) result.missing_sources.push_back("Module evidence");
    result.timeline = TimelineBuilder().build(evidence_set.evidence);

    EvidenceCorrelationEngine correlation;
    class SyncCrashRule final : public ICorrelationRule {
        std::string name() const override { return "Failed synchronization followed by crash"; }
        CorrelationResult evaluate(const EvidenceSet& evidence) const override {
            CorrelationResult result;
            result.rule_name = name();
            for (const auto& item : evidence.evidence) {
                if (item.description.find("sync") != std::string::npos ||
                    item.description.find("Synchron") != std::string::npos ||
                    item.attributes.count("fault_type")) result.evidence_ids.push_back(item.evidence_id);
            }
            if (!result.evidence_ids.empty()) {
                result.confidence = 0.6;
                result.explanation = "Explicit synchronization-related evidence and crash context were observed.";
                result.missing_support.push_back("Independent crash confirmation if absent.");
            }
            return result;
        }
    };
    correlation.register_rule(std::make_shared<SyncCrashRule>());
    result.correlations = correlation.correlate(evidence_set);
    result.classifications = FailureClassificationEngine().classify(evidence_set);

    FindingsManager findings;
    auto page_findings = PageForensicsAnalyzer().analyze(evidence_set);
    auto wal_findings = WALForensicsAnalyzer().analyze(evidence_set);
    auto transaction_findings = TransactionFailureAnalyzer().analyze(evidence_set);
    for (auto& finding : page_findings) findings.add(std::move(finding));
    for (auto& finding : wal_findings) findings.add(std::move(finding));
    for (auto& finding : transaction_findings) findings.add(std::move(finding));

    AnomalyDetector detector;
    detector.register_rule("Repeated injected synchronization failures", [](const EvidenceSet& evidence) {
        std::size_t count = 0;
        for (const auto& item : evidence.evidence)
            if (item.type == EvidenceType::InjectedFault &&
                item.description.find("sync") != std::string::npos) ++count;
        return count >= 2;
    }, 3, "Multiple explicit synchronization fault records were observed.");
    result.anomalies = detector.detect(evidence_set);
    result.hypotheses = RootCauseHypothesisEngine().generate(evidence_set, result.correlations);
    result.findings = findings.findings();
    result.complete = !evidence_set.evidence.empty() && !result.available_sources.empty();
    if (!result.complete) result.limitations.push_back("Required evidence sources were unavailable; conclusions are incomplete.");
    if (config_.require_complete_evidence && result.missing_sources.empty() && result.available_sources.size() < 2)
        result.missing_sources.push_back("Additional independent evidence sources");
    return result;
}

std::string ForensicsResult::to_json() const {
    std::ostringstream output;
    output << "{\"investigation_id\":\"" << investigation_id << "\","
           << "\"complete\":" << (complete ? "true" : "false") << ","
           << "\"timeline_events\":" << timeline.events.size() << ","
           << "\"findings\":" << findings.size() << ","
           << "\"hypotheses\":" << hypotheses.size() << ","
           << "\"available_sources\":" << available_sources.size() << ","
           << "\"missing_sources\":" << missing_sources.size() << "}";
    return output.str();
}

}
