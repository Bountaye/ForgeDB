#include "forgedb/client.hpp"
#include "forgedb/options.hpp"
#include <iostream>

int main(int argc, char** argv) {
    using namespace forgedb;
    try {
        std::string host = "127.0.0.1";
        std::uint16_t port = 6380;
        Command command;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") { std::cout << "forgedb-cli [--host IPv4] [--port N] [COMMAND args...]\n"; return 0; }
            if (arg == "--host") host = option_value(i, argc, argv);
            else if (arg == "--port") port = static_cast<std::uint16_t>(bounded_number(option_value(i, argc, argv), 1, 65535));
            else { for (; i < argc; ++i) command.emplace_back(argv[i]); }
        }
        Client client(host, port);
        if (!command.empty()) {
            const auto reply = client.command(command);
            std::cout << display(reply) << '\n'; return reply.type == Reply::Type::error ? 1 : 0;
        }
        std::string line;
        while (std::cout << host << ':' << port << "> " && std::getline(std::cin, line)) {
            if (line == "quit" || line == "exit") break;
            if (line.empty()) continue;
            try { std::cout << display(client.command(tokenize(line))) << '\n'; }
            catch (const ProtocolError& e) { std::cerr << e.what() << '\n'; }
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "forgedb-cli: " << e.what() << '\n'; return 1; }
}
