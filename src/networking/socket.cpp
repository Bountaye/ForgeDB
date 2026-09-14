#include "forgedb/socket.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace forgedb::net {
namespace {
void initialize() {
#ifdef _WIN32
    struct Winsock {
        Winsock() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed"); }
        ~Winsock() { WSACleanup(); }
    };
    static Winsock winsock;
#endif
}
int last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
bool transient() {
    const int e = last_error();
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == EINTR;
#endif
}
[[noreturn]] void socket_error(const char* action) { throw std::system_error(last_error(), std::system_category(), action); }
sockaddr_in address(const std::string& host, std::uint16_t port) {
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) throw std::invalid_argument("host must be an IPv4 address");
    return addr;
}
}
Socket::~Socket() { close(); }
Socket::Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, invalid)) {}
Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) { close(); handle_ = std::exchange(other.handle_, invalid); } return *this;
}
void Socket::close() noexcept {
    if (handle_ == invalid) return;
#ifdef _WIN32
    closesocket(handle_);
#else
    ::close(handle_);
#endif
    handle_ = invalid;
}
void Socket::nonblocking() {
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(handle_, static_cast<long>(FIONBIO), &mode)) socket_error("nonblocking socket");
#else
    const int flags = fcntl(handle_, F_GETFL, 0);
    if (flags < 0 || fcntl(handle_, F_SETFL, flags | O_NONBLOCK) < 0) socket_error("nonblocking socket");
#endif
}
void Socket::nodelay() {
    const int enabled = 1;
    if (setsockopt(handle_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled))) socket_error("TCP_NODELAY");
}
int Socket::receive(char* data, int size) {
#ifdef _WIN32
    const auto n = ::recv(handle_, data, size, 0);
#else
    const auto n = ::recv(handle_, data, static_cast<std::size_t>(size), 0);
#endif
    return n < 0 ? (transient() ? -2 : -1) : static_cast<int>(n);
}
int Socket::send(std::string_view bytes) {
    const auto n = static_cast<int>(std::min<std::size_t>(bytes.size(), 65536));
#ifdef _WIN32
    const int result = ::send(handle_, bytes.data(), n, 0);
#else
    const auto result = ::send(handle_, bytes.data(), static_cast<std::size_t>(n), MSG_NOSIGNAL);
#endif
    return result < 0 ? (transient() ? -2 : -1) : static_cast<int>(result);
}
Socket Socket::listen(const std::string& host, std::uint16_t port) {
    initialize();
    Socket result(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!result) socket_error("socket");
    const int enabled = 1;
#ifdef _WIN32
    if (setsockopt(result.handle_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&enabled), sizeof(enabled))) socket_error("exclusive bind");
#else
    if (setsockopt(result.handle_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled))) socket_error("reuse address");
#endif
    const auto addr = address(host, port);
    if (::bind(result.handle_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr))) socket_error("bind");
    if (::listen(result.handle_, SOMAXCONN)) socket_error("listen");
    result.nonblocking(); return result;
}
Socket Socket::connect(const std::string& host, std::uint16_t port) {
    initialize();
    Socket result(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!result) socket_error("socket");
    const auto addr = address(host, port);
    if (::connect(result.handle_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr))) socket_error("connect");
    result.nodelay();
#ifdef _WIN32
    const DWORD timeout = 10000;
#else
    const timeval timeout{10, 0};
#endif
    if (setsockopt(result.handle_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) ||
        setsockopt(result.handle_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout))) socket_error("socket timeout");
    return result;
}
Socket Socket::accept() {
    Socket result(::accept(handle_, nullptr, nullptr));
    if (!result) {
        const auto error = last_error();
#ifdef _WIN32
        const bool aborted = error == WSAECONNRESET || error == WSAECONNABORTED;
#else
        const bool aborted = error == ECONNABORTED || error == EPROTO;
#endif
        if (transient() || aborted) return {};
        socket_error("accept");
    }
    result.nonblocking(); result.nodelay(); return result;
}
int poll(std::vector<PollFd>& fds, int timeout_ms) {
#ifdef _WIN32
    const int n = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
#else
    const int n = ::poll(fds.data(), fds.size(), timeout_ms);
#endif
    if (n < 0 && !transient()) socket_error("poll");
    return n;
}
}
