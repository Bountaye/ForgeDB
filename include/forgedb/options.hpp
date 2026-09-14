#pragma once
#include "forgedb/resp.hpp"

namespace forgedb {
inline std::int64_t bounded_number(const std::string& value, std::int64_t min, std::int64_t max) {
    auto n = parse_integer(value);
    if (!n || *n < min || *n > max) throw std::invalid_argument("numeric option out of range: " + value);
    return *n;
}
inline std::string option_value(int& index, int argc, char** argv) {
    if (++index >= argc) throw std::invalid_argument("missing option value");
    return argv[index];
}
}
