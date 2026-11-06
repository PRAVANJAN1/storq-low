#include <storq/fault/fault_injector.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

#include <storq/analysis/page_analyzer.hpp>
#include <storq/common/utilities.hpp>
#include <storq/core/runtime.hpp>
#include <storq/format/format_inspector.hpp>
#include <storq/wal/wal_analyzer.hpp>

namespace storq {
namespace {

StorqError fault_error(ErrorCode code, std::string message) {
    return {ErrorCategory::FaultInjection, code, std::move(message), {}};
}

bool is_destructive(FaultType type) {
    switch (type) {
    case FaultType::PartialWrite:
    case FaultType::TruncatedWrite:
    case FaultType::FileTruncation:
    case FaultType::DiskFull:
    case FaultType::RandomCorruption:
    case FaultType::TargetedCorruption:
    case FaultType::PageHeaderCorruption:
    case FaultType::ChecksumCorruption:
    case FaultType::WALRecordCorruption:
        return true;
    default:
        return false;
    }
}

bool is_corruption(FaultType type) {
    return type == FaultType::RandomCorruption ||
           type == FaultType::TargetedCorruption ||
           type == FaultType::PageHeaderCorruption ||
           type == FaultType::ChecksumCorruption ||
           type == FaultType::WALRecordCorruption;
}

bool operation_matches_type(FaultType type, StorageOperationType operation) {
    switch (type) {
    case FaultType::WriteFailure:
    case FaultType::PartialWrite:
    case FaultType::TruncatedWrite:
    case FaultType::DelayedWrite:
    case FaultType::RejectedWrite:
    case FaultType::DiskFull:
    case FaultType::RandomCorruption:
    case FaultType::TargetedCorruption:
    case FaultType::PageHeaderCorruption:
    case FaultType::ChecksumCorruption:
    case FaultType::WALRecordCorruption:
    case FaultType::InvalidOffset:
        return operation == StorageOperationType::Write ||
               operation == StorageOperationType::Append;
    case FaultType::SyncFailure:
    case FaultType::InterruptedSync:
        return operation == StorageOperationType::Sync;
    case FaultType::FileTruncation:
        return operation == StorageOperationType::Truncate;
    case FaultType::FileCreationFailure:
        return operation == StorageOperationType::Open;
    case FaultType::FileAccessFailure:
        return operation == StorageOperationType::Read ||
               operation == StorageOperationType::Write ||
               operation == StorageOperationType::Append ||
               operation == StorageOperationType::Truncate ||
               operation == StorageOperationType::Sync ||
               operation == StorageOperationType::Metadata ||
               operation == StorageOperationType::Close ||
               operation == StorageOperationType::Open;
    }
    return false;
}

std::optional<StorageOperationType> effective_operation(const FaultRule& rule) {
    if (rule.target_operation) return rule.target_operation;
    return rule.config.target_operation;
}

std::string effective_resource(const FaultRule& rule) {
    return rule.target_resource.empty() ? rule.config.target_resource : rule.target_resource;
}

std::optional<std::uint64_t> effective_offset(const FaultRule& rule) {
    return rule.target_offset ? rule.target_offset : rule.config.target_offset;
}

std::optional<std::uint64_t> effective_page(const FaultRule& rule) {
    return rule.target_page ? rule.target_page : rule.config.target_page;
}

std::optional<std::uint64_t> effective_trigger_count(const FaultRule& rule) {
    return rule.trigger_count ? rule.trigger_count : rule.config.trigger_count;
}

bool contains_offset(const FaultContext& context, std::uint64_t target) {
    if (!context.offset) return false;
    if (context.requested_bytes == 0) return *context.offset == target;
    if (context.requested_bytes >
        std::numeric_limits<std::uint64_t>::max() - *context.offset) {
        return true;
    }
    return target >= *context.offset &&
           target < *context.offset + context.requested_bytes;
}

bool is_within(const std::filesystem::path& child, const std::filesystem::path& root) {
    const auto relative = child.lexically_relative(root);
    if (relative.empty() || relative == ".") return false;
    if (relative.is_absolute()) return false;
    const auto first = relative.begin();
    return first == relative.end() || *first != "..";
}

Result<std::filesystem::path> canonical_candidate(const std::filesystem::path& path) {
    if (path.empty()) {
        return fault_error(ErrorCode::InvalidArgument, "Test resource path must not be empty.");
    }
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return fault_error(ErrorCode::IoError,
                           "Unable to resolve test resource path: " + error.message());
    }
    return canonical;
}

std::uint64_t page_header_field_offset(SQLitePageHeaderField field) {
    switch (field) {
    case SQLitePageHeaderField::PageType: return 0;
    case SQLitePageHeaderField::FirstFreeblockOffset: return 1;
    case SQLitePageHeaderField::CellCount: return 3;
    case SQLitePageHeaderField::CellContentOffset: return 5;
    case SQLitePageHeaderField::FragmentedFreeBytes: return 7;
    case SQLitePageHeaderField::RightmostChildPage: return 8;
    }
    return 0;
}

std::uint64_t page_header_field_size(SQLitePageHeaderField field) {
    switch (field) {
    case SQLitePageHeaderField::PageType:
    case SQLitePageHeaderField::FragmentedFreeBytes:
        return 1;
    case SQLitePageHeaderField::RightmostChildPage:
        return 4;
    default:
        return 2;
    }
}

void apply_pattern(std::span<std::byte> bytes, std::size_t offset,
                   std::size_t count, const std::vector<std::byte>& pattern) {
    for (std::size_t index = 0; index < count; ++index) {
        if (pattern.empty()) {
            bytes[offset + index] ^= std::byte{0xff};
        } else {
            bytes[offset + index] = pattern[index % pattern.size()];
        }
    }
}

std::uint64_t overlap_start(std::uint64_t request_start, std::uint64_t request_length,
                            std::uint64_t target_start, std::uint64_t target_length) {
    const auto start = std::max(request_start, target_start);
    const auto request_end = request_length >
            std::numeric_limits<std::uint64_t>::max() - request_start
        ? std::numeric_limits<std::uint64_t>::max()
        : request_start + request_length;
    const auto target_end = target_length >
            std::numeric_limits<std::uint64_t>::max() - target_start
        ? std::numeric_limits<std::uint64_t>::max()
        : target_start + target_length;
    return start < std::min(request_end, target_end) ? start : 0;
}

std::uint64_t overlap_length(std::uint64_t request_start, std::uint64_t request_length,
                              std::uint64_t target_start, std::uint64_t target_length) {
    const auto start = std::max(request_start, target_start);
    const auto request_end = request_length >
            std::numeric_limits<std::uint64_t>::max() - request_start
        ? std::numeric_limits<std::uint64_t>::max()
        : request_start + request_length;
    const auto target_end = target_length >
            std::numeric_limits<std::uint64_t>::max() - target_start
        ? std::numeric_limits<std::uint64_t>::max()
        : target_start + target_length;
    const auto end = std::min(request_end, target_end);
    return start < end ? end - start : 0;
}

} // namespace

