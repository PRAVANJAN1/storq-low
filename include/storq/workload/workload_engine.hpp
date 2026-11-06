#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <storq/adapters/database_adapter.hpp>
#include <storq/common/result.hpp>
#include <storq/recovery/recovery_engine.hpp>

namespace storq {

enum class WorkloadOperationType { Insert, Read, Update, Delete, BeginTransaction, CommitTransaction, RollbackTransaction };
enum class OracleTransactionState { Created, Began, Executing, CommitRequested, Committed, RolledBack, Incomplete, OutcomeUncertain, Failed };
enum class KeyDistribution { Uniform, Sequential, Hot, Zipf };
enum class SchedulingPolicy { Sequential, DeterministicConcurrent, Concurrent };
enum class WorkloadProfileKind { ReadHeavy, WriteHeavy, Mixed, TransactionHeavy, LargeTransaction, Contention, RecoveryStress };

struct WorkloadConfig {
    std::uint64_t random_seed{1};
    std::uint64_t operation_count{100};
    std::uint64_t transaction_count{10};
    std::uint32_t min_operations_per_transaction{1};
    std::uint32_t max_operations_per_transaction{10};
    std::uint32_t concurrency_level{1};
    bool enable_reads{true};
    bool enable_writes{true};
    bool enable_updates{true};
    bool enable_deletes{true};
    bool deterministic_execution{true};
    std::chrono::seconds duration{0};
    double read_ratio{0.5};
    std::uint64_t record_count{1000};
    std::uint64_t max_value_size{128};
    std::uint64_t batch_size{1};
    std::chrono::milliseconds transaction_timeout{5000};
    std::uint32_t maximum_retries{3};
    KeyDistribution key_distribution{KeyDistribution::Uniform};
    SchedulingPolicy scheduling_policy{SchedulingPolicy::Sequential};
    DatabaseEngine database_engine{DatabaseEngine::SQLite};
    std::string profile_name{"Mixed"};
    std::uint64_t resource_limit{1000000};
};

struct WorkloadOperation {
    std::string operation_id;
    std::string transaction_id;
    WorkloadOperationType type{WorkloadOperationType::Read};
    std::string target_entity{"records"};
    std::string target_key;
    std::unordered_map<std::string, std::string> values;
    std::uint64_t sequence_number{0};
    std::optional<std::uint64_t> expected_page_number;
    std::string expected_output;
};

struct WorkloadTransaction {
    std::string transaction_id;
    std::vector<WorkloadOperation> operations;
    std::uint64_t begin_sequence{0};
    bool expects_commit{true};
    bool commit_synchronized{false};
    OracleTransactionState state{OracleTransactionState::Created};
    bool durable{true};
    bool outcome_uncertain{false};
};

struct OracleTransition { OracleTransactionState state; std::uint64_t sequence; };

struct TransactionOracleRecord {
    WorkloadTransaction transaction;
    std::vector<OracleTransition> transitions;
};

using ExpectedStateMap = std::unordered_map<std::string, std::string>;

struct ExpectedStateSnapshot {
    ExpectedStateMap records;
    std::vector<std::string> committed_transactions;
    std::vector<std::string> uncertain_transactions;
};

struct WorkloadProfile {
    std::string name;
    WorkloadConfig config;
};

class WorkloadProfileRegistry {
public:
    WorkloadProfileRegistry();
    Result<void> register_profile(WorkloadProfile profile);
    Result<WorkloadProfile> get(const std::string& name) const;
    std::vector<std::string> names() const;
private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, WorkloadProfile> profiles_;
};

class SeedManager {
public:
    explicit SeedManager(std::uint64_t seed) : seed_(seed) {}
    std::uint64_t seed() const noexcept { return seed_; }
private:
    std::uint64_t seed_;
};

class OperationGenerator {
public:
    explicit OperationGenerator(const WorkloadConfig& config);
    Result<WorkloadOperation> next(const std::string& transaction_id);
    const WorkloadConfig& config() const noexcept { return config_; }
private:
    WorkloadConfig config_;
    std::uint64_t sequence_{0};
    std::uint64_t random_state_;
};

class TransactionGenerator {
public:
    explicit TransactionGenerator(const WorkloadConfig& config);
    Result<std::vector<WorkloadTransaction>> generate();
private:
    WorkloadConfig config_;
};

class TransactionOracle final : public IExpectedTransactionHistory {
public:
    Result<void> register_transaction(const WorkloadTransaction& transaction);
    Result<void> record_operation(const WorkloadOperation& operation);
    Result<void> record_commit(const std::string& transaction_id, bool synchronized);
    Result<void> record_rollback(const std::string& transaction_id);
    Result<void> record_uncertain_outcome(const std::string& transaction_id);
    Result<void> record_incomplete(const std::string& transaction_id);
    Result<ExpectedStateSnapshot> snapshot() const;
    std::vector<storq::ExpectedTransaction> get_transactions() const override;
private:
    bool transition(const std::string& transaction_id, OracleTransactionState state);
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TransactionOracleRecord> transactions_;
    ExpectedStateMap committed_records_;
    std::unordered_map<std::string, ExpectedStateMap> pending_;
    std::uint64_t transition_sequence_{0};
};

class WorkloadReplay {
public:
    Result<void> save(const std::filesystem::path& destination,
                      const WorkloadConfig& config,
                      const std::vector<WorkloadTransaction>& transactions) const;
    Result<std::vector<WorkloadTransaction>> load(const std::filesystem::path& source) const;
};

struct WorkloadStatistics {
    std::uint64_t total_operations{0};
    std::uint64_t successful_operations{0};
    std::uint64_t failed_operations{0};
    std::uint64_t total_transactions{0};
    std::uint64_t committed_transactions{0};
    std::uint64_t rolled_back_transactions{0};
    std::uint64_t incomplete_transactions{0};
    std::uint64_t uncertain_transactions{0};
    std::uint64_t retries{0};
    std::uint64_t conflicts{0};
    std::vector<std::chrono::nanoseconds> operation_durations;
    std::vector<std::chrono::nanoseconds> transaction_durations;
    [[nodiscard]] double average_transaction_size() const;
    [[nodiscard]] double throughput() const;
};

class WorkloadStatisticsCollector {
public:
    void operation_completed(bool success, std::chrono::nanoseconds duration);
    void transaction_completed(bool committed, bool rolled_back, bool incomplete, bool uncertain,
                               std::uint64_t operation_count, std::chrono::nanoseconds duration);
    void retry() { ++statistics_.retries; }
    void conflict() { ++statistics_.conflicts; }
    WorkloadStatistics statistics() const;
private:
    mutable std::mutex mutex_;
    WorkloadStatistics statistics_;
};

Result<void> validate_workload_config(const WorkloadConfig& config);

class WorkloadEngine {
public:
    Result<std::vector<WorkloadTransaction>> generate(const WorkloadConfig& config);
    Result<WorkloadStatistics> execute(const WorkloadConfig& config,
                                       const std::filesystem::path& database_path,
                                       AdapterManager& manager);
};

}
