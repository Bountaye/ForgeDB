#pragma once
#include "forgedb/resp.hpp"
#include "forgedb/socket.hpp"

namespace forgedb {
class Client {
public:
    Client(const std::string& host, std::uint16_t port) : socket_(net::Socket::connect(host, port)) {}
    Reply command(const Command& command);
    void send(const Command& command);
    Reply receive();
private:
    net::Socket socket_;
    std::string buffer_;
    std::size_t offset_ = 0;
    std::string take(std::size_t count);
    std::string line();
    void fill();
    Reply parse(unsigned depth, std::size_t& budget);
};
std::string display(const Reply& reply);
}