Result<void> FaultPolicy::validate(const FaultConfig& config) {
    if (static_cast<int>(config.type) < static_cast<int>(FaultType::WriteFailure) ||
        static_cast<int>(config.type) > static_cast<int>(FaultType::InvalidOffset)) {
        return fault_error(ErrorCode::InvalidArgument, "Fault type is invalid.");
    }
    if (static_cast<int>(config.severity) < static_cast<int>(FaultSeverity::Low) ||
        static_cast<int>(config.severity) > static_cast<int>(FaultSeverity::Critical)) {
        return fault_error(ErrorCode::InvalidArgument, "Fault severity is invalid.");
    }
    if (static_cast<int>(config.page_header_field) <
            static_cast<int>(SQLitePageHeaderField::PageType) ||
        static_cast<int>(config.page_header_field) >
            static_cast<int>(SQLitePageHeaderField::RightmostChildPage) ||
        static_cast<int>(config.wal_region) < static_cast<int>(WALCorruptionRegion::FrameHeader) ||
        static_cast<int>(config.wal_region) > static_cast<int>(WALCorruptionRegion::Checksum)) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Format-specific corruption target is invalid.");
    }
    if (!std::isfinite(config.probability) ||
        config.probability < 0.0 || config.probability > 1.0) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Fault probability must be finite and between zero and one.");
    }
    if (config.delay.count() < 0) {
        return fault_error(ErrorCode::InvalidArgument, "Fault delay must not be negative.");
    }
    if (config.trigger_count && *config.trigger_count == 0) {
        return fault_error(ErrorCode::InvalidArgument,
                           "An exact matching trigger count must be greater than zero.");
    }
    if (config.target_page && *config.target_page == 0) {
        return fault_error(ErrorCode::InvalidArgument, "Target page numbers start at one.");
    }
    if (config.target_page &&
        *config.target_page > std::numeric_limits<std::uint32_t>::max()) {
        return fault_error(ErrorCode::InvalidArgument,
                           "SQLite page numbers must fit the format's 32-bit page-number field.");
    }
    if (config.corruption_length == 0 &&
        (is_corruption(config.type) || config.type == FaultType::TargetedCorruption)) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Corruption length must be greater than zero.");
    }
    if (config.target_offset && is_corruption(config.type) &&
        config.corruption_length >
            std::numeric_limits<std::uint64_t>::max() - *config.target_offset) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Corruption offset and length overflow the supported range.");
    }
    if (config.partial_write_fraction < 0.0 ||
        !std::isfinite(config.partial_write_fraction) ||
        config.partial_write_fraction >= 1.0) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Partial-write fraction must be finite and in [0, 1).");
    }
    if (config.partial_write_bytes && *config.partial_write_bytes == 0) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Fixed partial-write size must be greater than zero.");
    }
    if (config.maximum_partial_write_bytes &&
        *config.maximum_partial_write_bytes == 0) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Maximum partial-write size must be greater than zero.");
    }
    if (config.truncation_length && *config.truncation_length == 0) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Truncated-write length must be greater than zero.");
    }
    if (config.type == FaultType::PartialWrite &&
        !config.partial_write_bytes && config.partial_write_fraction == 0.0) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Partial writes require a positive byte count or fraction.");
    }
    if (config.type == FaultType::TruncatedWrite && !config.truncation_length) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Truncated writes require an explicit truncation length.");
    }
    if (config.type == FaultType::FileTruncation && !config.target_size) {
        return fault_error(ErrorCode::InvalidArgument,
                           "File truncation requires an explicit target size.");
    }
    if (config.type == FaultType::DiskFull &&
        (!config.maximum_capacity || *config.maximum_capacity == 0)) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Disk-full simulation requires a positive virtual capacity.");
    }
    if (config.type == FaultType::TargetedCorruption && !config.target_offset) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Targeted corruption requires a byte offset.");
    }
    if (config.type == FaultType::InvalidOffset && !config.target_offset) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Invalid-offset injection requires an explicit target offset.");
    }
    if (config.type == FaultType::PageHeaderCorruption && !config.target_page) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Page-header corruption requires a target page number.");
    }
    if (config.type == FaultType::ChecksumCorruption &&
        config.corruption_length > 8) {
        return fault_error(ErrorCode::InvalidArgument,
                           "SQLite WAL checksum corruption is limited to its 8-byte field.");
    }
    if (config.type == FaultType::WALRecordCorruption &&
        (!config.wal_frame_number || *config.wal_frame_number == 0)) {
        return fault_error(ErrorCode::InvalidArgument,
                           "WAL record corruption requires a positive frame number.");
    }
    if (config.type == FaultType::WALRecordCorruption &&
        config.wal_region != WALCorruptionRegion::PageContent &&
        config.corruption_length > 8) {
        return fault_error(ErrorCode::InvalidArgument,
                           "WAL header-field corruption cannot exceed 8 bytes.");
    }
    if (config.type == FaultType::PageHeaderCorruption &&
        config.corruption_length > page_header_field_size(config.page_header_field)) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Corruption length exceeds the selected SQLite page-header field.");
    }
    if (config.target_operation &&
        !operation_matches_type(config.type, *config.target_operation)) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Configured operation is incompatible with the selected fault type.");
    }
    return {};
}

void FaultHistory::record(FaultExecution execution) {
    std::lock_guard lock(mutex_);
    if (execution.recorded_at == std::chrono::system_clock::time_point{}) {
        execution.recorded_at = std::chrono::system_clock::now();
    }
    entries_.push_back(std::move(execution));
}

std::vector<FaultExecution> FaultHistory::entries() const {
    std::lock_guard lock(mutex_);
    return entries_;
}

void FaultHistory::record_operation(FaultOperationTrace operation) {
    std::lock_guard lock(mutex_);
    operations_.push_back(std::move(operation));
}

std::vector<FaultOperationTrace> FaultHistory::operations() const {
    std::lock_guard lock(mutex_);
    auto result = operations_;
    std::stable_sort(result.begin(), result.end(),
        [](const FaultOperationTrace& left, const FaultOperationTrace& right) {
            return left.operation_sequence < right.operation_sequence;
        });
    return result;
}

FaultStatistics FaultHistory::statistics() const {
    std::lock_guard lock(mutex_);
    FaultStatistics result;
    for (const auto& entry : entries_) {
        switch (entry.status) {
        case FaultStatus::Scheduled:
            ++result.total_configured;
            ++result.total_scheduled;
            break;
        case FaultStatus::Triggered:
            ++result.total_triggered;
            ++result.faults_by_type[entry.type];
            ++result.faults_by_operation[entry.operation];
            break;
        case FaultStatus::Executed:
            ++result.total_executed;
            result.total_injected_delay += entry.actual_delay;
            if (entry.type == FaultType::PartialWrite ||
                entry.type == FaultType::TruncatedWrite ||
                entry.type == FaultType::DiskFull) {
                result.total_partial_bytes += entry.actual_bytes;
            }
            result.total_corrupted_bytes += entry.corrupted_offsets.size();
            break;
        case FaultStatus::Failed:
            ++result.total_failed;
            break;
        case FaultStatus::Skipped:
            ++result.total_skipped;
            break;
        case FaultStatus::Cancelled:
            ++result.total_cancelled;
            result.total_injected_delay += entry.actual_delay;
            break;
        }
    }
    return result;
}

void FaultHistory::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    operations_.clear();
}

bool FaultDecisionEngine::matches(
    const FaultRule& rule, const FaultContext& context,
    std::uint64_t matching_trigger_count) {
    if (!rule.enabled || !rule.config.enabled ||
        !operation_matches_type(rule.config.type, context.operation)) {
        return false;
    }
    if (rule.config.type == FaultType::FileCreationFailure &&
        !context.creates_resource) {
        return false;
    }
    const auto operation = effective_operation(rule);
    if (operation && *operation != context.operation) return false;
    const auto resource = effective_resource(rule);
    if (!resource.empty() && resource != context.resource_id) return false;
    const auto offset = effective_offset(rule);
    if (offset && !contains_offset(context, *offset)) return false;
    const auto page = effective_page(rule);
    if (page && (!context.page_number || *page != *context.page_number)) return false;
    if (rule.operation_sequence &&
        *rule.operation_sequence != context.operation_sequence) {
        return false;
    }
    const auto trigger = effective_trigger_count(rule);
    return !trigger || *trigger == matching_trigger_count;
}

FaultScheduler::FaultScheduler(std::shared_ptr<FaultHistory> history, std::uint64_t seed)
    : random_(seed), seed_(seed), history_(std::move(history)) {}

Result<void> FaultScheduler::add_rule(FaultRule rule) {
    auto validation = FaultPolicy::validate(rule.config);
    if (!validation) return validation.error();
    const auto configured_operation = effective_operation(rule);
    if (configured_operation &&
        !operation_matches_type(rule.config.type, *configured_operation)) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Rule operation is incompatible with its selected fault type.");
    }
    if (rule.id.empty()) rule.id = generate_uuid();
    if (rule.maximum_triggers == 0) {
        return fault_error(ErrorCode::InvalidArgument,
                           "A fault rule must allow at least one trigger.");
    }
    std::lock_guard lock(mutex_);
    if (std::any_of(rules_.begin(), rules_.end(),
                    [&rule](const FaultRule& existing) { return existing.id == rule.id; })) {
        return fault_error(ErrorCode::DuplicateRegistration,
                           "Fault rule identifier is already registered: " + rule.id);
    }
    FaultExecution scheduled;
    scheduled.fault_id = rule.id;
    scheduled.type = rule.config.type;
    scheduled.status = FaultStatus::Scheduled;
    scheduled.severity = rule.config.severity;
    scheduled.description = "Fault rule scheduled.";
    history_->record(std::move(scheduled));
    if (rule.config.random_seed) {
        rule_random_.insert_or_assign(
            rule.id, std::mt19937_64(*rule.config.random_seed));
    }
    rules_.push_back(std::move(rule));
    std::stable_sort(rules_.begin(), rules_.end(),
        [](const FaultRule& left, const FaultRule& right) {
            return left.priority > right.priority;
        });
    return {};
}

bool FaultScheduler::cancel(std::string_view rule_id) {
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(rules_.begin(), rules_.end(),
        [rule_id](const FaultRule& rule) { return rule.id == rule_id; });
    if (found == rules_.end()) return false;
    FaultExecution execution;
    execution.fault_id = found->id;
    execution.type = found->config.type;
    execution.status = FaultStatus::Cancelled;
    execution.severity = found->config.severity;
    execution.description = "Scheduled fault rule cancelled.";
    history_->record(std::move(execution));
    matching_counts_.erase(found->id);
    trigger_counts_.erase(found->id);
    rule_random_.erase(found->id);
    rules_.erase(found);
    return true;
}

