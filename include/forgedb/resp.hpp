#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace forgedb {
using Command = std::vector<std::string>;
struct ProtocolError : std::runtime_error { using std::runtime_error::runtime_error; };
struct Reply {
    enum class Type { simple, error, integer, bulk, null, array };
    Type type = Type::null;
    std::string text;
    std::int64_t number = 0;
    std::vector<Reply> elements;
    static Reply ok(std::string text = "OK");
    static Reply error(std::string text);
    static Reply integer(std::int64_t value);
    static Reply bulk(std::string value);
    static Reply array(std::vector<Reply> values);
    std::string encode() const;
};
class RequestParser {
public:
    explicit RequestParser(std::size_t limit = 1024 * 1024) : limit_(limit) {}
    void append(std::string_view bytes);
    std::optional<Command> next();
    std::size_t buffered() const { return data_.size() - offset_; }
private:
    enum class State { start, length, data };
    std::string data_;
    std::size_t offset_ = 0;
    std::size_t limit_;
    std::size_t frame_bytes_ = 0, scan_ = 0, remaining_ = 0, bulk_length_ = 0;
    State state_ = State::start;
    Command command_;
    std::optional<std::string_view> line();
    void consume(std::size_t count);
};
Command tokenize(std::string_view line);
std::string encode_command(const Command& command);
std::optional<std::int64_t> parse_integer(std::string_view text);
std::string uppercase(std::string text);
}
