#include "forgedb/store.hpp"
#include <chrono>

namespace forgedb {
std::int64_t wall_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::optional<Entry> Store::get(const std::string& key, std::int64_t now) {
    auto it = values_.find(key);
    if (it == values_.end()) return {};
    if (it->second.expires_at >= 0 && it->second.expires_at <= now) {
        apply({key, {}}); ++expired_; return {};
    }
    return it->second;
}
void Store::apply(const Mutation& m) {
    auto it = values_.find(m.key);
    if (it != values_.end() && it->second.expires_at >= 0)
        expiry_.erase({it->second.expires_at, m.key});
    if (!m.entry) { if (it != values_.end()) values_.erase(it); return; }
    values_.insert_or_assign(m.key, *m.entry);
    if (m.entry->expires_at >= 0) expiry_.emplace(std::make_pair(m.entry->expires_at, m.key), true);
}
void Store::expire(std::int64_t now, std::size_t budget) {
    while (budget-- && !expiry_.empty() && expiry_.begin()->first.first <= now) {
        values_.erase(expiry_.begin()->first.second);
        expiry_.erase(expiry_.begin()); ++expired_;
    }
}
std::vector<Mutation> Store::snapshot(std::int64_t now) const {
    std::vector<Mutation> out;
    out.reserve(values_.size());
    for (const auto& [key, e] : values_)
        if (e.expires_at < 0 || e.expires_at > now) out.push_back({key, e});
    return out;
}
}