Result<std::optional<FaultDecision>> FaultScheduler::evaluate(FaultContext context) {
    std::lock_guard lock(mutex_);
    FaultOperationTrace operation_trace;
    operation_trace.operation_sequence = context.operation_sequence;
    operation_trace.operation = context.operation;
    operation_trace.resource_id = context.resource_id;
    operation_trace.offset = context.offset;
    operation_trace.requested_bytes = context.requested_bytes;
    operation_trace.creates_resource = context.creates_resource;
    operation_trace.evaluated_at = std::chrono::system_clock::now();
    for (const auto& rule : rules_) {
        auto candidate = rule;
        candidate.enabled = true;
        candidate.config.enabled = true;
        candidate.trigger_count.reset();
        candidate.config.trigger_count.reset();
        const auto next_matching_count = matching_counts_[rule.id] + 1;
        if (!FaultDecisionEngine::matches(candidate, context, next_matching_count)) {
            continue;
        }
        matching_counts_[rule.id] = next_matching_count;
        auto record_skipped = [&](std::string description) {
            FaultExecution skipped;
            skipped.fault_id = rule.id;
            skipped.type = rule.config.type;
            skipped.status = FaultStatus::Skipped;
            skipped.severity = rule.config.severity;
            skipped.operation = context.operation;
            skipped.resource_id = context.resource_id;
            skipped.operation_sequence = context.operation_sequence;
            skipped.target_offset = context.offset.value_or(0);
            skipped.requested_bytes = context.requested_bytes;
            skipped.triggered_at = std::chrono::system_clock::now();
            skipped.completed_at = skipped.triggered_at;
            skipped.description = std::move(description);
            history_->record(std::move(skipped));
        };
        if (!rule.enabled || !rule.config.enabled) {
            record_skipped("Fault rule is disabled.");
            continue;
        }
        const auto exact_trigger = effective_trigger_count(rule);
        if (exact_trigger && *exact_trigger != next_matching_count) {
            record_skipped("Operation did not reach the configured matching trigger count.");
            continue;
        }
        if (trigger_counts_[rule.id] >= rule.maximum_triggers) {
            record_skipped("Fault rule trigger limit has been reached.");
            continue;
        }
        std::uniform_real_distribution<double> probability(0.0, 1.0);
        auto rule_random = rule_random_.find(rule.id);
        const auto value = rule_random == rule_random_.end()
            ? probability(random_) : probability(rule_random->second);
        if (value >= rule.config.probability) {
            record_skipped("Deterministic probability decision did not select this operation.");
            continue;
        }
        ++trigger_counts_[rule.id];
        operation_trace.selected_fault_id = rule.id;
        history_->record_operation(std::move(operation_trace));
        return std::optional<FaultDecision>{FaultDecision{rule, std::move(context)}};
    }
    history_->record_operation(std::move(operation_trace));
    return std::optional<FaultDecision>{};
}

std::vector<FaultRule> FaultScheduler::rules() const {
    std::lock_guard lock(mutex_);
    return rules_;
}

std::vector<std::byte> FaultScheduler::random_bytes(
    std::size_t count, std::string_view rule_id) {
    std::lock_guard lock(mutex_);
    std::vector<std::byte> result(count);
    auto rule_random = rule_random_.find(rule_id);
    for (auto& value : result) {
        const auto random_value = rule_random == rule_random_.end()
            ? random_() : rule_random->second();
        value = static_cast<std::byte>(random_value & 0xffU);
    }
    return result;
}

std::uint64_t FaultScheduler::random_uint64(std::string_view rule_id) {
    std::lock_guard lock(mutex_);
    const auto rule_random = rule_random_.find(rule_id);
    return rule_random == rule_random_.end() ? random_() : rule_random->second();
}

std::uint64_t FaultScheduler::next_operation_sequence() noexcept {
    return operation_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
}

void FaultScheduler::reset(std::uint64_t seed) {
    std::lock_guard lock(mutex_);
    seed_ = seed;
    random_.seed(seed_);
    rule_random_.clear();
    for (const auto& rule : rules_) {
        if (rule.config.random_seed) {
            rule_random_.emplace(rule.id, std::mt19937_64(*rule.config.random_seed));
        }
    }
    matching_counts_.clear();
    trigger_counts_.clear();
    operation_sequence_.store(0, std::memory_order_relaxed);
}

bool FaultScheduler::has_destructive_rules() const {
    std::lock_guard lock(mutex_);
    return std::any_of(rules_.begin(), rules_.end(), [](const FaultRule& rule) {
        return rule.enabled && rule.config.enabled && is_destructive(rule.config.type);
    });
}

FaultInjectedBackend::FaultInjectedBackend(std::shared_ptr<IStorageBackend> backend)
    : backend_(std::move(backend)) {}

FaultInjectedBackend::FaultInjectedBackend(
    std::shared_ptr<IStorageBackend> backend,
    std::shared_ptr<FaultScheduler> scheduler,
    std::shared_ptr<FaultHistory> history,
    bool destructive_authorized)
    : backend_(std::move(backend)), scheduler_(std::move(scheduler)),
      history_(std::move(history)), destructive_authorized_(destructive_authorized) {}

FaultInjectedBackend::~FaultInjectedBackend() {
    static_cast<void>(close());
}

Result<FaultContext> FaultInjectedBackend::context(
    StorageOperationType operation, std::optional<std::uint64_t> offset,
    std::uint64_t requested_bytes) const {
    FaultContext result;
    result.operation = operation;
    result.offset = offset;
    result.requested_bytes = requested_bytes;
    if (scheduler_) result.operation_sequence = scheduler_->next_operation_sequence();
    if (!backend_) {
        return fault_error(ErrorCode::InvalidState, "Fault backend has no underlying backend.");
    }
    auto metadata_result = backend_->metadata();
    if (!metadata_result) return metadata_result.error();
    if (metadata_result.value().path) {
        auto resolved = canonical_candidate(*metadata_result.value().path);
        if (!resolved) return resolved.error();
        result.resource_id = resolved.value().string();
    } else {
        result.resource_id = metadata_result.value().backend;
    }
    const auto registered_rules = scheduler_
        ? scheduler_->rules() : std::vector<FaultRule>{};
    if (offset && std::any_of(registered_rules.begin(), registered_rules.end(),
            [](const FaultRule& rule) {
                return effective_page(rule).has_value() &&
                       rule.enabled && rule.config.enabled;
            })) {
        std::array<std::byte, 100> header_bytes{};
        auto read = backend_->read_at(0, header_bytes);
        if (!read) return read.error();
        if (read.value() == header_bytes.size()) {
            auto parsed = SQLiteFormatParser::parse_header(header_bytes);
            if (parsed &&
                SQLiteFormatParser::valid_page_size(parsed.value().page_size)) {
                result.page_number = *offset / parsed.value().page_size + 1;
            }
        }
    }
    return result;
}

Result<std::optional<FaultDecision>> FaultInjectedBackend::decide(
    StorageOperationType operation, std::optional<std::uint64_t> offset,
    std::uint64_t requested_bytes) const {
    if (!scheduler_) return std::optional<FaultDecision>{};
    auto operation_context = context(operation, offset, requested_bytes);
    if (!operation_context) return operation_context.error();
    return scheduler_->evaluate(std::move(operation_context.value()));
}

Result<void> FaultInjectedBackend::wait_for_delay(
    const FaultDecision& decision, FaultExecution& execution, bool& cancelled) {
    const auto duration = decision.rule.config.delay;
    if (duration.count() == 0) return {};
    const auto started = std::chrono::steady_clock::now();
    std::unique_lock lock(delay_mutex_);
    const auto stop_token = delay_stop_source_.get_token();
    delay_condition_.wait_for(lock, stop_token, duration, [] {
        return false;
    });
    cancelled = stop_token.stop_requested();
    execution.actual_delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started);
    return {};
}

Result<void> FaultInjectedBackend::require_destructive_authorization(
    const FaultDecision& decision) const {
    if (is_destructive(decision.rule.config.type) && !destructive_authorized_) {
        return fault_error(ErrorCode::InvalidState,
                           "Destructive fault rejected: backend is outside the authorized disposable test environment.");
    }
    return {};
}

void FaultInjectedBackend::record(
    const FaultDecision& decision, FaultStatus status, std::uint64_t actual_bytes,
    std::string description, std::vector<std::uint64_t> offsets,
    std::chrono::nanoseconds delay) const {
    if (!history_) return;
    FaultExecution execution;
    execution.fault_id = decision.rule.id;
    execution.type = decision.rule.config.type;
    execution.status = status;
    execution.severity = decision.rule.config.severity;
    execution.operation = decision.context.operation;
    execution.resource_id = decision.context.resource_id;
    execution.operation_sequence = decision.context.operation_sequence;
    execution.target_offset = decision.context.offset.value_or(0);
    execution.requested_bytes = decision.context.requested_bytes;
    execution.actual_bytes = actual_bytes;
    execution.triggered_at = std::chrono::system_clock::now();
    if (status != FaultStatus::Triggered) execution.completed_at = execution.triggered_at;
    execution.actual_delay = delay;
    execution.corrupted_offsets = std::move(offsets);
    execution.description = std::move(description);
    history_->record(std::move(execution));
}

