#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace forgedb {
struct Entry {
    std::string value;
    std::int64_t expires_at = -1;
    bool operator==(const Entry&) const = default;
};
struct Mutation { std::string key; std::optional<Entry> entry; };
std::int64_t wall_time_ms();
// Engine owns synchronization. The index contains exactly one node per TTL key.
class Store {
public:
    std::optional<Entry> get(const std::string& key, std::int64_t now);
    void apply(const Mutation& mutation);
    void expire(std::int64_t now, std::size_t budget = 256);
    std::vector<Mutation> snapshot(std::int64_t now) const;
    std::size_t size() const { return values_.size(); }
    std::uint64_t expired() const { return expired_; }
private:
    std::unordered_map<std::string, Entry> values_;
    std::map<std::pair<std::int64_t, std::string>, bool> expiry_;
    std::uint64_t expired_ = 0;
};
}
