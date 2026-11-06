#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <storq/common/result.hpp>
#include <storq/index/index_key.hpp>
#include <storq/records/record_id.hpp>

namespace storq {

class HashIndex {
public:
    HashIndex(std::string name, std::filesystem::path path = {});

    [[nodiscard]] Result<void> insert(const IndexKey& key, const RecordID& id);
    [[nodiscard]] Result<void> erase(const IndexKey& key, const RecordID& id);
    [[nodiscard]] Result<std::vector<RecordID>> find(const IndexKey& key) const;
    [[nodiscard]] Result<void> save() const;
    [[nodiscard]] Result<void> load();
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    std::string name_;
    std::filesystem::path path_;
    std::map<std::string, std::set<RecordID>> buckets_;
};

} // namespace storq