Result<std::vector<std::uint64_t>> FaultInjectedBackend::corrupt_write(
    const FaultDecision& decision, std::uint64_t offset,
    std::vector<std::byte>& bytes) {
    const auto& config = decision.rule.config;
    std::vector<std::uint64_t> modified;
    if (config.type == FaultType::RandomCorruption) {
        const auto count = std::min<std::uint64_t>(config.corruption_length, bytes.size());
        if (count == 0) return modified;
        auto masks = scheduler_->random_bytes(
            static_cast<std::size_t>(count), decision.rule.id);
        std::set<std::size_t> selected;
        const auto bound = bytes.size();
        for (std::size_t index = 0; index < count; ++index) {
            std::size_t position = static_cast<std::size_t>(
                scheduler_->random_uint64(decision.rule.id) % bound);
            while (selected.contains(position)) {
                position = (position + 1) % bound;
            }
            selected.insert(position);
        }
        std::size_t random_index = 0;
        for (const auto position : selected) {
            auto mask = masks[random_index++];
            if (mask == std::byte{0}) mask = std::byte{1};
            bytes[position] ^= mask;
            modified.push_back(offset + position);
        }
        return modified;
    }

    std::uint64_t target_start = 0;
    std::uint64_t target_length = config.corruption_length;
    if (config.type == FaultType::TargetedCorruption) {
        target_start = *config.target_offset;
    } else if (config.type == FaultType::PageHeaderCorruption) {
        std::array<std::byte, 100> database_header{};
        auto header_read = backend_->read_at(0, database_header);
        if (!header_read) return header_read.error();
        if (header_read.value() != database_header.size()) {
            return fault_error(ErrorCode::InvalidArgument,
                               "Page-header corruption requires a complete SQLite database header.");
        }
        auto parsed_database = SQLiteFormatParser::parse_header(database_header);
        if (!parsed_database) return parsed_database.error();
        const auto page_size = parsed_database.value().page_size;
        const auto page_number = *config.target_page;
        if (!SQLiteFormatParser::valid_page_size(page_size)) {
            return fault_error(ErrorCode::InvalidArgument,
                               "SQLite page header has an invalid page size.");
        }
        auto page_offset_result = SQLiteFormatParser::page_offset(page_size, page_number);
        auto page_boundary_result = SQLiteFormatParser::page_boundary(page_size, page_number);
        if (!page_offset_result) return page_offset_result.error();
        if (!page_boundary_result) return page_boundary_result.error();
        auto total_size = backend_->size();
        if (!total_size) return total_size.error();
        if (page_boundary_result.value() > total_size.value()) {
            return fault_error(ErrorCode::InvalidArgument,
                               "Target SQLite page is outside the backend's complete page range.");
        }
        std::vector<std::byte> page(page_size);
        auto page_read = backend_->read_at(page_offset_result.value(), page);
        if (!page_read) return page_read.error();
        if (page_read.value() != page.size()) {
            return fault_error(ErrorCode::InvalidArgument,
                               "Target SQLite page is truncated.");
        }
        auto page_analysis = SQLitePageHeaderParser::parse(
            static_cast<std::uint32_t>(page_number), page, page_size);
        if (!page_analysis) return page_analysis.error();
        if (config.page_header_field == SQLitePageHeaderField::RightmostChildPage &&
            !page_analysis.value().is_interior()) {
            return fault_error(ErrorCode::InvalidArgument,
                               "Rightmost-child corruption requires an interior B-tree page.");
        }
        const auto header_offset = page_number == 1 ? 100U : 0U;
        target_start = page_offset_result.value() + header_offset +
                       page_header_field_offset(config.page_header_field);
        target_length = page_header_field_size(config.page_header_field);
    } else if (config.type == FaultType::ChecksumCorruption ||
               config.type == FaultType::WALRecordCorruption) {
        std::array<std::byte, 32> wal_header_bytes{};
        auto header_read = backend_->read_at(0, wal_header_bytes);
        if (!header_read) return header_read.error();
        if (header_read.value() != wal_header_bytes.size()) {
            return fault_error(ErrorCode::InvalidArgument,
                               "WAL corruption requires a complete SQLite WAL header.");
        }
        SQLiteWALParser parser;
        auto parsed_header = parser.parse_header(wal_header_bytes);
        if (!parsed_header) return parsed_header.error();
        const auto& wal_header = parsed_header.value();
        const auto frame_number = config.wal_frame_number.value_or(0);
        auto file_size = backend_->size();
        if (!file_size) return file_size.error();
        if (config.type == FaultType::ChecksumCorruption) {
            if (frame_number == 0) {
                target_start = 24;
                target_length = 8;
            } else {
                const auto frame_size = 24ULL + wal_header.page_size;
                if (frame_number - 1 >
                    (std::numeric_limits<std::uint64_t>::max() - 32ULL) / frame_size) {
                    return fault_error(ErrorCode::InvalidArgument, "WAL frame offset overflow.");
                }
                const auto frame_offset = 32ULL + (frame_number - 1) * frame_size;
                if (frame_size > file_size.value() - std::min(file_size.value(), frame_offset)) {
                    return fault_error(ErrorCode::InvalidArgument,
                                       "WAL checksum target is not a complete frame.");
                }
                target_start = frame_offset + 16;
                target_length = 8;
            }
        } else {
            if (frame_number == 0) {
                return fault_error(ErrorCode::InvalidArgument,
                                   "WAL record corruption frame numbers start at one.");
            }
            const auto frame_size = 24ULL + wal_header.page_size;
            if (frame_number - 1 >
                (std::numeric_limits<std::uint64_t>::max() - 32ULL) / frame_size) {
                return fault_error(ErrorCode::InvalidArgument, "WAL frame offset overflow.");
            }
            const auto frame_offset = 32ULL + (frame_number - 1) * frame_size;
            if (frame_offset > file_size.value() ||
                frame_size > file_size.value() - frame_offset) {
                return fault_error(ErrorCode::InvalidArgument,
                                   "WAL corruption target is not a complete frame.");
            }
            switch (config.wal_region) {
            case WALCorruptionRegion::FrameHeader:
                target_start = frame_offset;
                target_length = 8;
                break;
            case WALCorruptionRegion::Salt:
                target_start = frame_offset + 8;
                target_length = 8;
                break;
            case WALCorruptionRegion::Checksum:
                target_start = frame_offset + 16;
                target_length = 8;
                break;
            case WALCorruptionRegion::PageContent:
                target_start = frame_offset + 24;
                target_length = wal_header.page_size;
                break;
            }
        }
    }

    const auto request_length = static_cast<std::uint64_t>(bytes.size());
    const auto start = overlap_start(offset, request_length, target_start, target_length);
    const auto count = overlap_length(offset, request_length, target_start, target_length);
    if (count == 0) return modified;
    const auto local_start = static_cast<std::size_t>(start - offset);
    const auto maximum = std::min<std::uint64_t>(count, config.corruption_length);
    apply_pattern(bytes, local_start, static_cast<std::size_t>(maximum),
                  config.corruption_pattern);
    for (std::uint64_t index = 0; index < maximum; ++index) {
        modified.push_back(start + index);
    }
    return modified;
}

Result<std::size_t> FaultInjectedBackend::read_at(
    std::uint64_t offset, std::span<std::byte> buffer) {
    if (!backend_) return fault_error(ErrorCode::InvalidState, "Underlying storage is absent.");
    auto decision_result = decide(StorageOperationType::Read, offset, buffer.size());
    if (!decision_result) return decision_result.error();
    const auto decision = std::move(decision_result.value());
    if (decision && decision->rule.config.type == FaultType::FileAccessFailure) {
        record(*decision, FaultStatus::Triggered, 0, "Injected file access failure.");
        auto error = fault_error(ErrorCode::IoError, "Injected file access failure.");
        record(*decision, FaultStatus::Failed, 0, error.message);
        return error;
    }
    if (decision && decision->rule.config.type == FaultType::InvalidOffset) {
        record(*decision, FaultStatus::Triggered, 0, "Injected invalid read offset.");
        auto error = fault_error(ErrorCode::InvalidArgument, "Injected invalid storage read offset.");
        record(*decision, FaultStatus::Failed, 0, error.message);
        return error;
    }
    return backend_->read_at(offset, buffer);
}

