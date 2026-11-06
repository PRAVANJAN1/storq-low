#include <storq/storq.hpp>

#include <fstream>
#include <array>
#include <cstring>
#include <iostream>

struct ExampleInspector final : storq::IRecoveredStateInspector {
    storq::RecoveredDatabaseState inspect(const std::filesystem::path&) override {
        storq::RecoveredDatabaseState state;
        state.database_opened = true;
        state.logical_inspection_completed = true;
        storq::RecoveredTransaction transaction;
        transaction.transaction_id = "tx-wal-1";
        transaction.transaction_effects_present = true;
        transaction.transaction_effects_complete = true;
        state.transactions.push_back(transaction);
        return state;
    }
};

int main() {
    auto workspace = std::filesystem::temp_directory_path() / "storq-recovery-example";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    auto database = workspace / "source.db";
    std::ofstream database_file(database, std::ios::binary);
    std::array<std::byte, 512> evidence{};
    const char signature[] = "SQLite format 3\0";
    std::memcpy(evidence.data(), signature, sizeof(signature) - 1);
    evidence[16] = std::byte{0};
    evidence[17] = std::byte{2};
    database_file.write(reinterpret_cast<const char*>(evidence.data()), static_cast<std::streamsize>(evidence.size()));

    storq::Runtime runtime;
    if (!runtime.initialize()) return 1;
    storq::RecoveryVerificationEngine engine(runtime);
    if (!engine.initialize()) return 2;

    auto history = std::make_shared<storq::StaticTransactionHistory>();
    storq::ExpectedTransaction transaction;
    transaction.transaction_id = "tx-wal-1";
    transaction.expected_state = storq::ExpectedTransactionState::Committed;
    transaction.commit_sequence = 1;
    transaction.commit_was_synchronized = true;
    transaction.durability_expected = true;
    history->transactions.push_back(transaction);

    storq::RecoveryVerificationRequest request;
    request.recovery.database_path = database;
    request.recovery.isolated_workspace = workspace;
    request.history = history;
    request.executor = std::make_shared<storq::SQLiteRecoveryExecutor>();
    request.inspector = std::make_shared<ExampleInspector>();
    auto result = engine.verify(request);
    if (!result) return 3;
    std::cout << result.value().report.to_json() << '\n';

    static_cast<void>(engine.shutdown());
    static_cast<void>(runtime.shutdown());
    return 0;
}
