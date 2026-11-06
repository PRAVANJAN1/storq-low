#include <storq/storq.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename T>
T unwrap(storq::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void require_ok(storq::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
} // namespace

int main() {
    storq::Record record;
    record.emplace("active", storq::RecordValue(true));
    record.emplace("age", storq::RecordValue(std::int64_t{25}));
    record.emplace("name", storq::RecordValue(std::string{"Example"}));
    record.emplace("note", storq::RecordValue{});
    auto encoded = unwrap(storq::RecordManager::serialize(record));
    auto decoded = unwrap(storq::RecordManager::deserialize(encoded));
    require(decoded.size() == record.size(), "record field count was not preserved");
    require(std::get<bool>(decoded.at("active").value), "boolean record value was corrupted");
    require(std::get<std::int64_t>(decoded.at("age").value) == 25, "integer record value was corrupted");
    require(std::get<std::string>(decoded.at("name").value) == "Example", "string record value was corrupted");

    storq::DataPage page(4096);
    require_ok(page.initialize(2));
    std::uint32_t slot = 0;
    require_ok(page.insert(0, 1, encoded, slot));
    require(page.slots().size() == 1, "slot was not created");
    storq::RecordID id{2, slot, 1};
    auto page_payload = unwrap(page.read(id));
    require(page_payload == encoded, "data-page record payload was corrupted");
    require_ok(page.erase(id));
    require(!page.read(id), "deleted record remained readable");
    require(storq::RecordID::from_string(id.to_string()) == id, "RecordID round trip failed");

    const auto path = std::filesystem::temp_directory_path() / "storq-record-manager-test.storq";
    std::filesystem::remove(path);
    require_ok(storq::StorqFormat::create(path));
    storq::RecordManager manager(path);
    require_ok(manager.open());
    auto stored_id = unwrap(manager.insert(record));
    require(std::get<bool>(unwrap(manager.read(stored_id)).at("active").value), "persisted record was not readable");
    require(unwrap(manager.exists(stored_id)), "persisted record did not exist");
    require_ok(manager.erase(stored_id));
    require(!unwrap(manager.exists(stored_id)), "deleted persisted record still existed");
    require_ok(manager.close());
    std::filesystem::remove(path);

    std::cout << "record tests passed\n";
    return 0;
}