Result<std::size_t> FaultInjectedBackend::write_at(
    std::uint64_t offset, std::span<const std::byte> buffer) {
    if (!backend_) return fault_error(ErrorCode::InvalidState, "Underlying storage is absent.");
    auto decision_result = decide(StorageOperationType::Write, offset, buffer.size());
    if (!decision_result) return decision_result.error();
    auto decision = std::move(decision_result.value());
    if (!decision) return backend_->write_at(offset, buffer);
    const auto type = decision->rule.config.type;
    if (type == FaultType::FileAccessFailure || type == FaultType::WriteFailure ||
        type == FaultType::RejectedWrite || type == FaultType::InvalidOffset) {
        record(*decision, FaultStatus::Triggered, 0,
               type == FaultType::RejectedWrite ? "Write rejected before reaching backend."
                                                : "Injected write failure.");
        auto error = fault_error(
            type == FaultType::InvalidOffset ? ErrorCode::InvalidArgument :
                (type == FaultType::RejectedWrite ? ErrorCode::InvalidState : ErrorCode::IoError),
            type == FaultType::RejectedWrite ? "Storage write rejected by fault rule."
                                             : "Storage write failed by fault rule.");
        if (type == FaultType::RejectedWrite) {
            error.metadata.emplace("rejected", "true");
        }
        record(*decision, FaultStatus::Failed, 0, error.message);
        return error;
    }
    if (type == FaultType::PartialWrite || type == FaultType::TruncatedWrite ||
        type == FaultType::DiskFull || is_corruption(type)) {
        auto authorization = require_destructive_authorization(*decision);
        if (!authorization) {
            record(*decision, FaultStatus::Triggered, 0, authorization.error().message);
            record(*decision, FaultStatus::Failed, 0, authorization.error().message);
            return authorization.error();
        }
    }
    if (type == FaultType::DelayedWrite) {
        FaultExecution delay_execution;
        record(*decision, FaultStatus::Triggered, 0, "Delayed write started.");
        bool cancelled = false;
        auto waited = wait_for_delay(*decision, delay_execution, cancelled);
        if (!waited) return waited.error();
        if (cancelled) {
            record(*decision, FaultStatus::Cancelled, 0, "Delayed write was cancelled.",
                   {}, delay_execution.actual_delay);
            return backend_->write_at(offset, buffer);
        }
        auto result = backend_->write_at(offset, buffer);
        record(*decision, result ? FaultStatus::Executed : FaultStatus::Failed,
               result ? result.value() : 0,
               result ? "Write completed after configured delay." : result.error().message,
               {}, delay_execution.actual_delay);
        return result;
    }
    if (is_corruption(type)) {
        std::vector<std::byte> payload(buffer.begin(), buffer.end());
        auto modified = corrupt_write(*decision, offset, payload);
        if (!modified) {
            record(*decision, FaultStatus::Triggered, 0, modified.error().message);
            record(*decision, FaultStatus::Failed, 0, modified.error().message);
            return modified.error();
        }
        if (modified.value().empty()) {
            record(*decision, FaultStatus::Skipped, 0,
                   "Write did not overlap the validated corruption target.");
            return backend_->write_at(offset, buffer);
        }
        record(*decision, FaultStatus::Triggered, 0, "Corrupt write started.");
        auto result = backend_->write_at(offset, payload);
        record(*decision, result ? FaultStatus::Executed : FaultStatus::Failed,
               result ? result.value() : 0,
               result ? "Configured bytes corrupted in disposable storage."
                      : result.error().message,
               modified.value());
        return result;
    }
    if (type == FaultType::PartialWrite || type == FaultType::TruncatedWrite) {
        std::uint64_t count = 0;
        if (type == FaultType::PartialWrite) {
            if (decision->rule.config.partial_write_bytes) {
                count = *decision->rule.config.partial_write_bytes;
            } else {
                count = static_cast<std::uint64_t>(
                    static_cast<long double>(buffer.size()) *
                    decision->rule.config.partial_write_fraction);
            }
            if (decision->rule.config.maximum_partial_write_bytes) {
                count = std::min(count, *decision->rule.config.maximum_partial_write_bytes);
            }
        } else {
            count = *decision->rule.config.truncation_length;
        }
        count = std::min<std::uint64_t>(count, buffer.size());
        record(*decision, FaultStatus::Triggered, 0,
               type == FaultType::PartialWrite ? "Partial write started."
                                               : "Truncated write started.");
        auto result = backend_->write_at(offset,
            buffer.first(static_cast<std::size_t>(count)));
        record(*decision, result ? FaultStatus::Executed : FaultStatus::Failed,
               result ? result.value() : 0,
               result ? "Only the configured prefix was persisted."
                      : result.error().message);
        return result;
    }
    if (type == FaultType::DiskFull) {
        auto current_size = backend_->size();
        if (!current_size) return current_size.error();
        const auto capacity = *decision->rule.config.maximum_capacity;
        const auto start = decision->context.operation == StorageOperationType::Append
            ? current_size.value() : offset;
        const auto current_end = start > current_size.value()
            ? start : current_size.value();
        if (current_end >= capacity) {
            record(*decision, FaultStatus::Triggered, 0, "Virtual capacity exhausted.");
            auto error = fault_error(ErrorCode::IoError,
                                     "Simulated disk-full capacity rejected storage write.");
            record(*decision, FaultStatus::Failed, 0, error.message);
            return error;
        }
        const auto remaining = capacity - current_end;
        if (buffer.size() <= remaining) {
            record(*decision, FaultStatus::Skipped, 0,
                   "Virtual capacity was sufficient; write delegated unchanged.");
            return backend_->write_at(start, buffer);
        }
        record(*decision, FaultStatus::Triggered, 0,
               "Virtual capacity permits only a prefix of this write.");
        auto result = backend_->write_at(start,
            buffer.first(static_cast<std::size_t>(remaining)));
        record(*decision, result ? FaultStatus::Executed : FaultStatus::Failed,
               result ? result.value() : 0,
               result ? "Virtual disk-full condition persisted the remaining capacity."
                      : result.error().message);
        return result;
    }
    return backend_->write_at(offset, buffer);
}

Result<std::size_t> FaultInjectedBackend::append(std::span<const std::byte> buffer) {
    if (!backend_) return fault_error(ErrorCode::InvalidState, "Underlying storage is absent.");
    auto size_result = backend_->size();
    if (!size_result) return size_result.error();
    const auto offset = size_result.value();
    auto decision_result = decide(StorageOperationType::Append, offset, buffer.size());
    if (!decision_result) return decision_result.error();
    auto decision = std::move(decision_result.value());
    if (!decision) return backend_->append(buffer);
    const auto type = decision->rule.config.type;
    if (type == FaultType::FileAccessFailure || type == FaultType::WriteFailure ||
        type == FaultType::RejectedWrite || type == FaultType::InvalidOffset) {
        record(*decision, FaultStatus::Triggered, 0, "Injected append rejection.");
        auto error = fault_error(ErrorCode::IoError, "Storage append failed by fault rule.");
        record(*decision, FaultStatus::Failed, 0, error.message);
        return error;
    }
    if (type == FaultType::DiskFull || type == FaultType::PartialWrite ||
        type == FaultType::TruncatedWrite || is_corruption(type)) {
        auto authorization = require_destructive_authorization(*decision);
        if (!authorization) {
            record(*decision, FaultStatus::Triggered, 0, authorization.error().message);
            record(*decision, FaultStatus::Failed, 0, authorization.error().message);
            return authorization.error();
        }
    }
    if (type == FaultType::DelayedWrite) {
        record(*decision, FaultStatus::Triggered, 0, "Delayed append started.");
        FaultExecution execution;
        bool cancelled = false;
        auto waited = wait_for_delay(*decision, execution, cancelled);
        if (!waited) return waited.error();
        if (cancelled) {
            record(*decision, FaultStatus::Cancelled, 0, "Delayed append was cancelled.",
                   {}, execution.actual_delay);
            return backend_->append(buffer);
        }
        auto result = backend_->append(buffer);
        record(*decision, result ? FaultStatus::Executed : FaultStatus::Failed,
               result ? result.value() : 0,
               result ? "Append completed after configured delay." : result.error().message,
               {}, execution.actual_delay);
        return result;
    }
    if (type == FaultType::PartialWrite || type == FaultType::TruncatedWrite) {
        const auto configured = type == FaultType::PartialWrite
            ? decision->rule.config.partial_write_bytes.value_or(
                  static_cast<std::uint64_t>(static_cast<long double>(buffer.size()) *
                      decision->rule.config.partial_write_fraction))
            : *decision->rule.config.truncation_length;
        const auto limited = type == FaultType::PartialWrite &&
                             decision->rule.config.maximum_partial_write_bytes
            ? std::min(configured, *decision->rule.config.maximum_partial_write_bytes)
            : configured;
        const auto count = std::min<std::uint64_t>(limited, buffer.size());
        record(*decision, FaultStatus::Triggered, 0, "Short append started.");
        auto result = backend_->append(buffer.first(static_cast<std::size_t>(count)));
        record(*decision, result ? FaultStatus::Executed : FaultStatus::Failed,
               result ? result.value() : 0,
               result ? "Only the configured append prefix was persisted."
                      : result.error().message);
        return result;
    }
    if (type == FaultType::DiskFull) {
        const auto capacity = *decision->rule.config.maximum_capacity;
        const auto remaining = size_result.value() < capacity
            ? capacity - size_result.value() : 0;
        if (remaining == 0) {
            record(*decision, FaultStatus::Triggered, 0, "Virtual capacity exhausted.");
            auto error = fault_error(ErrorCode::IoError,
                                     "Simulated disk-full capacity rejected append.");
            record(*decision, FaultStatus::Failed, 0, error.message);
            return error;
        }
        if (buffer.size() <= remaining) {
            record(*decision, FaultStatus::Skipped, 0,
                   "Virtual capacity was sufficient; append delegated unchanged.");
            return backend_->append(buffer);
        }
        record(*decision, FaultStatus::Triggered, 0, "Virtual capacity limits append length.");
        auto result = backend_->append(buffer.first(static_cast<std::size_t>(remaining)));
        record(*decision, result ? FaultStatus::Executed : FaultStatus::Failed,
               result ? result.value() : 0,
               result ? "Virtual disk-full condition persisted remaining capacity."
                      : result.error().message);
        return result;
    }
    if (is_corruption(type)) {
        std::vector<std::byte> payload(buffer.begin(), buffer.end());
        auto modified = corrupt_write(*decision, offset, payload);
        if (!modified) return modified.error();
        if (modified.value().empty()) {
            record(*decision, FaultStatus::Skipped, 0,
                   "Append did not overlap the validated corruption target.");
            return backend_->append(buffer);
        }
        record(*decision, FaultStatus::Triggered, 0, "Corrupt append started.");
        auto result = backend_->append(payload);
        record(*decision, result ? FaultStatus::Executed : FaultStatus::Failed,
               result ? result.value() : 0,
               result ? "Configured bytes corrupted in disposable storage."
                      : result.error().message,
               modified.value());
        return result;
    }
    return backend_->append(buffer);
}

