#include "forgedb/engine.hpp"
#include <limits>
#include <sstream>
#include <unordered_map>

namespace forgedb {
namespace {
using Overlay = std::unordered_map<std::string, std::optional<Entry>>;
using Steady = std::chrono::steady_clock;
std::uint64_t elapsed_ns(Steady::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Steady::now() - start).count());
}
std::size_t reply_size(const Reply& reply) {
    std::size_t n = reply.text.size() + 32;
    for (const auto& child : reply.elements) n += reply_size(child);
    return n;
}
Reply evaluate(const Command& c, Store& store, Overlay& staged, std::int64_t now) {
    const auto op = uppercase(c[0]);
    auto get = [&](const std::string& key) -> std::optional<Entry> {
        const auto it = staged.find(key);
        return it != staged.end() ? it->second : store.get(key, now);
    };
    auto put = [&](const std::string& key, std::optional<Entry> e) { staged.insert_or_assign(key, std::move(e)); };
    if (op == "PING") return c.size() == 1 ? Reply::ok("PONG") : Reply::bulk(c[1]);
    if (op == "SET") { put(c[1], Entry{c[2], -1}); return Reply::ok(); }
    if (op == "MSET") {
        for (std::size_t i = 1; i < c.size(); i += 2) put(c[i], Entry{c[i + 1], -1});
        return Reply::ok();
    }
    if (op == "GET") { auto e = get(c[1]); return e ? Reply::bulk(e->value) : Reply{}; }
    if (op == "MGET") {
        std::vector<Reply> out;
        std::size_t bytes = 32;
        for (std::size_t i = 1; i < c.size(); ++i) {
            auto e = get(c[i]);
            bytes += 32 + (e ? e->value.size() : 0);
            if (bytes > max_response_size) throw std::length_error("response limit exceeded");
            out.push_back(e ? Reply::bulk(e->value) : Reply{});
        }
        return Reply::array(std::move(out));
    }
    if (op == "DEL" || op == "EXISTS") {
        std::int64_t count = 0;
        for (std::size_t i = 1; i < c.size(); ++i)
            if (get(c[i])) { ++count; if (op == "DEL") put(c[i], {}); }
        return Reply::integer(count);
    }
    if (op == "INCR" || op == "DECR" || op == "INCRBY") {
        auto e = get(c[1]);
        const auto previous = e ? parse_integer(e->value) : std::optional<std::int64_t>(0);
        const auto delta = op == "INCRBY" ? parse_integer(c[2]) : std::optional<std::int64_t>(op == "DECR" ? -1 : 1);
        if (!previous || !delta) return Reply::error("value is not a signed 64-bit integer");
        if ((*delta > 0 && *previous > std::numeric_limits<std::int64_t>::max() - *delta) ||
            (*delta < 0 && *previous < std::numeric_limits<std::int64_t>::min() - *delta))
            return Reply::error("integer overflow");
        const auto value = *previous + *delta;
        put(c[1], Entry{std::to_string(value), e ? e->expires_at : -1});
        return Reply::integer(value);
    }
    if (op == "EXPIRE") {
        const auto seconds = parse_integer(c[2]);
        if (!seconds || *seconds > (std::numeric_limits<std::int64_t>::max() - now) / 1000)
            return Reply::error("invalid expiration");
        auto e = get(c[1]);
        if (!e) return Reply::integer(0);
        if (*seconds <= 0) put(c[1], {});
        else { e->expires_at = now + *seconds * 1000; put(c[1], e); }
        return Reply::integer(1);
    }
    if (op == "TTL") {
        auto e = get(c[1]);
        return Reply::integer(!e ? -2 : e->expires_at < 0 ? -1 : (e->expires_at - now) / 1000);
    }
    if (op == "PERSIST") {
        auto e = get(c[1]);
        if (!e || e->expires_at < 0) return Reply::integer(0);
        e->expires_at = -1; put(c[1], e); return Reply::integer(1);
    }
    if (op == "DBSIZE") {
        // This administrative command deliberately provides an exact live count.
        store.expire(now, std::numeric_limits<std::size_t>::max());
        auto count = static_cast<std::int64_t>(store.size());
        for (const auto& [key, entry] : staged) {
            if (store.get(key, now)) --count;
            if (entry) ++count;
        }
        return Reply::integer(count);
    }
    return Reply::error("command unavailable in transaction");
}
}
std::optional<std::string> validate_command(const Command& c) {
    if (c.empty()) return "empty command";
    const auto op = uppercase(c[0]);
    const auto n = c.size();
    bool valid = false;
    if (op == "PING") valid = n == 1 || n == 2;
    else if (op == "SET" || op == "EXPIRE" || op == "INCRBY") valid = n == 3;
    else if (op == "GET" || op == "TTL" || op == "PERSIST" || op == "INCR" || op == "DECR") valid = n == 2;
    else if (op == "DEL" || op == "EXISTS" || op == "MGET") valid = n >= 2;
    else if (op == "MSET") valid = n >= 3 && n % 2 == 1;
    else if (op == "DBSIZE" || op == "INFO" || op == "COMPACT" || op == "MULTI" || op == "EXEC" || op == "DISCARD") valid = n == 1;
    else return "unknown command";
    if (!valid) return "wrong number of arguments";
    return {};
}
Engine::Engine(std::filesystem::path dir, FsyncPolicy policy, bool persistent,
               std::function<std::int64_t()> clock, PersistenceHooks hooks) : policy_(policy),
    clock_(std::move(clock)), hooks_(std::move(hooks)) {
    if (persistent) aof_ = std::make_unique<Aof>(dir, policy, [&](const Mutation& m) { store_.apply(m); }, hooks_.append);
    store_.expire(clock_(), std::numeric_limits<std::size_t>::max());
    maintenance_ = std::thread([this] { maintain(); });
}
Engine::~Engine() {
    try { shutdown(); } catch (const std::exception&) { /* Explicit shutdown reports errors to main. */ }
}
Reply Engine::execute(const Command& c) {
    if (auto error = validate_command(c)) return Reply::error(*error);
    const auto op = uppercase(c[0]);
    if (op == "COMPACT") return compact();
    if (op == "INFO") return info();
    return run({c}, false);
}
Reply Engine::transaction(const std::vector<Command>& commands) { return run(commands, true); }
Reply Engine::run(const std::vector<Command>& commands, bool transaction) {
    const auto wait_start = Steady::now();
    std::unique_lock lock(mutex_);
    lock_wait_ns_ += elapsed_ns(wait_start);
    const auto start = Steady::now();
    if (stopping_) return Reply::error("database shutting down");
    Overlay staged;
    std::vector<Reply> replies;
    std::size_t response_bytes = 32;
    const auto now = clock_();
    try {
        for (const auto& c : commands) {
            if (auto error = validate_command(c)) return Reply::error(*error);
            auto reply = evaluate(c, store_, staged, now);
            response_bytes += reply_size(reply);
            if (response_bytes > max_response_size) throw std::length_error("transaction response limit exceeded");
            replies.push_back(std::move(reply));
        }
        if (!staged.empty()) {
            std::vector<Mutation> changes;
            changes.reserve(staged.size());
            for (auto& [key, entry] : staged) changes.push_back({key, std::move(entry)});
            if (aof_) {
                const auto record = encode_record(changes);
                const auto io_start = Steady::now();
                aof_->append(record);
                aof_ns_ += elapsed_ns(io_start);
            }
            // Once the WAL commit succeeds, an allocation failure during publish
            // must fail-stop. Restart replays the complete committed record.
            try { for (const auto& m : changes) store_.apply(m); }
            catch (...) { std::terminate(); }
        }
        execution_ns_ += elapsed_ns(start);
        if (transaction) return Reply::array(std::move(replies));
        return std::move(replies.front());
    } catch (const std::length_error& e) { return Reply::error(e.what()); }
      catch (const std::system_error&) { return Reply::error("persistence I/O failed; outcome uncertain, inspect server and restart"); }
      catch (const std::runtime_error& e) { return Reply::error(e.what()); }
}
Reply Engine::compact() {
    std::unique_lock lock(mutex_);
    if (!aof_) return Reply::error("persistence disabled");
    if (stopping_ || compacting_) return Reply::error("compaction unavailable or already running");
    if (!aof_->healthy()) return Reply::error("persistence faulted");
    // Previous thread has released mutex_ and has no further engine accesses.
    if (compactor_.joinable()) compactor_.join();
    auto snapshot = store_.snapshot(clock_());
    aof_->begin_compaction(); compacting_ = true; background_error_.clear();
    try {
        compactor_ = std::thread([this, snapshot = std::move(snapshot)] {
            try {
                auto candidate = aof_->write_snapshot(snapshot, hooks_.compaction_checkpoint);
                std::lock_guard guard(mutex_);
                aof_->finish_compaction(std::move(candidate)); ++compactions_;
            } catch (const std::exception& e) {
                std::lock_guard guard(mutex_); aof_->cancel_compaction(); background_error_ = e.what();
            }
            std::lock_guard guard(mutex_); compacting_ = false;
        });
    } catch (const std::system_error& e) {
        aof_->cancel_compaction(); compacting_ = false; return Reply::error(e.what());
    }
    return Reply::ok("Background compaction started");
}
Reply Engine::info() {
    std::lock_guard lock(mutex_);
    store_.expire(clock_(), std::numeric_limits<std::size_t>::max());
    std::ostringstream out;
    out << "forgedb_version:1.0.0\r\nuptime_seconds:"
        << std::chrono::duration_cast<std::chrono::seconds>(Steady::now() - started_).count()
        << "\r\nconnected_clients:" << stats.clients.load()
        << "\r\ntotal_commands:" << stats.commands.load()
        << "\r\nworker_threads:" << stats.workers
        << "\r\nkeys:" << store_.size() << "\r\nexpired_keys:" << store_.expired()
        << "\r\npersistence:" << (aof_ ? aof_->healthy() ? "ok" : "faulted" : "disabled")
        << "\r\nfsync_policy:" << (policy_ == FsyncPolicy::always ? "always" : policy_ == FsyncPolicy::everysec ? "everysec" : "none")
        << "\r\naof_bytes:" << (aof_ ? aof_->size() : 0)
        << "\r\naof_syncs:" << (aof_ ? aof_->sync_count() : 0)
        << "\r\nrecovered_tail:" << (aof_ && aof_->recovered_tail() ? 1 : 0)
        << "\r\ncompacting:" << (compacting_ ? 1 : 0)
        << "\r\ncompactions:" << compactions_
        << "\r\nlock_wait_ns:" << lock_wait_ns_ << "\r\nexecution_ns:" << execution_ns_
        << "\r\naof_append_ns:" << aof_ns_ << "\r\nbackground_error:" << background_error_ << "\r\n";
    return Reply::bulk(out.str());
}
void Engine::maintain() {
    auto last_sync = Steady::now();
    std::unique_lock lock(mutex_);
    while (!wake_.wait_for(lock, std::chrono::milliseconds(50), [this] { return stopping_; })) {
        store_.expire(clock_());
        if (aof_ && aof_->healthy() && policy_ == FsyncPolicy::everysec && Steady::now() - last_sync >= std::chrono::seconds(1)) {
            try { aof_->sync(); } catch (const std::exception& e) { background_error_ = e.what(); }
            last_sync = Steady::now();
        }
    }
}
void Engine::shutdown() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    wake_.notify_all();
    if (maintenance_.joinable()) maintenance_.join();
    if (compactor_.joinable()) compactor_.join();
    std::lock_guard lock(mutex_);
    if (aof_) aof_->sync();
}
}
