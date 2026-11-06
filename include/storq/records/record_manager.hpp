#pragma once

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include <storq/common/result.hpp>
#include <storq/records/record.hpp>
#include <storq/records/record_id.hpp>

namespace storq {

class StorageFile;

class RecordManager {
public:
    explicit RecordManager(std::filesystem::path path);

    [[nodiscard]] Result<void> open();
    [[nodiscard]] Result<void> close();
    [[nodiscard]] Result<RecordID> insert(const Record& record);
    [[nodiscard]] Result<Record> read(const RecordID& id);
    [[nodiscard]] Result<void> update(const RecordID& id, const Record& record);
    [[nodiscard]] Result<void> erase(const RecordID& id);
    [[nodiscard]] Result<bool> exists(const RecordID& id);

    [[nodiscard]] static Result<std::vector<std::byte>> serialize(const Record& record);
    [[nodiscard]] static Result<Record> deserialize(std::span<const std::byte> bytes);
    [[nodiscard]] static Result<RecordID> parse_record_id(const std::string& value);

private:
    [[nodiscard]] Result<std::uint64_t> allocate_page();
    [[nodiscard]] Result<void> update_page_count(std::uint64_t count);

    std::filesystem::path path_;
    std::shared_ptr<StorageFile> file_;
    std::uint32_t page_size_{4096};
};

} // namespace storq
