#include <storq/index/hash_index.hpp>

#include <fstream>
#include <sstream>

namespace storq {
namespace {
StorqError index_error(std::string message) {
    return {ErrorCategory::Storage, ErrorCode::InvalidArgument, std::move(message), {}};
}
std::string key_text(const IndexKey& key) {
    auto encoded = key.serialize();
    if (!encoded) return {};
    std::string output;
    for (auto byte : encoded.value()) {
        const char hex[] = "0123456789abcdef";
        const auto value = std::to_integer<unsigned>(byte);
        output.push_back(hex[value >> 4]);
        output.push_back(hex[value & 0xf]);
    }
    return output;
}
} // namespace

HashIndex::HashIndex(std::string name, std::filesystem::path path)
    : name_(std::move(name)), path_(std::move(path)) {}

Result<void> HashIndex::insert(const IndexKey& key, const RecordID& id) {
    buckets_[key_text(key)].insert(id);
    return {};
}

Result<void> HashIndex::erase(const IndexKey& key, const RecordID& id) {
    auto found = buckets_.find(key_text(key));
    if (found == buckets_.end() || found->second.erase(id) == 0)
        return index_error("Indexed record was not found.");
    if (found->second.empty()) buckets_.erase(found);
    return {};
}

Result<std::vector<RecordID>> HashIndex::find(const IndexKey& key) const {
    auto found = buckets_.find(key_text(key));
    if (found == buckets_.end()) return std::vector<RecordID>{};
    return std::vector<RecordID>(found->second.begin(), found->second.end());
}

Result<void> HashIndex::save() const {
    if (path_.empty()) return index_error("Hash index persistence path is empty.");
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) return index_error("Unable to create hash index.");
    for (const auto& [key, ids] : buckets_) {
        output << key << ' ' << ids.size();
        for (const auto& id : ids) output << ' ' << id.to_string();
        output << '\n';
    }
    return output ? Result<void>{} : Result<void>(index_error("Unable to write hash index."));
}

Result<void> HashIndex::load() {
    buckets_.clear();
    if (path_.empty() || !std::filesystem::exists(path_)) return {};
    std::ifstream input(path_, std::ios::binary);
    if (!input) return index_error("Unable to open hash index.");
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        std::string key;
        std::size_t count = 0;
        stream >> key >> count;
        for (std::size_t i = 0; i != count; ++i) {
            std::string id_text;
            stream >> id_text;
            auto id = RecordID::from_string(id_text);
            if (id.is_valid()) buckets_[key].insert(id);
        }
    }
    return {};
}

} // namespace storq