Result<void> FaultInjectedBackend::truncate(std::uint64_t requested_size) {
    if (!backend_) return fault_error(ErrorCode::InvalidState, "Underlying storage is absent.");
    auto decision_result = decide(StorageOperationType::Truncate, std::nullopt, requested_size);
    if (!decision_result) return decision_result.error();
    auto decision = std::move(decision_result.value());
    if (!decision) return backend_->truncate(requested_size);
    const auto type = decision->rule.config.type;
    if (type == FaultType::FileAccessFailure) {
        record(*decision, FaultStatus::Triggered, 0, "Injected truncation access failure.");
        auto error = fault_error(ErrorCode::IoError, "Storage truncation access denied by rule.");
        record(*decision, FaultStatus::Failed, 0, error.message);
        return error;
    }
    if (type != FaultType::FileTruncation) return backend_->truncate(requested_size);
    auto authorization = require_destructive_authorization(*decision);
    if (!authorization) {
        record(*decision, FaultStatus::Triggered, 0, authorization.error().message);
        record(*decision, FaultStatus::Failed, 0, authorization.error().message);
        return authorization.error();
    }
    record(*decision, FaultStatus::Triggered, 0, "Controlled file truncation started.");
    auto result = backend_->truncate(*decision->rule.config.target_size);
    record(*decision, result ? FaultStatus::Executed : FaultStatus::Failed,
           result ? *decision->rule.config.target_size : 0,
           result ? "File truncated to the explicitly configured target size."
                  : result.error().message);
    return result;
}

Result<void> FaultInjectedBackend::sync() {
    if (!backend_) return fault_error(ErrorCode::InvalidState, "Underlying storage is absent.");
    auto decision_result = decide(StorageOperationType::Sync, std::nullopt, 0);
    if (!decision_result) return decision_result.error();
    auto decision = std::move(decision_result.value());
    if (!decision) return backend_->sync();
    if (decision->rule.config.type == FaultType::SyncFailure ||
        decision->rule.config.type == FaultType::InterruptedSync ||
        decision->rule.config.type == FaultType::FileAccessFailure) {
        const auto interrupted = decision->rule.config.type == FaultType::InterruptedSync;
        record(*decision, FaultStatus::Triggered, 0,
               interrupted ? "Synchronization interruption injected."
                           : "Synchronization failure injected.");
        auto error = fault_error(ErrorCode::IoError,
            interrupted ? "Synchronization interrupted before completion."
                        : "Storage synchronization failed by fault rule.");
        if (interrupted) error.metadata.emplace("interrupted", "true");
        record(*decision, FaultStatus::Failed, 0, error.message);
        return error;
    }
    return backend_->sync();
}

Result<std::uint64_t> FaultInjectedBackend::size() const {
    if (!backend_) {
        return fault_error(ErrorCode::InvalidState, "Underlying storage is absent.");
    }
    auto decision_result = decide(StorageOperationType::Metadata, std::nullopt, 0);
    if (!decision_result) return decision_result.error();
    auto decision = std::move(decision_result.value());
    if (decision && decision->rule.config.type == FaultType::FileAccessFailure) {
        record(*decision, FaultStatus::Triggered, 0, "Injected metadata access failure.");
        auto error = fault_error(ErrorCode::IoError, "Storage metadata access denied by rule.");
        record(*decision, FaultStatus::Failed, 0, error.message);
        return error;
    }
    return backend_->size();
}

Result<StorageMetadata> FaultInjectedBackend::metadata() const {
    if (!backend_) {
        return fault_error(ErrorCode::InvalidState, "Underlying storage is absent.");
    }
    auto decision_result = decide(StorageOperationType::Metadata, std::nullopt, 0);
    if (!decision_result) return decision_result.error();
    auto decision = std::move(decision_result.value());
    if (decision && decision->rule.config.type == FaultType::FileAccessFailure) {
        record(*decision, FaultStatus::Triggered, 0, "Injected metadata access failure.");
        auto error = fault_error(ErrorCode::IoError, "Storage metadata access denied by rule.");
        record(*decision, FaultStatus::Failed, 0, error.message);
        return error;
    }
    return backend_->metadata();
}

Result<void> FaultInjectedBackend::close() {
    if (!backend_) return {};
    auto decision_result = decide(StorageOperationType::Close, std::nullopt, 0);
    if (!decision_result) return decision_result.error();
    auto decision = std::move(decision_result.value());
    if (decision && decision->rule.config.type == FaultType::FileAccessFailure) {
        record(*decision, FaultStatus::Triggered, 0, "Injected close access failure.");
        auto error = fault_error(ErrorCode::IoError, "Storage close was rejected by fault rule.");
        record(*decision, FaultStatus::Failed, 0, error.message);
        return error;
    }
    return backend_->close();
}

bool FaultInjectedBackend::is_open() const noexcept {
    return backend_ && backend_->is_open();
}

StorageAccess FaultInjectedBackend::access() const noexcept {
    return backend_ ? backend_->access() : StorageAccess::ReadOnly;
}

void FaultInjectedBackend::cancel_delays() noexcept {
    {
        std::lock_guard lock(delay_mutex_);
        delay_stop_source_.request_stop();
        delay_stop_source_ = std::stop_source{};
    }
    delay_condition_.notify_all();
}

FaultManager::FaultManager(std::shared_ptr<FaultScheduler> scheduler)
    : scheduler_(std::move(scheduler)) {}

Result<void> FaultManager::add_rule(FaultRule rule) {
    if (!scheduler_) {
        return fault_error(ErrorCode::InvalidState, "Fault scheduler is unavailable.");
    }
    return scheduler_->add_rule(std::move(rule));
}

bool FaultManager::remove_rule(std::string_view rule_id) {
    return scheduler_ && scheduler_->cancel(rule_id);
}

std::vector<FaultRule> FaultManager::rules() const {
    return scheduler_ ? scheduler_->rules() : std::vector<FaultRule>{};
}

FaultInjector::FaultInjector(Runtime& runtime)
    : runtime_(runtime), history_(std::make_shared<FaultHistory>()),
      scheduler_(std::make_shared<FaultScheduler>(history_)),
      manager_(scheduler_) {}

FaultInjector::~FaultInjector() {
    static_cast<void>(shutdown());
}

std::string_view FaultInjector::name() const noexcept {
    return "FaultInjector";
}

Result<void> FaultInjector::initialize() {
    std::lock_guard lock(mutex_);
    if (initialized_) {
        return fault_error(ErrorCode::AlreadyInitialized, "Fault injector is already initialized.");
    }
    static_cast<void>(runtime_.capabilities().register_capability("fault_injection"));
    static_cast<void>(runtime_.capabilities().register_capability("fault_injection.deterministic"));
    static_cast<void>(runtime_.capabilities().register_capability("fault_injection.storage_backend"));
    initialized_ = true;
    return {};
}

