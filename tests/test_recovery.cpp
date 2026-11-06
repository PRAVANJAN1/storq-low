#include <storq/storq.hpp>

#include <cassert>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>

struct CompleteStateInspector final : storq::IRecoveredStateInspector {
    storq::RecoveredDatabaseState inspect(const std::filesystem::path&) override {
        storq::RecoveredDatabaseState state;
        state.database_opened = true;
        state.logical_inspection_completed = true;
        state.observed_record_count = 1;
        storq::RecoveredTransaction transaction;
        transaction.transaction_id = "tx-1";
        transaction.transaction_effects_present = true;
        transaction.transaction_effects_complete = true;
        state.transactions.push_back(transaction);
        return state;
    }
};

int main() {
    std::filesystem::path workspace = std::filesystem::temp_directory_path() / "storq-recovery-test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    std::ofstream database(workspace / "source.db", std::ios::binary);
    std::array<std::byte, 512> evidence{};
    const char signature[] = "SQLite format 3\0";
    std::memcpy(evidence.data(), signature, sizeof(signature) - 1);
    evidence[16] = std::byte{0};
    evidence[17] = std::byte{2}; // 512-byte page
    database.write(reinterpret_cast<const char*>(evidence.data()), static_cast<std::streamsize>(evidence.size()));

    storq::Runtime runtime;
    assert(runtime.initialize());
    storq::RecoveryVerificationEngine engine(runtime);
    assert(engine.initialize());

    auto history = std::make_shared<storq::StaticTransactionHistory>();
    storq::ExpectedTransaction committed;
    committed.transaction_id = "tx-1";
    committed.expected_state = storq::ExpectedTransactionState::Committed;
    committed.commit_sequence = 10;
    committed.commit_was_synchronized = true;
    committed.durability_expected = true;
    history->transactions.push_back(committed);

    storq::RecoveryVerificationRequest request;
    request.recovery.database_path = workspace / "source.db";
    request.recovery.isolated_workspace = workspace;
    request.history = history;
    request.executor = std::make_shared<storq::SQLiteRecoveryExecutor>();
    request.inspector = std::make_shared<CompleteStateInspector>();

    auto result = engine.verify(request);
    assert(result);
    assert(result.value().status == storq::RecoveryVerificationStatus::Passed);

    auto missing = result.value().report.transactions;
    assert(missing.size() == 1);
    assert(missing[0].status == storq::TransactionVerificationStatus::Survived);
    assert(!result.value().report.to_json().empty());
    assert(engine.shutdown());
    assert(runtime.shutdown());
    std::cout << "Recovery verification tests passed\n";
    return 0;
}
