#include "forgedb/server.hpp"
#include <array>
#include <iostream>

namespace forgedb {
struct Server::Worker {
    struct Connection {
        net::Socket socket;
        RequestParser parser;
        Session session;
        std::string output;
        std::size_t sent = 0;
        bool closing = false, dead = false, runnable = false;
        Connection(net::Socket s, std::size_t limit) : socket(std::move(s)), parser(limit) {}
    };
    Server& owner;
    std::mutex pending_mutex;
    std::condition_variable wake;
    std::vector<net::Socket> pending;
    std::vector<Connection> clients;
    std::thread thread;
    explicit Worker(Server& server) : owner(server), thread([this] { loop(); }) {}
    ~Worker() { if (thread.joinable()) thread.join(); }
    void enqueue(net::Socket socket) {
        { std::lock_guard lock(pending_mutex); pending.push_back(std::move(socket)); }
        wake.notify_one();
    }
    bool service(Connection& c, short events) {
        if (events & (POLLERR | POLLNVAL)) return false;
        try {
            if (!c.closing && c.output.size() - c.sent < 65536 && (events & (POLLIN | POLLHUP))) {
                std::array<char, 16384> buffer{};
                const int n = c.socket.receive(buffer.data(), static_cast<int>(buffer.size()));
                if (n == -1) return false;
                if (n == 0) c.closing = true;
                if (n > 0) { c.parser.append(std::string_view(buffer.data(), static_cast<std::size_t>(n))); c.runnable = true; }
            }
            for (int budget = 0; budget < 64 && c.output.size() - c.sent < 65536; ++budget) {
                auto command = c.parser.next();
                if (!command) { c.runnable = false; break; }
                auto response = c.session.execute(owner.engine_, *command).encode();
                if (response.size() > max_response_size || c.output.size() - c.sent + response.size() > max_response_size + 65536)
                    throw ProtocolError("output limit exceeded");
                c.output += response;
            }
        } catch (const ProtocolError& e) {
            c.output += Reply::error(e.what()).encode(); c.closing = true;
            // Never execute any later bytes following a malformed frame.
            c.parser = RequestParser(owner.config_.max_request_size);
            c.runnable = false;
        }
        if (c.sent < c.output.size()) {
            const auto n = c.socket.send(std::string_view(c.output).substr(c.sent));
            if (n == -1 || n == 0) return false;
            if (n > 0) c.sent += static_cast<std::size_t>(n);
            if (c.sent == c.output.size()) { c.output.clear(); c.sent = 0; }
            else if (c.sent >= 65536) { c.output.erase(0, c.sent); c.sent = 0; }
        }
        return !(c.closing && c.output.empty() && !c.runnable);
    }
    void loop() {
        try {
            while (!owner.stop_.load()) {
                {
                    std::unique_lock lock(pending_mutex);
                    if (clients.empty() && pending.empty()) wake.wait_for(lock, std::chrono::milliseconds(20));
                    for (auto& s : pending) clients.emplace_back(std::move(s), owner.config_.max_request_size);
                    pending.clear();
                }
                if (clients.empty()) continue;
                std::vector<net::PollFd> fds;
                fds.reserve(clients.size());
                bool buffered = false;
                for (const auto& c : clients) {
                    short events = c.closing || c.output.size() - c.sent >= 65536 ? 0 : POLLIN;
                    if (!c.output.empty()) events = static_cast<short>(events | POLLOUT);
                    fds.push_back({c.socket.handle(), events, 0});
                    if (c.runnable && c.output.size() - c.sent < 65536) buffered = true;
                }
                net::poll(fds, buffered ? 0 : 20);
                for (std::size_t i = 0; i < clients.size(); ++i) {
                    if (!service(clients[i], fds[i].revents)) clients[i].dead = true;
                }
                const auto removed = std::erase_if(clients, [](const auto& c) { return c.dead; });
                owner.engine_.stats.clients.fetch_sub(removed);
            }
        } catch (const std::exception& e) {
            std::cerr << "Network worker failed: " << e.what() << '\n';
            owner.failed_ = true; owner.stop_ = true;
        }
        owner.engine_.stats.clients.fetch_sub(clients.size()); clients.clear();
        std::lock_guard lock(pending_mutex);
        owner.engine_.stats.clients.fetch_sub(pending.size()); pending.clear();
    }
};
Server::Server(Engine& engine, ServerConfig config) : engine_(engine), config_(std::move(config)) {
    engine_.stats.workers = config_.workers;
}
Server::~Server() { stop(); }
void Server::stop() {
    stop_ = true;
    for (auto& w : workers_) w->wake.notify_one();
    workers_.clear();
}
void Server::run(const std::function<bool()>& requested) {
    auto listener = net::Socket::listen(config_.host, config_.port);
    for (std::size_t i = 0; i < config_.workers; ++i) workers_.push_back(std::make_unique<Worker>(*this));
    std::cout << "ForgeDB v1.0\nListening on " << config_.host << ':' << config_.port
              << "\nWorkers: " << config_.workers << '\n' << std::flush;
    std::size_t next = 0;
    while (!stop_.load() && !requested()) {
        std::vector<net::PollFd> fds{{listener.handle(), POLLIN, 0}};
        if (net::poll(fds, 20) <= 0) continue;
        // Bounded accept work guarantees the shutdown predicate is revisited.
        for (int budget = 0; budget < 128; ++budget) {
            auto socket = listener.accept(); if (!socket) break;
            if (engine_.stats.clients.load() >= config_.max_clients) {
                socket.send("-ERR maximum clients reached\r\n"); continue;
            }
            ++engine_.stats.clients;
            workers_[next++ % workers_.size()]->enqueue(std::move(socket));
        }
    }
    stop();
    if (failed_) throw std::runtime_error("network worker failed");
}
}
