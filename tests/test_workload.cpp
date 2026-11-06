#include <storq/storq.hpp>

#include <cassert>
#include <iostream>

using namespace storq;

int main() {
    WorkloadConfig config;
    config.random_seed = 42;
    config.operation_count = 30;
    config.transaction_count = 3;
    config.min_operations_per_transaction = 2;
    config.max_operations_per_transaction = 4;
    assert(validate_workload_config(config));

    TransactionGenerator generator(config);
    auto generated = generator.generate();
    assert(generated);
    assert(generated.value().size() == 3);

    auto replay = generated.value();
    auto generated_again = generator.generate();
    assert(generated_again);
    assert(generated_again.value().size() == replay.size());
    for (std::size_t transaction_index = 0; transaction_index < replay.size(); ++transaction_index) {
        assert(replay[transaction_index].transaction_id ==
               generated_again.value()[transaction_index].transaction_id);
    }

    TransactionOracle oracle;
    for (const auto& transaction : replay) {
        assert(oracle.register_transaction(transaction));
        for (const auto& operation : transaction.operations)
            assert(oracle.record_operation(operation));
        assert(transaction.expects_commit ? oracle.record_commit(transaction.transaction_id, false)
                                           : oracle.record_rollback(transaction.transaction_id));
    }
    auto snapshot = oracle.snapshot();
    assert(snapshot);
    assert(snapshot.value().committed_transactions.size() <= replay.size());

    auto history = oracle.get_transactions();
    StaticTransactionHistory static_history;
    for (const auto& oracle_transaction : history) {
        ExpectedTransaction expected;
        expected.transaction_id = oracle_transaction.transaction_id;
        if (oracle_transaction.expected_state == ExpectedTransactionState::Committed)
            expected.expected_state = ExpectedTransactionState::Committed;
        else
            expected.expected_state = ExpectedTransactionState::RolledBack;
        static_history.transactions.push_back(expected);
    }
    assert(static_history.get_transactions().size() == history.size());

    TransactionOracle uncertain;
    auto single = replay.front();
    single.transaction_id = "uncertain-tx";
    for (auto& operation : single.operations) operation.transaction_id = single.transaction_id;
    assert(uncertain.register_transaction(single));
    assert(uncertain.record_commit("uncertain-tx", false));
    assert(!uncertain.record_commit("uncertain-tx", false));

    WorkloadProfileRegistry profiles;
    assert(profiles.get("Mixed"));
    std::cout << "Workload and oracle tests passed\n";
    return 0;
}
