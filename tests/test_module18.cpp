#include <storq/storq.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
template <typename T>
T unwrap(storq::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void require_ok(storq::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
} // namespace

int main() {
    auto first = unwrap(storq::IndexKey(std::string{"alpha"}).serialize());
    auto key = unwrap(storq::IndexKey::deserialize(first));
    require(key == storq::IndexKey(std::string{"alpha"}), "index key round trip failed");

    const auto index_path = std::filesystem::temp_directory_path() / "storq-module18.idx";
    std::filesystem::remove(index_path);
    storq::HashIndex index("email", index_path);
    storq::RecordID id{2, 1, 1};
    require_ok(index.insert(storq::IndexKey(std::string{"user@example.com"}), id));
    require(unwrap(index.find(storq::IndexKey(std::string{"user@example.com"}))).size() == 1,
            "hash index lookup failed");
    require_ok(index.save());
    storq::HashIndex restored("email", index_path);
    require_ok(restored.load());
    require(unwrap(restored.find(storq::IndexKey(std::string{"user@example.com"}))).size() == 1,
            "hash index persistence failed");
    std::filesystem::remove(index_path);

    const auto database = std::filesystem::temp_directory_path() / "storq-module18.storq";
    const auto wal_path = database.string() + ".storq-wal";
    std::filesystem::remove(database);
    std::filesystem::remove(wal_path);
    storq::NativeWAL wal(database);
    require_ok(wal.open());
    storq::WALRecord begin{storq::WALRecordType::TransactionBegin, 7, 0, 0, 0, 0, {}};
    require_ok(wal.append(begin));
    storq::WALRecord commit{storq::WALRecordType::TransactionCommit, 7, 0, begin.lsn, 0, 0, {}};
    require_ok(wal.append(commit));
    require_ok(wal.sync());
    require(unwrap(wal.read_all()).size() == 2, "WAL record count mismatch");
    {
        std::fstream wal_file(wal_path, std::ios::in | std::ios::out | std::ios::binary);
        wal_file.seekp(136);
        char byte = 0;
        wal_file.read(&byte, 1);
        byte ^= 0xff;
        wal_file.seekp(136);
        wal_file.write(&byte, 1);
    }
    require(!wal.read_all(), "corrupt WAL record checksum was accepted");
    std::filesystem::remove(database);
    std::filesystem::remove(wal_path);
    std::cout << "Module 18 tests passed\n";
    return 0;
}
