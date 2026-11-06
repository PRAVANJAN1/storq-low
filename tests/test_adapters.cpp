#include <storq/storq.hpp>

#include <cassert>
#include <iostream>
#include <fstream>

using namespace storq;
int main() {
    Runtime runtime;
    assert(runtime.initialize());
    AdapterManager manager(&runtime);
    assert(manager.initialize_adapter(DatabaseEngine::SQLite));

    auto adapter_result = manager.resolve(std::filesystem::path{});
    assert(!adapter_result);

    SQLiteAdapter adapter;
    AdapterConfig config;
    assert(adapter.initialize(config));
    assert(adapter.capabilities().supports(DatabaseCapability::Transactions));

    std::filesystem::path database = "storq_adapter_test.db";
    std::filesystem::remove(database);
    {
        SQLiteConnection setup(adapter, DatabaseConnectionConfig{database});
        setup.open();
        assert(setup.execute("CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT)"));
        setup.close();
    }
    auto detected = adapter.can_open(database);
    assert(detected && detected.value());

    DatabaseConnectionConfig connection_config;
    connection_config.database_path = database;
    auto connection_result = adapter.connect(connection_config);
    assert(connection_result);
    auto connection = connection_result.value();
    auto transaction_result = connection->transaction();
    assert(transaction_result);
    auto transaction = transaction_result.value();
    assert(transaction->begin());
    assert(connection->execute("INSERT INTO items(value) VALUES ('adapter')"));
    assert(transaction->commit());
    assert(!transaction->rollback());

    SQLiteInspector inspector;
    auto metadata = inspector.inspect(database);
    assert(metadata && metadata.value().page_size.has_value());
    assert(metadata.value().page_count.has_value());

    SQLiteSnapshot snapshot;
    SnapshotRequest snapshot_request;
    snapshot_request.source_database = database;
    snapshot_request.destination = "storq_adapter_snapshot.db";
    std::filesystem::remove(snapshot_request.destination);
    assert(snapshot.create(snapshot_request));
    assert(snapshot.validate());
    std::filesystem::remove(snapshot_request.destination);
    assert(snapshot.remove());

    SQLiteRecovery recovery;
    AdapterRecoveryRequest recovery_request;
    recovery_request.source_database = database;
    recovery_request.workspace = "storq_adapter_recovery";
    std::filesystem::remove_all(recovery_request.workspace);
    auto recovery_result = recovery.recover(recovery_request);
    assert(recovery_result && recovery_result.value().can_open);

    assert(connection->close());
    assert(adapter.shutdown());
    std::filesystem::remove(database);
    std::filesystem::remove_all("storq_adapter_recovery");
    assert(runtime.shutdown());
    std::cout << "Adapter framework tests passed\n";
    return 0;
}
