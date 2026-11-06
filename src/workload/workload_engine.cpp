#include <storq/workload/workload_engine.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace storq {

namespace {
StorqError workload_error(ErrorCode code, std::string message) {
    return StorqError{ErrorCategory::Configuration, code, std::move(message), {}};
}

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

std::string transaction_history_value(const WorkloadTransaction& transaction) {
    std::ostringstream value;
    value << transaction.operations.size() << ":" << transaction.expects_commit << ":" << transaction.commit_synchronized;
    return value.str();
}
}

Result<void> validate_workload_config(const WorkloadConfig& config) {
    if (config.operation_count == 0 || config.transaction_count == 0)
        return workload_error(ErrorCode::InvalidArgument, "Operation and transaction counts must be positive.");
    if (config.operation_count < config.transaction_count)
        return workload_error(ErrorCode::InvalidArgument, "Operation count must be at least transaction count.");
    if (config.min_operations_per_transaction == 0 ||
        config.min_operations_per_transaction > config.max_operations_per_transaction)
        return workload_error(ErrorCode::InvalidArgument, "Invalid operations-per-transaction range.");
    if (config.max_operations_per_transaction > config.resource_limit)
        return workload_error(ErrorCode::InvalidArgument, "Transaction size exceeds the resource limit.");
    if (config.operation_count > config.resource_limit)
        return workload_error(ErrorCode::InvalidArgument, "Operation count exceeds the resource limit.");
    if (config.concurrency_level == 0 || config.concurrency_level > 64)
        return workload_error(ErrorCode::InvalidArgument, "Concurrency level must be between 1 and 64.");
    if (!config.enable_reads && !config.enable_writes && !config.enable_updates && !config.enable_deletes)
        return workload_error(ErrorCode::InvalidArgument, "At least one operation type must be enabled.");
    if (config.read_ratio < 0.0 || config.read_ratio > 1.0)
        return workload_error(ErrorCode::InvalidArgument, "Read ratio must be between zero and one.");
    return {};
}

WorkloadProfileRegistry::WorkloadProfileRegistry() {
    for (const auto& name : {"ReadHeavy", "WriteHeavy", "Mixed", "TransactionHeavy",
                             "LargeTransaction", "Contention", "RecoveryStress"}) {
        WorkloadConfig config;
        config.profile_name = name;
        if (name == std::string("ReadHeavy")) config.read_ratio = 0.9;
        else if (name == std::string("WriteHeavy")) { config.read_ratio = 0.1; config.max_operations_per_transaction = 20; }
        else if (name == std::string("TransactionHeavy")) { config.transaction_count = 100; config.max_operations_per_transaction = 2; }
        else if (name == std::string("LargeTransaction")) { config.min_operations_per_transaction = 20; config.max_operations_per_transaction = 50; }
        else if (name == std::string("Contention")) { config.key_distribution = KeyDistribution::Hot; config.concurrency_level = 4; }
        else if (name == std::string("RecoveryStress")) config.scheduling_policy = SchedulingPolicy::DeterministicConcurrent;
        profiles_[config.profile_name] = WorkloadProfile{config.profile_name, config};
    }
}

Result<void> WorkloadProfileRegistry::register_profile(WorkloadProfile profile) {
    if (profile.name.empty()) return workload_error(ErrorCode::InvalidArgument, "Profile name is required.");
    auto result = validate_workload_config(profile.config);
    if (!result) return result;
    std::lock_guard lock(mutex_);
    if (profiles_.count(profile.name)) return workload_error(ErrorCode::DuplicateRegistration, "Workload profile already exists.");
    profiles_[profile.name] = std::move(profile);
    return {};
}

Result<WorkloadProfile> WorkloadProfileRegistry::get(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = profiles_.find(name);
    if (it == profiles_.end()) return workload_error(ErrorCode::NotFound, "Workload profile not found.");
    return it->second;
}

std::vector<std::string> WorkloadProfileRegistry::names() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    for (const auto& [name, profile] : profiles_) result.push_back(name);
    return result;
}

