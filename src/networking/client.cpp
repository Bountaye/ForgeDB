#include "forgedb/client.hpp"
#include <iomanip>
#include <sstream>

namespace forgedb {
void Client::fill() {
    if (offset_) { buffer_.erase(0, offset_); offset_ = 0; }
    char data[8192];
    const int n = socket_.receive(data, sizeof(data));
    if (n <= 0) throw std::runtime_error("server disconnected or receive timed out");
    buffer_.append(data, static_cast<std::size_t>(n));
}
std::string Client::take(std::size_t count) {
    while (buffer_.size() - offset_ < count) fill();
    auto s = buffer_.substr(offset_, count); offset_ += count; return s;
}
std::string Client::line() {
    for (;;) {
        const auto end = buffer_.find("\r\n", offset_);
        if (end != std::string::npos) { auto s = buffer_.substr(offset_, end - offset_); offset_ = end + 2; return s; }
        if (buffer_.size() - offset_ > 65536) throw ProtocolError("response header too large");
        fill();
    }
}
void Client::send(const Command& command) {
    const auto frame = encode_command(command);
    std::string_view bytes(frame);
    while (!bytes.empty()) {
        const auto n = socket_.send(bytes);
        if (n <= 0) throw std::runtime_error("send failed or timed out");
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
}
Reply Client::parse(unsigned depth, std::size_t& budget) {
    if (depth > 4 || budget < 32) throw ProtocolError("response limit exceeded");
    budget -= 32;
    const auto header = line();
    if (header.empty()) throw ProtocolError("empty response");
    const auto body = header.substr(1);
    switch (header[0]) {
    case '+': return Reply::ok(body);
    case '-': return Reply::error(body);
    case ':': {
        auto n = parse_integer(body); if (!n) throw ProtocolError("invalid integer response");
        return Reply::integer(*n);
    }
    case '$': {
        auto n = parse_integer(body);
        if (n && *n == -1) return {};
        if (!n || *n < 0 || static_cast<std::uint64_t>(*n) > budget) throw ProtocolError("invalid bulk response");
        budget -= static_cast<std::size_t>(*n);
        auto value = take(static_cast<std::size_t>(*n));
        if (take(2) != "\r\n") throw ProtocolError("invalid response terminator");
        return Reply::bulk(std::move(value));
    }
    case '*': {
        auto n = parse_integer(body);
        if (!n || *n < 0 || *n > 65536) throw ProtocolError("invalid array response");
        std::vector<Reply> values;
        for (std::int64_t i = 0; i < *n; ++i) values.push_back(parse(depth + 1, budget));
        return Reply::array(std::move(values));
    }
    default: throw ProtocolError("unknown response type");
    }
}
Reply Client::receive() { std::size_t budget = 17 * 1024 * 1024; return parse(0, budget); }
Reply Client::command(const Command& command) { send(command); return receive(); }
std::string display(const Reply& r) {
    switch (r.type) {
    case Reply::Type::null: return "(nil)";
    case Reply::Type::integer: return "(integer) " + std::to_string(r.number);
    case Reply::Type::error: return "(error) " + r.text;
    case Reply::Type::simple: return r.text;
    case Reply::Type::bulk: { std::ostringstream s; s << std::quoted(r.text); return s.str(); }
    case Reply::Type::array: {
        std::string out;
        for (std::size_t i = 0; i < r.elements.size(); ++i) out += std::to_string(i + 1) + ") " + display(r.elements[i]) + "\n";
        return out.empty() ? "(empty array)" : out;
    }
    }
    throw std::logic_error("invalid reply");
}
}
