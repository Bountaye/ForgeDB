#include "forgedb/server.hpp"
#include "forgedb/options.hpp"
#include <csignal>
#include <iostream>
#ifdef FORGEDB_TEST_DRIVER
#include <fstream>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
extern "C" void signal_handler(int) { interrupted = 1; }
#ifdef _WIN32
std::atomic<bool> console_stop{false};
BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) { console_stop = true; return TRUE; }
    return FALSE;
}
#endif
}
int main(int argc, char** argv) {
    using namespace forgedb;
    try {
        ServerConfig config;
        std::filesystem::path directory = "data";
        FsyncPolicy policy = FsyncPolicy::always;
        bool persistence = true;
        std::int64_t run_for_ms = 0;
        PersistenceHooks hooks;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "forgedb-server [--host IPv4] [--port 6380] [--data-dir data]\n"
                    "  [--worker-threads 4] [--max-clients 1024] [--max-request-size 1048576]\n"
                    "  [--fsync-policy always|everysec|none] [--no-persistence] [--run-for-ms N]\n";
                return 0;
            }
            if (arg == "--no-persistence") { persistence = false; continue; }
            const auto value = option_value(i, argc, argv);
#ifdef FORGEDB_TEST_DRIVER
            if (arg == "--test-compaction-gate") {
                hooks.compaction_checkpoint = [path = std::filesystem::path(value)] {
                    std::ofstream(path / "entered").put('1');
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                    while (!std::filesystem::exists(path / "release")) {
                        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("test compaction gate timed out");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                };
                continue;
            }
            if (arg == "--test-fail-write") {
                hooks.append = [failure = bounded_number(value, 1, 1000000), count = std::int64_t{0}](File& file, std::string_view record) mutable {
                    if (++count == failure) {
                        file.append(record.substr(0, record.size() / 2)); file.sync();
                        throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "injected partial write");
                    }
                    file.append(record);
                };
                continue;
            }
#endif
            if (arg == "--host") config.host = value;
            else if (arg == "--port") config.port = static_cast<std::uint16_t>(bounded_number(value, 1, 65535));
            else if (arg == "--data-dir") directory = value;
            else if (arg == "--worker-threads") config.workers = static_cast<std::size_t>(bounded_number(value, 1, 128));
            else if (arg == "--max-clients") config.max_clients = static_cast<std::size_t>(bounded_number(value, 1, 100000));
            else if (arg == "--max-request-size") config.max_request_size = static_cast<std::size_t>(bounded_number(value, 64, 8 * 1024 * 1024));
            else if (arg == "--run-for-ms") run_for_ms = bounded_number(value, 1, 86400000);
            else if (arg == "--fsync-policy") {
                if (value == "always") policy = FsyncPolicy::always;
                else if (value == "everysec") policy = FsyncPolicy::everysec;
                else if (value == "none") policy = FsyncPolicy::none;
                else throw std::invalid_argument("unknown fsync policy");
            } else throw std::invalid_argument("unknown option: " + arg);
        }
        std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
#ifdef _WIN32
        SetConsoleCtrlHandler(console_handler, TRUE);
#else
        std::signal(SIGPIPE, SIG_IGN);
#endif
        Engine engine(directory, policy, persistence, wall_time_ms, std::move(hooks));
        Server server(engine, config);
        const auto started = std::chrono::steady_clock::now();
        std::cout << "Persistence: " << (persistence ? "enabled" : "disabled") << '\n';
        server.run([&] {
            bool stopped = interrupted != 0;
#ifdef _WIN32
            stopped = stopped || console_stop.load();
#endif
            return stopped || (run_for_ms && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(run_for_ms));
        });
        engine.shutdown();
        std::cout << "Shutdown complete\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "ForgeDB: " << e.what() << '\n'; return 1; }
}