OperationGenerator::OperationGenerator(const WorkloadConfig& config)
    : config_(config), random_state_(config.random_seed ? config.random_seed : 0x9e3779b97f4a7c15ULL) {}

Result<WorkloadOperation> OperationGenerator::next(const std::string& transaction_id) {
    WorkloadOperation operation;
    operation.operation_id = "op-" + std::to_string(config_.random_seed) + "-" + std::to_string(sequence_);
    operation.transaction_id = transaction_id;
    operation.sequence_number = sequence_++;
    std::uint64_t random = next_random(random_state_) % 100;
    bool writes = config_.enable_writes || config_.enable_updates || config_.enable_deletes;
    if (config_.enable_reads && (!writes || random < config_.read_ratio * 100.0))
        operation.type = WorkloadOperationType::Read;
    else if (config_.enable_writes && random % 3 == 0)
        operation.type = WorkloadOperationType::Insert;
    else if (config_.enable_updates && random % 3 == 1)
        operation.type = WorkloadOperationType::Update;
    else
        operation.type = WorkloadOperationType::Delete;
    std::uint64_t key = next_random(random_state_) % std::max<std::uint64_t>(1, config_.record_count);
    if (config_.key_distribution == KeyDistribution::Sequential)
        key = (sequence_ - 1) % std::max<std::uint64_t>(1, config_.record_count);
    else if (config_.key_distribution == KeyDistribution::Hot)
        key = next_random(random_state_) % std::max<std::uint64_t>(1, std::min<std::uint64_t>(10, config_.record_count));
    operation.target_key = std::to_string(key);
    if (operation.type == WorkloadOperationType::Insert || operation.type == WorkloadOperationType::Update)
        operation.values["value"] = std::string(std::min<std::uint64_t>(config_.max_value_size, 16), 'x');
    return operation;
}

TransactionGenerator::TransactionGenerator(const WorkloadConfig& config) : config_(config) {}

Result<std::vector<WorkloadTransaction>> TransactionGenerator::generate() {
    auto valid = validate_workload_config(config_);
    if (!valid) return workload_error(valid.error().code, valid.error().message);
    std::vector<WorkloadTransaction> transactions;
    OperationGenerator operations(config_);
    std::uint64_t state = config_.random_seed;
    for (std::uint64_t transaction_index = 0;
         transaction_index < config_.transaction_count; ++transaction_index) {
        WorkloadTransaction transaction;
        transaction.transaction_id = "tx-" + std::to_string(config_.random_seed) + "-" +
                                     std::to_string(transaction_index);
        transaction.begin_sequence = transaction_index * std::max<std::uint32_t>(1, config_.min_operations_per_transaction);
        std::uint64_t size = config_.min_operations_per_transaction;
        if (config_.max_operations_per_transaction > config_.min_operations_per_transaction)
            size += (next_random(state) % (config_.max_operations_per_transaction -
                                           config_.min_operations_per_transaction + 1));
        size = std::min<std::uint64_t>(size, config_.operation_count);
        transaction.expects_commit = true;
        if (config_.profile_name == "TransactionHeavy" && next_random(state) % 5 == 0)
            transaction.expects_commit = false;
        transaction.operations.reserve(static_cast<std::size_t>(size));
        for (std::uint64_t i = 0; i < size &&
             operations.config().operation_count > transactions.size() + transaction.operations.size(); ++i) {
            auto operation = operations.next(transaction.transaction_id);
            if (!operation) return workload_error(operation.error().code, operation.error().message);
            transaction.operations.push_back(operation.value());
        }
        transactions.push_back(std::move(transaction));
    }
    return transactions;
}

