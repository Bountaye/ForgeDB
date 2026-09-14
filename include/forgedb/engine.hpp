#pragma once
#include "forgedb/aof.hpp"
#include "forgedb/resp.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace forgedb {
constexpr std::size_t max_response_size = 16 * 1024 * 1024;
struct RuntimeStats {
    std::atomic<std::uint64_t> clients{0}, commands{0};
    std::size_t workers = 0;
};
std::optional<std::string> validate_command(const Command& command);
class Engine {
public:
    Engine(std::filesystem::path directory, FsyncPolicy policy, bool persistent = true,
           std::function<std::int64_t()> clock = wall_time_ms,
           PersistenceHooks hooks = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Reply execute(const Command& command);
    Reply transaction(const std::vector<Command>& commands);
    Reply compact();
    Reply info();
    void shutdown();
    RuntimeStats stats;
private:
    Reply run(const std::vector<Command>& commands, bool transaction);
    void maintain();
    Store store_;
    std::unique_ptr<Aof> aof_;
    FsyncPolicy policy_;
    std::function<std::int64_t()> clock_;
    PersistenceHooks hooks_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false, compacting_ = false;
    std::string background_error_;
    std::uint64_t compactions_ = 0, lock_wait_ns_ = 0, execution_ns_ = 0, aof_ns_ = 0;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::thread maintenance_, compactor_;
};
class Session {
public:
    Reply execute(Engine& engine, const Command& command);
private:
    bool multi_ = false, dirty_ = false;
    std::size_t bytes_ = 0;
    std::vector<Command> queue_;
    void reset();
};
}