Result<void> FaultInjector::shutdown() {
    {
        std::lock_guard lock(mutex_);
        initialized_ = false;
    }
    cancel_pending_delays();
    return {};
}

bool FaultInjector::is_initialized() const noexcept {
    std::lock_guard lock(mutex_);
    return initialized_;
}

Result<void> FaultInjector::configure_environment(TestEnvironment environment) {
    if (!environment.isolated || !environment.disposable) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Fault environment must be explicitly isolated and disposable.");
    }
    if (environment.root_directory.empty()) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Fault environment root directory must be specified.");
    }
    std::error_code error;
    if (!std::filesystem::is_directory(environment.root_directory, error) || error) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Fault environment root must already exist as a directory.");
    }
    const std::filesystem::directory_iterator begin(environment.root_directory, error);
    if (error) {
        return fault_error(ErrorCode::IoError,
                           "Unable to inspect fault environment root: " + error.message());
    }
    if (begin != std::filesystem::directory_iterator{}) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Fault environment root must be empty when it is authorized.");
    }
    auto canonical = std::filesystem::canonical(environment.root_directory, error);
    if (error) {
        return fault_error(ErrorCode::IoError,
                           "Unable to canonicalize fault environment root: " + error.message());
    }
    if (canonical == canonical.root_path()) {
        return fault_error(ErrorCode::InvalidArgument,
                           "A filesystem root cannot be authorized as a test environment.");
    }
    std::lock_guard lock(mutex_);
    if (environment_configured_) {
        return fault_error(ErrorCode::InvalidState,
                           "Fault environment cannot be reconfigured after authorization.");
    }
    environment_ = std::move(environment);
    canonical_root_ = std::move(canonical);
    environment_configured_ = true;
    return {};
}

Result<void> FaultInjector::set_random_seed(std::uint64_t seed) {
    scheduler_->reset(seed);
    return {};
}

Result<void> FaultInjector::add_rule(FaultRule rule) {
    auto validation = FaultPolicy::validate(rule.config);
    if (!validation) return validation.error();
    if (is_destructive(rule.config.type) && !environment_allows_destructive()) {
        return fault_error(ErrorCode::InvalidState,
                           "Destructive fault rules require an explicitly authorized disposable test environment.");
    }
    return manager_.add_rule(std::move(rule));
}

bool FaultInjector::remove_rule(std::string_view rule_id) {
    return manager_.remove_rule(rule_id);
}

Result<void> FaultInjector::reset_scenario(std::uint64_t seed) {
    cancel_pending_delays();
    scheduler_->reset(seed);
    history_->clear();
    for (const auto& rule : scheduler_->rules()) {
        FaultExecution scheduled;
        scheduled.fault_id = rule.id;
        scheduled.type = rule.config.type;
        scheduled.status = FaultStatus::Scheduled;
        scheduled.severity = rule.config.severity;
        scheduled.description = "Fault rule scheduled for reset scenario.";
        history_->record(std::move(scheduled));
    }
    return {};
}

Result<std::shared_ptr<StorageFile>> FaultInjector::open_file(
    StorageManager& storage_manager, const std::filesystem::path& path,
    const StorageOpenOptions& options) {
    if (!is_initialized()) {
        return fault_error(ErrorCode::InvalidState,
                           "Fault injector must be initialized before opening storage.");
    }
    const bool writable = options.access != StorageAccess::ReadOnly;
    if (options.access == StorageAccess::Truncate &&
        !environment_allows_destructive()) {
        return fault_error(ErrorCode::InvalidState,
                           "Truncate-on-open requires explicit destructive test authorization.");
    }
    std::filesystem::path resolved_path = path;
    if (writable || scheduler_->has_destructive_rules() ||
        options.access == StorageAccess::Create ||
        options.access == StorageAccess::CreateNew ||
        options.access == StorageAccess::Truncate) {
        auto validated = validate_environment_path(path, false);
        if (!validated) return validated.error();
        resolved_path = validated.value();
    }
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(resolved_path, exists_error);
    if (exists_error) {
        return fault_error(ErrorCode::IoError,
                           "Unable to inspect storage path: " + exists_error.message());
    }
    const bool new_file = options.access == StorageAccess::CreateNew ||
        (options.access == StorageAccess::Create && !exists);
    FaultContext context;
    context.operation = StorageOperationType::Open;
    context.resource_id = resolved_path.string();
    context.operation_sequence = scheduler_->next_operation_sequence();
    context.creates_resource = new_file;
    auto decision_result = scheduler_->evaluate(context);
    if (!decision_result) return decision_result.error();
    auto decision = std::move(decision_result.value());
    if (decision && (decision->rule.config.type == FaultType::FileCreationFailure ||
                     decision->rule.config.type == FaultType::FileAccessFailure)) {
        FaultExecution triggered;
        triggered.fault_id = decision->rule.id;
        triggered.type = decision->rule.config.type;
        triggered.status = FaultStatus::Triggered;
        triggered.severity = decision->rule.config.severity;
        triggered.operation = StorageOperationType::Open;
        triggered.resource_id = context.resource_id;
        triggered.operation_sequence = context.operation_sequence;
        triggered.triggered_at = std::chrono::system_clock::now();
        triggered.description = "File open/create failure injected before backend creation.";
        history_->record(std::move(triggered));
        FaultExecution failed;
        failed.fault_id = decision->rule.id;
        failed.type = decision->rule.config.type;
        failed.status = FaultStatus::Failed;
        failed.severity = decision->rule.config.severity;
        failed.operation = StorageOperationType::Open;
        failed.resource_id = context.resource_id;
        failed.operation_sequence = context.operation_sequence;
        failed.triggered_at = std::chrono::system_clock::now();
        failed.completed_at = failed.triggered_at;
        failed.description = "Storage open rejected by configured fault.";
        history_->record(std::move(failed));
        return fault_error(ErrorCode::IoError, "Storage file creation/access failed by fault rule.");
    }
    auto backend = NativeFileBackend::open(resolved_path, options);
    if (!backend) return backend.error();
    auto metadata = backend.value()->metadata();
    if (!metadata) {
        static_cast<void>(backend.value()->close());
        return metadata.error();
    }
    if (writable) {
        if (!metadata.value().path) {
            static_cast<void>(backend.value()->close());
            return fault_error(ErrorCode::InvalidState,
                               "Writable native backend did not report its resolved path.");
        }
        auto validated = validate_environment_path(*metadata.value().path, true);
        if (!validated) {
            static_cast<void>(backend.value()->close());
            return validated.error();
        }
    }
    auto wrapped = std::shared_ptr<FaultInjectedBackend>(new FaultInjectedBackend(
        backend.value(), scheduler_, history_,
        destructive_authorized_for(metadata.value())));
    track_backend(wrapped);
    auto file = storage_manager.create_storage(std::move(wrapped));
    if (!file) {
        static_cast<void>(backend.value()->close());
        return file.error();
    }
    if (new_file) {
        std::lock_guard lock(mutex_);
        created_files_.insert(resolved_path);
    }
    return file.value();
}

Result<std::shared_ptr<StorageFile>> FaultInjector::create_memory_storage(
    StorageManager& storage_manager, std::size_t capacity) {
    if (!is_initialized()) {
        return fault_error(ErrorCode::InvalidState,
                           "Fault injector must be initialized before creating storage.");
    }
    FaultContext context;
    context.operation = StorageOperationType::Open;
    context.resource_id = "memory";
    context.operation_sequence = scheduler_->next_operation_sequence();
    context.creates_resource = true;
    auto decision_result = scheduler_->evaluate(context);
    if (!decision_result) return decision_result.error();
    auto decision = std::move(decision_result.value());
    if (decision && (decision->rule.config.type == FaultType::FileCreationFailure ||
                     decision->rule.config.type == FaultType::FileAccessFailure)) {
        FaultExecution triggered;
        triggered.fault_id = decision->rule.id;
        triggered.type = decision->rule.config.type;
        triggered.status = FaultStatus::Triggered;
        triggered.severity = decision->rule.config.severity;
        triggered.operation = StorageOperationType::Open;
        triggered.resource_id = "memory";
        triggered.operation_sequence = context.operation_sequence;
        triggered.triggered_at = std::chrono::system_clock::now();
        triggered.description = "Memory resource creation failure injected.";
        history_->record(std::move(triggered));
        FaultExecution failed;
        failed.fault_id = decision->rule.id;
        failed.type = decision->rule.config.type;
        failed.status = FaultStatus::Failed;
        failed.severity = decision->rule.config.severity;
        failed.operation = StorageOperationType::Open;
        failed.resource_id = "memory";
        failed.operation_sequence = context.operation_sequence;
        failed.triggered_at = std::chrono::system_clock::now();
        failed.completed_at = failed.triggered_at;
        failed.description = "Memory storage creation rejected by fault rule.";
        history_->record(std::move(failed));
        return fault_error(ErrorCode::IoError,
                           "Memory storage creation failed by fault rule.");
    }
    auto backend = std::make_shared<MemoryStorageBackend>(capacity);
    const bool authorized = environment_allows_destructive();
    auto wrapped = std::shared_ptr<FaultInjectedBackend>(
        new FaultInjectedBackend(backend, scheduler_, history_, authorized));
    track_backend(wrapped);
    return storage_manager.create_storage(std::move(wrapped));
}