Result<void> TransactionOracle::register_transaction(const WorkloadTransaction& transaction) {
    std::lock_guard lock(mutex_);
    if (transactions_.count(transaction.transaction_id))
        return workload_error(ErrorCode::DuplicateRegistration, "Transaction is already registered.");
    TransactionOracleRecord record;
    record.transaction = transaction;
    record.transaction.state = OracleTransactionState::Created;
    record.transitions.push_back({OracleTransactionState::Created, ++transition_sequence_});
    transactions_[transaction.transaction_id] = std::move(record);
    pending_[transaction.transaction_id] = {};
    return {};
}

bool TransactionOracle::transition(const std::string& transaction_id, OracleTransactionState state) {
    auto& record = transactions_.at(transaction_id);
    record.transitions.push_back({state, ++transition_sequence_});
    record.transaction.state = state;
    return true;
}

Result<void> TransactionOracle::record_operation(const WorkloadOperation& operation) {
    std::lock_guard lock(mutex_);
    auto transaction = transactions_.find(operation.transaction_id);
    if (transaction == transactions_.end())
        return workload_error(ErrorCode::NotFound, "Transaction is not registered.");
    if (transaction->second.transaction.state == OracleTransactionState::Created)
        transition(operation.transaction_id, OracleTransactionState::Began);
    if (transaction->second.transaction.state == OracleTransactionState::Began)
        transition(operation.transaction_id, OracleTransactionState::Executing);
    auto& pending = pending_[operation.transaction_id];
    if (operation.type == WorkloadOperationType::Insert || operation.type == WorkloadOperationType::Update)
        pending[operation.target_entity + "/" + operation.target_key] = operation.values.count("value")
            ? operation.values.at("value") : std::string{};
    else if (operation.type == WorkloadOperationType::Delete)
        pending.erase(operation.target_entity + "/" + operation.target_key);
    return {};
}

Result<void> TransactionOracle::record_commit(const std::string& transaction_id, bool synchronized) {
    std::lock_guard lock(mutex_);
    auto it = transactions_.find(transaction_id);
    if (it == transactions_.end()) return workload_error(ErrorCode::NotFound, "Transaction is not registered.");
    if (it->second.transaction.state == OracleTransactionState::Committed)
        return workload_error(ErrorCode::InvalidState, "Transaction was already committed.");
    transition(transaction_id, OracleTransactionState::CommitRequested);
    transition(transaction_id, OracleTransactionState::Committed);
    it->second.transaction.commit_synchronized = synchronized;
    for (const auto& [key, value] : pending_[transaction_id]) committed_records_[key] = value;
    pending_[transaction_id].clear();
    return {};
}

Result<void> TransactionOracle::record_rollback(const std::string& transaction_id) {
    std::lock_guard lock(mutex_);
    auto it = transactions_.find(transaction_id);
    if (it == transactions_.end()) return workload_error(ErrorCode::NotFound, "Transaction is not registered.");
    if (it->second.transaction.state == OracleTransactionState::Committed ||
        it->second.transaction.state == OracleTransactionState::RolledBack)
        return workload_error(ErrorCode::InvalidState, "Transaction has completed.");
    transition(transaction_id, OracleTransactionState::RolledBack);
    pending_[transaction_id].clear();
    return {};
}

Result<void> TransactionOracle::record_uncertain_outcome(const std::string& transaction_id) {
    std::lock_guard lock(mutex_);
    auto it = transactions_.find(transaction_id);
    if (it == transactions_.end()) return workload_error(ErrorCode::NotFound, "Transaction is not registered.");
    transition(transaction_id, OracleTransactionState::OutcomeUncertain);
    it->second.transaction.outcome_uncertain = true;
    pending_[transaction_id].clear();
    return {};
}

Result<void> TransactionOracle::record_incomplete(const std::string& transaction_id) {
    std::lock_guard lock(mutex_);
    auto it = transactions_.find(transaction_id);
    if (it == transactions_.end()) return workload_error(ErrorCode::NotFound, "Transaction is not registered.");
    if (it->second.transaction.state == OracleTransactionState::Committed ||
        it->second.transaction.state == OracleTransactionState::RolledBack ||
        it->second.transaction.state == OracleTransactionState::Incomplete)
        return workload_error(ErrorCode::InvalidState, "Transaction has completed.");
    transition(transaction_id, OracleTransactionState::Incomplete);
    pending_[transaction_id].clear();
    return {};
}

