#include "forgedb/resp.hpp"
#include <charconv>
#include <cctype>
#include <limits>

namespace forgedb {
Reply Reply::ok(std::string s) { Reply r; r.type = Type::simple; r.text = std::move(s); return r; }
Reply Reply::error(std::string s) { Reply r; r.type = Type::error; r.text = std::move(s); return r; }
Reply Reply::integer(std::int64_t n) { Reply r; r.type = Type::integer; r.number = n; return r; }
Reply Reply::bulk(std::string s) { Reply r; r.type = Type::bulk; r.text = std::move(s); return r; }
Reply Reply::array(std::vector<Reply> a) { Reply r; r.type = Type::array; r.elements = std::move(a); return r; }
std::string Reply::encode() const {
    switch (type) {
    case Type::simple: return "+" + text + "\r\n";
    case Type::error: return "-ERR " + text + "\r\n";
    case Type::integer: return ":" + std::to_string(number) + "\r\n";
    case Type::bulk: return "$" + std::to_string(text.size()) + "\r\n" + text + "\r\n";
    case Type::null: return "$-1\r\n";
    case Type::array: {
        std::string out = "*" + std::to_string(elements.size()) + "\r\n";
        for (const auto& e : elements) out += e.encode();
        return out;
    }
    }
    throw std::logic_error("invalid reply type");
}
std::optional<std::int64_t> parse_integer(std::string_view s) {
    std::int64_t n = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || p != s.data() + s.size() || s.empty()) return {};
    return n;
}
std::string uppercase(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
Command tokenize(std::string_view line) {
    Command result;
    std::string word;
    bool started = false;
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\') {
            if (++i == line.size()) throw ProtocolError("unfinished escape");
            const char e = line[i];
            word += e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e;
            started = true;
        } else if (quote) {
            if (c == quote) quote = 0; else word += c;
        } else if (c == '\'' || c == '"') { quote = c; started = true; }
        else if (c == ' ' || c == '\t') {
            if (started) { result.push_back(std::move(word)); word.clear(); started = false; }
        } else { word += c; started = true; }
    }
    if (quote) throw ProtocolError("unterminated quote");
    if (started) result.push_back(std::move(word));
    if (result.empty() || result.size() > 4096) throw ProtocolError("invalid argument count");
    return result;
}
void RequestParser::append(std::string_view bytes) {
    // Allow one receive chunk beyond a full frame, so pipelined messages do not
    // get mistaken for a single oversized request. next() enforces frame size.
    if (bytes.size() > limit_ + 65536 || buffered() > limit_ + 65536 - bytes.size())
        throw ProtocolError("input buffer limit exceeded");
    if (offset_ != 0 && (offset_ >= data_.size() / 2 || data_.size() + bytes.size() > limit_)) {
        data_.erase(0, offset_); scan_ -= offset_; offset_ = 0;
    }
    data_.append(bytes);
}
void RequestParser::consume(std::size_t count) {
    if (count > limit_ - frame_bytes_) throw ProtocolError("request too large");
    offset_ += count; frame_bytes_ += count; scan_ = offset_;
}
std::optional<std::string_view> RequestParser::line() {
    const auto end = data_.find("\r\n", scan_);
    if (end == std::string::npos) {
        if (buffered() > limit_ - frame_bytes_) throw ProtocolError("request too large");
        // Revisit only the last byte, which might be the CR of a split CRLF.
        scan_ = data_.size() > offset_ ? data_.size() - 1 : offset_;
        return {};
    }
    auto result = std::string_view(data_).substr(offset_, end - offset_);
    consume(end + 2 - offset_); return result;
}
std::optional<Command> RequestParser::next() {
    for (;;) {
        if (state_ == State::start) {
            auto first = line();
            if (!first) return {};
            if (first->empty()) throw ProtocolError("empty request");
            if ((*first)[0] != '*') {
                auto command = tokenize(*first); frame_bytes_ = 0; return command;
            }
            const auto count = parse_integer(first->substr(1));
            if (!count || *count < 1 || *count > 4096) throw ProtocolError("invalid array length");
            remaining_ = static_cast<std::size_t>(*count); state_ = State::length;
        }
        if (state_ == State::length) {
            auto header = line();
            if (!header) return {};
            if (header->empty() || (*header)[0] != '$') throw ProtocolError("expected bulk string");
            const auto length = parse_integer(header->substr(1));
            if (!length || *length < 0 || static_cast<std::uint64_t>(*length) > limit_)
                throw ProtocolError("invalid bulk length");
            bulk_length_ = static_cast<std::size_t>(*length);
            if (bulk_length_ + 2 > limit_ - frame_bytes_) throw ProtocolError("request too large");
            state_ = State::data;
        }
        if (buffered() < bulk_length_ + 2) return {};
        if (data_.compare(offset_ + bulk_length_, 2, "\r\n") != 0) throw ProtocolError("invalid bulk terminator");
        command_.emplace_back(data_.data() + offset_, bulk_length_);
        consume(bulk_length_ + 2);
        if (--remaining_ == 0) {
            auto result = std::move(command_); command_.clear();
            state_ = State::start; frame_bytes_ = 0; return result;
        }
        state_ = State::length;
    }
}
std::string encode_command(const Command& c) {
    std::string out = "*" + std::to_string(c.size()) + "\r\n";
    for (const auto& s : c) out += "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
    return out;
}
}
