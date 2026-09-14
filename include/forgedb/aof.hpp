#pragma once
#include "forgedb/file.hpp"
#include "forgedb/store.hpp"
#include <functional>
#include <memory>
#include <string_view>

namespace forgedb {
constexpr std::size_t max_record_size = 16 * 1024 * 1024;
std::uint32_t crc32(std::string_view bytes);
std::string encode_record(const std::vector<Mutation>& mutations);
std::vector<Mutation> decode_payload(std::string_view payload);
enum class FsyncPolicy { always, everysec, none };
// Dependency injection for deterministic fault tests; production leaves these empty.
struct PersistenceHooks {
    std::function<void()> compaction_checkpoint;
    std::function<void(File&, std::string_view)> append;
};
// All methods except write_snapshot are serialized by Engine's mutex.
class Aof {
public:
    Aof(const std::filesystem::path& directory, FsyncPolicy policy,
        const std::function<void(const Mutation&)>& replay,
        std::function<void(File&, std::string_view)> writer = {});
    void append(const std::string& record);
    void sync();
    std::uint64_t size() const;
    bool healthy() const { return healthy_; }
    bool recovered_tail() const { return recovered_tail_; }
    void begin_compaction();
    File write_snapshot(const std::vector<Mutation>& snapshot, const std::function<void()>& checkpoint = {}) const;
    void finish_compaction(File candidate);
    void cancel_compaction();
    std::uint64_t sync_count() const { return sync_count_; }
private:
    std::filesystem::path directory_, path_, temporary_;
    std::unique_ptr<File> lock_, file_;
    FsyncPolicy policy_;
    std::function<void(File&, std::string_view)> writer_;
    bool healthy_ = true, recovered_tail_ = false, capturing_ = false, overflow_ = false;
    bool dirty_ = false;
    std::vector<std::string> delta_;
    std::size_t delta_bytes_ = 0;
    std::uint64_t sync_count_ = 0;
};
}