Result<ExpectedStateSnapshot> TransactionOracle::snapshot() const {
    std::lock_guard lock(mutex_);
    ExpectedStateSnapshot snapshot;
    snapshot.records = committed_records_;
    for (const auto& [id, record] : transactions_) {
        if (record.transaction.state == OracleTransactionState::Committed) snapshot.committed_transactions.push_back(id);
        if (record.transaction.outcome_uncertain) snapshot.uncertain_transactions.push_back(id);
    }
    std::sort(snapshot.committed_transactions.begin(), snapshot.committed_transactions.end());
    std::sort(snapshot.uncertain_transactions.begin(), snapshot.uncertain_transactions.end());
    return snapshot;
}

std::vector<storq::ExpectedTransaction> TransactionOracle::get_transactions() const {
    std::lock_guard lock(mutex_);
    std::vector<storq::ExpectedTransaction> transactions;
    for (const auto& [id, record] : transactions_) {
        auto transaction = record.transaction;
        if (transaction.state == OracleTransactionState::Began ||
            transaction.state == OracleTransactionState::Executing)
            transaction.state = OracleTransactionState::Incomplete;
        storq::ExpectedTransaction history;
        history.transaction_id = transaction.transaction_id;
        history.begin_sequence = transaction.begin_sequence;
        if (transaction.commit_synchronized) history.commit_sequence = transaction.begin_sequence + transaction.operations.size();
        history.expected_state = transaction.state == OracleTransactionState::Committed
            ? ExpectedTransactionState::Committed
            : transaction.state == OracleTransactionState::RolledBack
                ? ExpectedTransactionState::RolledBack
                : transaction.state == OracleTransactionState::Incomplete
                    ? ExpectedTransactionState::Incomplete
                    : ExpectedTransactionState::Unknown;
        history.commit_was_synchronized = transaction.commit_synchronized;
        history.expected_values["operation_count"] = std::to_string(transaction.operations.size());
        transactions.push_back(std::move(history));
    }
    std::sort(transactions.begin(), transactions.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.transaction_id < rhs.transaction_id; });
    return transactions;
}

Result<void> WorkloadReplay::save(const std::filesystem::path& destination, const WorkloadConfig& config,
                                  const std::vector<WorkloadTransaction>& transactions) const {
    std::ofstream output(destination);
    if (!output) return workload_error(ErrorCode::IoError, "Unable to create replay file.");
    output << config.random_seed << '\n' << transactions.size() << '\n';
    for (const auto& transaction : transactions) {
        output << transaction.transaction_id << ' ' << transaction.operations.size() << '\n';
        for (const auto& operation : transaction.operations)
            output << operation.operation_id << '|' << operation.transaction_id << '|'
                   << static_cast<int>(operation.type) << '|' << operation.sequence_number << '\n';
    }
    return {};
}

Result<std::vector<WorkloadTransaction>> WorkloadReplay::load(const std::filesystem::path& source) const {
    std::ifstream input(source);
    if (!input) return workload_error(ErrorCode::IoError, "Unable to open replay file.");
    std::uint64_t seed = 0;
    std::size_t transaction_count = 0;
    if (!(input >> seed >> transaction_count))
        return workload_error(ErrorCode::InvalidArgument, "Invalid workload replay file.");
    std::vector<WorkloadTransaction> transactions;
    for (std::size_t i = 0; i < transaction_count; ++i) {
        WorkloadTransaction transaction;
        std::size_t operation_count = 0;
        if (!(input >> transaction.transaction_id >> operation_count))
            return workload_error(ErrorCode::InvalidArgument, "Invalid transaction replay record.");
        transaction.operations.reserve(operation_count);
        std::string line;
        std::getline(input, line);
        for (std::size_t j = 0; j < operation_count; ++j) {
            std::getline(input, line);
            std::istringstream stream(line);
            WorkloadOperation operation;
            stream >> operation.operation_id;
            operation.transaction_id = transaction.transaction_id;
            if (!operation.operation_id.empty()) transaction.operations.push_back(operation);
        }
        transactions.push_back(std::move(transaction));
    }
    return transactions;
}

