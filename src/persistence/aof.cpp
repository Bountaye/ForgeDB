#include "forgedb/aof.hpp"
#include <array>
#include <bit>
#include <iostream>
#include <limits>

namespace forgedb {
namespace {
void put32(std::string& out, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) out += static_cast<char>((n >> (i * 8)) & 255);
}
void put64(std::string& out, std::uint64_t n) {
    for (unsigned i = 0; i < 8; ++i) out += static_cast<char>((n >> (i * 8)) & 255);
}
std::uint32_t get32(std::string_view& s) {
    if (s.size() < 4) throw std::runtime_error("corrupt AOF integer");
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[i])) << (i * 8);
    s.remove_prefix(4); return n;
}
std::uint64_t get64(std::string_view& s) {
    if (s.size() < 8) throw std::runtime_error("corrupt AOF timestamp");
    std::uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) n |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[i])) << (i * 8);
    s.remove_prefix(8); return n;
}
std::string take_string(std::string_view& s) {
    const auto n = get32(s);
    if (n > s.size()) throw std::runtime_error("corrupt AOF string length");
    std::string result(s.substr(0, n)); s.remove_prefix(n); return result;
}
void put_string(std::string& s, const std::string& value) {
    if (value.size() > max_record_size) throw std::length_error("mutation too large");
    put32(s, static_cast<std::uint32_t>(value.size())); s += value;
}
}
std::uint32_t crc32(std::string_view s) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto c = i;
            for (int bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1) ? 0xedb88320U : 0U);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xffffffffU;
    for (const char b : s) c = table[(c ^ static_cast<unsigned char>(b)) & 255] ^ (c >> 8);
    return c ^ 0xffffffffU;
}
std::string encode_record(const std::vector<Mutation>& mutations) {
    if (mutations.empty() || mutations.size() > 65536) throw std::length_error("invalid mutation count");
    std::string payload;
    put32(payload, static_cast<std::uint32_t>(mutations.size()));
    for (const auto& m : mutations) {
        payload += m.entry ? '\1' : '\0'; put_string(payload, m.key);
        if (m.entry) { put64(payload, std::bit_cast<std::uint64_t>(m.entry->expires_at)); put_string(payload, m.entry->value); }
        if (payload.size() > max_record_size) throw std::length_error("transaction result exceeds AOF record limit");
    }
    std::string out = "FDB1";
    put32(out, static_cast<std::uint32_t>(payload.size())); put32(out, crc32(payload));
    put32(out, crc32(out)); out += payload; return out;
}
std::vector<Mutation> decode_payload(std::string_view s) {
    const auto count = get32(s);
    if (count == 0 || count > 65536 || count > s.size() / 5) throw std::runtime_error("corrupt AOF mutation count");
    std::vector<Mutation> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (s.empty() || (s[0] != '\0' && s[0] != '\1')) throw std::runtime_error("corrupt AOF mutation tag");
        const bool present = s[0] != '\0'; s.remove_prefix(1);
        Mutation m{take_string(s), {}};
        if (present) {
            const auto expiry = std::bit_cast<std::int64_t>(get64(s));
            if (expiry < -1) throw std::runtime_error("corrupt AOF expiry");
            m.entry = Entry{take_string(s), expiry};
        }
        out.push_back(std::move(m));
    }
    if (!s.empty()) throw std::runtime_error("trailing bytes in AOF record");
    return out;
}
Aof::Aof(const std::filesystem::path& dir, FsyncPolicy policy,
         const std::function<void(const Mutation&)>& replay,
         std::function<void(File&, std::string_view)> writer) : directory_(dir),
    path_(dir / "append.aof"), temporary_(dir / "append.compacting"), policy_(policy), writer_(std::move(writer)) {
    std::filesystem::create_directories(dir);
    lock_ = std::make_unique<File>(dir / "LOCK", File::Mode::lock);
    file_ = std::make_unique<File>(path_, File::Mode::append);
    const auto end = file_->size();
    std::uint64_t offset = 0;
    while (offset < end) {
        auto header = file_->read(offset, 16);
        if (header.size() < 16) { recovered_tail_ = true; break; }
        std::string_view h(header);
        if (h.substr(0, 4) != "FDB1") throw std::runtime_error("invalid AOF magic at byte " + std::to_string(offset));
        h.remove_prefix(4);
        const auto length = get32(h), checksum = get32(h), header_checksum = get32(h);
        if (crc32(std::string_view(header).substr(0, 12)) != header_checksum || length > max_record_size || length < 9)
            throw std::runtime_error("corrupt AOF header at byte " + std::to_string(offset));
        if (length > end - offset - 16) { recovered_tail_ = true; break; }
        const auto payload = file_->read(offset + 16, length);
        if (payload.size() != length || crc32(payload) != checksum)
            throw std::runtime_error("corrupt AOF checksum at byte " + std::to_string(offset));
        const auto mutations = decode_payload(payload); // Validate the whole transaction before replay.
        for (const auto& m : mutations) replay(m);
        offset += 16 + length;
    }
    if (recovered_tail_) {
        std::cerr << "Recovery: discarded incomplete AOF tail at byte " << offset << '\n';
        file_->truncate(offset); file_->sync();
    }
    // The committed AOF is authoritative. An abandoned rewrite is never replayed.
    std::filesystem::remove(temporary_);
    File::sync_directory(directory_);
}
void Aof::append(const std::string& record) {
    if (!healthy_) throw std::runtime_error("persistence is faulted; restart required");
    // Reserve/copy delta BEFORE touching disk. Allocation failure must not produce
    // a durable mutation followed by an ordinary error response.
    if (capturing_ && !overflow_) {
        if (record.size() > 32 * 1024 * 1024 - delta_bytes_) { overflow_ = true; delta_.clear(); }
        else { delta_.push_back(record); delta_bytes_ += record.size(); }
    }
    try {
        if (writer_) writer_(*file_, record); else file_->append(record);
        dirty_ = true;
        if (policy_ == FsyncPolicy::always) sync();
    } catch (const std::exception&) { healthy_ = false; throw; }
}
void Aof::sync() {
    if (!healthy_) throw std::runtime_error("persistence is faulted");
    if (!dirty_) return;
    try { file_->sync(); dirty_ = false; ++sync_count_; }
    catch (const std::exception&) { healthy_ = false; throw; }
}
std::uint64_t Aof::size() const { return std::filesystem::file_size(path_); }
void Aof::begin_compaction() { capturing_ = true; overflow_ = false; delta_.clear(); delta_bytes_ = 0; }
File Aof::write_snapshot(const std::vector<Mutation>& snapshot, const std::function<void()>& checkpoint) const {
    File candidate(temporary_, File::Mode::truncate);
    bool first = true;
    for (const auto& m : snapshot) {
        candidate.append(encode_record({m}));
        if (first && checkpoint) checkpoint();
        first = false;
    }
    candidate.sync(); return candidate;
}
void Aof::finish_compaction(File candidate) {
    if (overflow_ || !healthy_) throw std::runtime_error("compaction cancelled: delta limit or AOF fault");
    for (const auto& record : delta_) candidate.append(record);
    candidate.sync();
#ifdef _WIN32
    // Windows replacement can reject an open destination even with share-delete.
    // Close handles under the commit mutex, atomically rename, then reopen. The
    // old pathname is never deleted first. Any failure here faults further writes.
    candidate.close(); file_.reset();
    try {
        File::replace(temporary_, path_);
        file_ = std::make_unique<File>(path_, File::Mode::append);
    } catch (...) { healthy_ = false; throw; }
#else
    // Allocate the owner before publishing the file name; no allocation after rename.
    auto replacement = std::make_unique<File>(std::move(candidate));
    File::replace(temporary_, path_);
    file_.swap(replacement); dirty_ = false;
#endif
    dirty_ = false;
    try { File::sync_directory(directory_); }
    catch (const std::exception&) { healthy_ = false; throw; }
    capturing_ = false; delta_.clear(); delta_bytes_ = 0;
}
void Aof::cancel_compaction() { capturing_ = false; delta_.clear(); delta_bytes_ = 0; }
}
