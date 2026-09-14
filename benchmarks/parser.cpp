#include "forgedb/resp.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>

int main() {
    using namespace forgedb;
    Command command{"MSET"};
    for (int i = 0; i < 512; ++i) {
        command.push_back("key:" + std::to_string(i)); command.emplace_back(128, 'x');
    }
    const auto frame = encode_command(command);
    constexpr int iterations = 100;
    constexpr std::size_t chunk = 64;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        RequestParser parser;
        for (std::size_t offset = 0; offset < frame.size(); offset += chunk) {
            parser.append(std::string_view(frame).substr(offset, std::min(chunk, frame.size() - offset)));
            auto parsed = parser.next();
            if (offset + chunk >= frame.size()) {
                if (!parsed || *parsed != command) return 1;
            } else if (parsed) return 1;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "{\"iterations\":" << iterations << ",\"frame_bytes\":" << frame.size()
        << ",\"fragment_bytes\":" << chunk << ",\"total_ms\":" << ms
        << ",\"ms_per_frame\":" << ms / iterations << "}\n";
}