void WorkloadStatisticsCollector::operation_completed(bool success, std::chrono::nanoseconds duration) {
    std::lock_guard lock(mutex_);
    ++statistics_.total_operations;
    if (success) ++statistics_.successful_operations; else ++statistics_.failed_operations;
    statistics_.operation_durations.push_back(duration);
}

void WorkloadStatisticsCollector::transaction_completed(bool committed, bool rolled_back, bool incomplete, bool uncertain,
                                                        std::uint64_t operation_count, std::chrono::nanoseconds duration) {
    std::lock_guard lock(mutex_);
    ++statistics_.total_transactions;
    statistics_.committed_transactions += committed ? 1 : 0;
    statistics_.rolled_back_transactions += rolled_back ? 1 : 0;
    statistics_.incomplete_transactions += incomplete ? 1 : 0;
    statistics_.uncertain_transactions += uncertain ? 1 : 0;
    statistics_.operation_durations.insert(statistics_.operation_durations.end(), operation_count, std::chrono::nanoseconds{0});
    statistics_.transaction_durations.push_back(duration);
}

WorkloadStatistics WorkloadStatisticsCollector::statistics() const {
    std::lock_guard lock(mutex_);
    return statistics_;
}

double WorkloadStatistics::average_transaction_size() const {
    return total_transactions == 0 ? 0.0 : static_cast<double>(total_operations) / total_transactions;
}

double WorkloadStatistics::throughput() const {
    std::uint64_t duration_ns = 0;
    for (const auto duration : transaction_durations) duration_ns += static_cast<std::uint64_t>(duration.count());
    return duration_ns == 0 ? 0.0 : total_operations * 1000000000.0 / duration_ns;
}

Result<std::vector<WorkloadTransaction>> WorkloadEngine::generate(const WorkloadConfig& config) {
    return TransactionGenerator(config).generate();
}

Result<WorkloadStatistics> WorkloadEngine::execute(const WorkloadConfig& config,
                                                    const std::filesystem::path& database_path,
                                                    AdapterManager& manager) {
    auto generated = generate(config);
    if (!generated) return workload_error(generated.error().code, generated.error().message);
    auto adapter = manager.resolve(database_path);
    if (!adapter) return workload_error(adapter.error().code, adapter.error().message);
    DatabaseConnectionConfig connection_config;
    connection_config.database_path = database_path;
    connection_config.read_only = false;
    auto connection = adapter.value()->connect(connection_config);
    if (!connection) return workload_error(connection.error().code, connection.error().message);
    WorkloadStatisticsCollector collector;
    for (const auto& transaction : generated.value()) {
        auto transaction_result = connection.value()->transaction();
        bool succeeded = static_cast<bool>(transaction_result);
        if (succeeded) succeeded = static_cast<bool>(transaction_result.value()->begin());
        if (succeeded && !transaction.operations.empty()) {
            auto count = std::min<std::size_t>(transaction.operations.size(), 1);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& operation = transaction.operations[i];
                succeeded = static_cast<bool>(connection.value()->execute(
                    "SELECT 1 FROM records WHERE id = " + operation.target_key));
                collector.operation_completed(succeeded, std::chrono::nanoseconds{0});
                if (!succeeded) break;
            }
        }
        if (succeeded) succeeded = static_cast<bool>(transaction_result.value()->commit());
        else static_cast<void>(transaction_result.value()->rollback());
        collector.transaction_completed(succeeded, !succeeded, false, false,
                                        transaction.operations.size(), std::chrono::nanoseconds{0});
    }
    static_cast<void>(connection.value()->close());
    return collector.statistics();
}

}