Result<std::shared_ptr<StorageFile>> FaultInjector::wrap_storage(
    StorageManager& storage_manager, std::shared_ptr<IStorageBackend> backend) {
    if (!is_initialized()) {
        return fault_error(ErrorCode::InvalidState,
                           "Fault injector must be initialized before wrapping storage.");
    }
    if (!backend || !backend->is_open()) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Only an open storage backend can be fault-decorated.");
    }
    auto metadata = backend->metadata();
    if (!metadata) return metadata.error();
    const bool authorized = destructive_authorized_for(metadata.value());
    if (scheduler_->has_destructive_rules() && !authorized) {
        return fault_error(ErrorCode::InvalidState,
                           "Refusing to attach destructive rules to storage outside the disposable test environment.");
    }
    auto wrapped = std::shared_ptr<FaultInjectedBackend>(new FaultInjectedBackend(
        std::move(backend), scheduler_, history_, authorized));
    track_backend(wrapped);
    return storage_manager.create_storage(std::move(wrapped));
}

Result<std::filesystem::path> FaultInjector::create_test_copy(
    StorageManager& storage_manager, const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    auto safe_destination = validate_environment_path(destination, false);
    if (!safe_destination) return safe_destination.error();
    StorageOpenOptions source_options;
    source_options.access = StorageAccess::ReadOnly;
    auto source_file = storage_manager.open_file(source, source_options);
    if (!source_file) return source_file.error();
    auto snapshot = storage_manager.create_snapshot(source_file.value(), safe_destination.value());
    auto source_close = source_file.value()->close();
    if (!snapshot) {
        if (!source_close) {
            auto error = snapshot.error();
            error.metadata.emplace("source_close_error", source_close.error().message);
            return error;
        }
        return snapshot.error();
    }
    auto copy_close = snapshot.value().file->close();
    if (!source_close) return source_close.error();
    if (!copy_close) return copy_close.error();
    {
        std::lock_guard lock(mutex_);
        created_files_.insert(safe_destination.value());
    }
    return safe_destination.value();
}

Result<void> FaultInjector::cleanup_file(const std::filesystem::path& path) {
    auto validated = validate_environment_path(path, true);
    if (!validated) return validated.error();
    {
        std::lock_guard lock(mutex_);
        if (!created_files_.contains(validated.value())) {
            return fault_error(ErrorCode::InvalidState,
                               "Cleanup is limited to exact files created by this injector.");
        }
    }
    std::error_code error;
    const bool removed = std::filesystem::remove(validated.value(), error);
    if (error) {
        return fault_error(ErrorCode::IoError,
                           "Unable to remove test copy: " + error.message());
    }
    if (!removed) {
        return fault_error(ErrorCode::NotFound, "Test copy no longer exists.");
    }
    std::lock_guard lock(mutex_);
    created_files_.erase(validated.value());
    return {};
}

std::vector<FaultExecution> FaultInjector::history() const {
    return history_->entries();
}

std::vector<FaultOperationTrace> FaultInjector::operation_trace() const {
    return history_->operations();
}

FaultStatistics FaultInjector::statistics() const {
    return history_->statistics();
}

Result<FaultCapacityStatus> FaultInjector::capacity_status(
    const StorageFile& file, std::string_view rule_id) const {
    const auto registered_rules = scheduler_->rules();
    const auto rule = std::find_if(registered_rules.begin(), registered_rules.end(),
        [rule_id](const FaultRule& candidate) {
            return candidate.id == rule_id &&
                   candidate.config.type == FaultType::DiskFull;
        });
    if (rule == registered_rules.end()) {
        return fault_error(ErrorCode::NotFound,
                           "No registered disk-full rule matches the requested identifier.");
    }
    auto metadata = file.metadata();
    if (!metadata) return metadata.error();
    auto resource = metadata.value().path
        ? metadata.value().path->string() : metadata.value().backend;
    if (metadata.value().path) {
        auto resolved = canonical_candidate(*metadata.value().path);
        if (!resolved) return resolved.error();
        resource = resolved.value().string();
    }
    const auto target_resource = effective_resource(*rule);
    if (!target_resource.empty() && target_resource != resource) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Storage resource does not match the disk-full rule.");
    }
    auto current_usage = file.size();
    if (!current_usage) return current_usage.error();
    const auto capacity = *rule->config.maximum_capacity;
    const auto remaining = current_usage.value() < capacity
        ? capacity - current_usage.value() : 0;
    return FaultCapacityStatus{
        capacity, current_usage.value(), remaining, remaining == 0};
}

void FaultInjector::cancel_pending_delays() noexcept {
    std::vector<std::shared_ptr<FaultInjectedBackend>> active;
    {
        std::lock_guard lock(mutex_);
        backends_.erase(std::remove_if(backends_.begin(), backends_.end(),
            [](const auto& backend) { return backend.expired(); }), backends_.end());
        for (const auto& backend : backends_) {
            if (auto locked = backend.lock()) active.push_back(std::move(locked));
        }
    }
    for (const auto& backend : active) backend->cancel_delays();
}

std::vector<FaultRule> FaultInjector::rules() const {
    return manager_.rules();
}

Result<std::filesystem::path> FaultInjector::validate_environment_path(
    const std::filesystem::path& path, bool require_existing) const {
    std::filesystem::path root;
    bool configured = false;
    {
        std::lock_guard lock(mutex_);
        root = canonical_root_;
        configured = environment_configured_;
    }
    if (!configured) {
        return fault_error(ErrorCode::InvalidState,
                           "Configure an isolated disposable test environment first.");
    }
    if (require_existing && !std::filesystem::exists(path)) {
        return fault_error(ErrorCode::NotFound, "Test file does not exist.");
    }
    auto resolved = canonical_candidate(path);
    if (!resolved) return resolved.error();
    if (!is_within(resolved.value(), root)) {
        return fault_error(ErrorCode::InvalidArgument,
                           "Storage path resolves outside the authorized test directory.");
    }
    return resolved.value();
}

bool FaultInjector::destructive_authorized_for(const StorageMetadata& metadata) const {
    if (!environment_allows_destructive()) return false;
    if (!metadata.path) return metadata.backend == "memory";
    auto resolved = canonical_candidate(*metadata.path);
    if (!resolved) return false;
    std::lock_guard lock(mutex_);
    return environment_configured_ && is_within(resolved.value(), canonical_root_);
}

bool FaultInjector::environment_allows_destructive() const {
    std::lock_guard lock(mutex_);
    return environment_configured_ && environment_.isolated &&
           environment_.disposable && environment_.allow_destructive_operations;
}

void FaultInjector::track_backend(
    const std::shared_ptr<FaultInjectedBackend>& backend) {
    std::lock_guard lock(mutex_);
    backends_.erase(std::remove_if(backends_.begin(), backends_.end(),
        [](const auto& existing) { return existing.expired(); }), backends_.end());
    backends_.push_back(backend);
}

std::string_view fault_type_name(FaultType type) noexcept {
    switch (type) {
    case FaultType::WriteFailure: return "Write failure";
    case FaultType::PartialWrite: return "Partial write";
    case FaultType::TruncatedWrite: return "Truncated write";
    case FaultType::DelayedWrite: return "Delayed write";
    case FaultType::RejectedWrite: return "Rejected write";
    case FaultType::SyncFailure: return "Sync failure";
    case FaultType::InterruptedSync: return "Interrupted sync";
    case FaultType::FileTruncation: return "File truncation";
    case FaultType::FileCreationFailure: return "File creation failure";
    case FaultType::FileAccessFailure: return "File access failure";
    case FaultType::DiskFull: return "Disk full";
    case FaultType::RandomCorruption: return "Random corruption";
    case FaultType::TargetedCorruption: return "Targeted corruption";
    case FaultType::PageHeaderCorruption: return "Page-header corruption";
    case FaultType::ChecksumCorruption: return "Checksum corruption";
    case FaultType::WALRecordCorruption: return "WAL-record corruption";
    case FaultType::InvalidOffset: return "Invalid offset";
    }
    return "Unknown";
}

std::string_view fault_status_name(FaultStatus status) noexcept {
    switch (status) {
    case FaultStatus::Scheduled: return "Scheduled";
    case FaultStatus::Triggered: return "Triggered";
    case FaultStatus::Executed: return "Executed";
    case FaultStatus::Failed: return "Failed";
    case FaultStatus::Skipped: return "Skipped";
    case FaultStatus::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

} // namespace storq
