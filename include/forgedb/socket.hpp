#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <poll.h>
#endif

namespace forgedb::net {
#ifdef _WIN32
using Handle = SOCKET;
using PollFd = WSAPOLLFD;
constexpr Handle invalid = INVALID_SOCKET;
#else
using Handle = int;
using PollFd = pollfd;
constexpr Handle invalid = -1;
#endif
class Socket {
public:
    Socket() = default;
    explicit Socket(Handle handle) : handle_(handle) {}
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Handle handle() const { return handle_; }
    explicit operator bool() const { return handle_ != invalid; }
    void nonblocking();
    void nodelay();
    // -2 means would-block/interrupted, -1 means fatal, 0 means orderly EOF.
    int receive(char* data, int size);
    int send(std::string_view data);
    static Socket listen(const std::string& host, std::uint16_t port);
    static Socket connect(const std::string& host, std::uint16_t port);
    Socket accept();
private:
    Handle handle_ = invalid;
    void close() noexcept;
};
int poll(std::vector<PollFd>& fds, int timeout_ms);
}
