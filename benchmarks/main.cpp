#include "forgedb/client.hpp"
#include "forgedb/options.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <thread>

int main(int argc, char** argv) {
    using namespace forgedb;
    using Clock = std::chrono::steady_clock;
    try {
        std::string host = "127.0.0.1", workload = "GET";
        std::uint16_t port = 6380;
        std::size_t clients = 8, requests = 10000, payload = 64, keyspace = 1000;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "forgedb-benchmark [--host IPv4] [--port N] [--clients N] [--requests TOTAL]\n"
                    "  [--command GET|SET|MIXED|INCR] [--value-size BYTES] [--keyspace N]\n"; return 0;
            }
            const auto v = option_value(i, argc, argv);
            if (arg == "--host") host = v;
            else if (arg == "--port") port = static_cast<std::uint16_t>(bounded_number(v, 1, 65535));
            else if (arg == "--clients") clients = static_cast<std::size_t>(bounded_number(v, 1, 1024));
            else if (arg == "--requests") requests = static_cast<std::size_t>(bounded_number(v, 1, 100000000));
            else if (arg == "--value-size") payload = static_cast<std::size_t>(bounded_number(v, 0, 512 * 1024));
            else if (arg == "--keyspace") keyspace = static_cast<std::size_t>(bounded_number(v, 1, 1000000));
            else if (arg == "--command") workload = uppercase(v);
            else throw std::invalid_argument("unknown benchmark option");
        }
        if (workload != "GET" && workload != "SET" && workload != "MIXED" && workload != "INCR") throw std::invalid_argument("invalid workload");
        if (clients > requests) throw std::invalid_argument("clients cannot exceed request count");
        const std::string value(payload, 'x');
        // Deterministic, untimed seed phase. All GETs hit existing values.
        Client seed(host, port);
        for (std::size_t i = 0; i < keyspace && workload != "INCR"; ++i)
            if (seed.command({"SET", "bench:" + std::to_string(i), value}).type != Reply::Type::simple)
                throw std::runtime_error("benchmark seed failed");
        if (workload == "INCR" && seed.command({"SET", "bench:counter", "0"}).type != Reply::Type::simple)
            throw std::runtime_error("counter seed failed");
        std::mutex mutex;
        std::condition_variable gate;
        std::size_t ready = 0;
        bool go = false;
        struct Result { std::vector<double> latencies; std::size_t errors = 0; };
        std::vector<Result> results(clients);
        std::vector<std::thread> threads;
        for (std::size_t id = 0; id < clients; ++id) threads.emplace_back([&, id] {
            auto& result = results[id];
            const auto count = requests / clients + (id < requests % clients ? 1 : 0);
            result.latencies.reserve(count);
            std::unique_ptr<Client> client;
            try {
                client = std::make_unique<Client>(host, port);
                if (client->command({"PING"}).text != "PONG") throw std::runtime_error("benchmark handshake failed");
            }
            catch (const std::exception&) { client.reset(); result.errors = count; }
            { std::unique_lock lock(mutex); ++ready; gate.notify_all(); gate.wait(lock, [&] { return go; }); }
            if (!client) return;
            for (std::size_t i = 0; i < count; ++i) {
                const auto key = "bench:" + std::to_string((i * clients + id) % keyspace);
                const bool write = workload == "SET" || (workload == "MIXED" && (i + id) % 2 == 0);
                Command command = workload == "INCR" ? Command{"INCR", "bench:counter"} :
                    write ? Command{"SET", key, value} : Command{"GET", key};
                const auto begin = Clock::now();
                try {
                    const auto reply = client->command(command);
                    const bool valid = workload == "INCR" ? reply.type == Reply::Type::integer :
                        write ? reply.type == Reply::Type::simple && reply.text == "OK" : reply.type == Reply::Type::bulk && reply.text == value;
                    if (!valid) ++result.errors;
                } catch (const std::exception&) {
                    // Stop using a potentially desynchronized connection. Count all
                    // unattempted operations as errors, never as successful work.
                    result.errors += count - i;
                    result.latencies.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
                    break;
                }
                result.latencies.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
            }
        });
        { std::unique_lock lock(mutex); gate.wait(lock, [&] { return ready == clients; }); }
        const auto begin = Clock::now();
        { std::lock_guard lock(mutex); go = true; } gate.notify_all();
        for (auto& thread : threads) thread.join();
        const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        std::vector<double> latencies;
        std::size_t errors = 0;
        for (auto& result : results) { errors += result.errors; latencies.insert(latencies.end(), result.latencies.begin(), result.latencies.end()); }
        std::sort(latencies.begin(), latencies.end());
        auto percentile = [&](double fraction) {
            if (latencies.empty()) return 0.0;
            const auto index = static_cast<std::size_t>(fraction * static_cast<double>(latencies.size() - 1));
            return latencies[index];
        };
        const double mean = latencies.empty() ? 0.0 : std::accumulate(latencies.begin(), latencies.end(), 0.0) / static_cast<double>(latencies.size());
        std::cout << std::fixed << std::setprecision(6)
            << "{\"command\":\"" << workload << "\",\"clients\":" << clients << ",\"requests\":" << requests
            << ",\"value_bytes\":" << payload << ",\"keyspace\":" << keyspace << ",\"seconds\":" << seconds
            << ",\"ops_per_second\":" << static_cast<double>(requests - errors) / seconds
            << ",\"mean_ms\":" << mean << ",\"p50_ms\":" << percentile(0.50)
            << ",\"p95_ms\":" << percentile(0.95) << ",\"p99_ms\":" << percentile(0.99)
            << ",\"errors\":" << errors << ",\"error_rate\":" << static_cast<double>(errors) / static_cast<double>(requests) << "}\n";
        return errors ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << "forgedb-benchmark: " << e.what() << '\n'; return 1; }
}
