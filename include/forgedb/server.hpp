#pragma once
#include "forgedb/engine.hpp"
#include "forgedb/socket.hpp"
#include <functional>
#include <memory>

namespace forgedb {
struct ServerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6380;
    std::size_t workers = 4, max_clients = 1024, max_request_size = 1024 * 1024;
};
class Server {
public:
    Server(Engine& engine, ServerConfig config);
    ~Server();
    void run(const std::function<bool()>& stop_requested);
private:
    struct Worker;
    Engine& engine_;
    ServerConfig config_;
    std::atomic<bool> stop_{false}, failed_{false};
    std::vector<std::unique_ptr<Worker>> workers_;
    void stop();
};
}
