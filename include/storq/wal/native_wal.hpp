#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <storq/common/result.hpp>

namespace storq {

enum class WALRecordType : std::uint16_t {
    TransactionBegin = 1,
    PageAllocate = 2,
    PageFree = 3,
    PageBeforeImage = 4,
    PageAfterImage = 5,
    RecordInsert = 6,
    RecordUpdate = 7,
    RecordDelete = 8,
    IndexInsert = 9,
    IndexDelete = 10,
    MetadataUpdate = 11,
    TransactionCommit = 12,
    TransactionAbort = 13,
    CheckpointBegin = 14,
    CheckpointEnd = 15
};

struct WALRecord {
    WALRecordType type{WALRecordType::TransactionBegin};
    std::uint64_t transaction_id{0};
    std::uint64_t lsn{0};
    std::uint64_t previous_lsn{0};
    std::uint64_t page_id{0};
    std::uint32_t page_generation{0};
    std::vector<std::byte> payload;
};

class NativeWAL {
public:
    explicit NativeWAL(std::filesystem::path database_path);

    [[nodiscard]] Result<void> open();
    [[nodiscard]] Result<void> append(const WALRecord& record);
    [[nodiscard]] Result<std::vector<WALRecord>> read_all() const;
    [[nodiscard]] Result<void> sync();
    [[nodiscard]] Result<void> checkpoint(std::uint64_t transaction_id);
    [[nodiscard]] std::uint64_t last_lsn() const noexcept { return last_lsn_; }

private:
    [[nodiscard]] Result<std::uint64_t> scan_last_lsn() const;
    std::filesystem::path path_;
    std::uint64_t last_lsn_{0};
};

} // namespace storq
