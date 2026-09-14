#include "forgedb/engine.hpp"
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <thread>

namespace {
int checks = 0;
void check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(x) check((x), #x, __LINE__)
template<class Fn> void throws(Fn fn) {
    bool threw = false;
    try { fn(); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("forgedb-unit-" + std::to_string(std::random_device{}()));
    Temp() { std::filesystem::create_directories(path); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void parser_tests() {
    using namespace forgedb;
    const Command command{"SET", "a", std::string("hello\r\n\0world", 13)};
    const auto encoded = encode_command(command);
    for (std::size_t split = 0; split < encoded.size(); ++split) {
        RequestParser parser; parser.append(std::string_view(encoded).substr(0, split));
        CHECK(!parser.next()); parser.append(std::string_view(encoded).substr(split));
        CHECK(parser.next() == command); CHECK(!parser.next());
    }
    RequestParser bytes;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        bytes.append(std::string_view(encoded).substr(i, 1));
        if (i + 1 == encoded.size()) CHECK(bytes.next() == command); else CHECK(!bytes.next());
    }
    RequestParser pipeline; pipeline.append(encoded + "GET a\r\nPING\r\n");
    CHECK(pipeline.next() == command); CHECK(pipeline.next() == Command({"GET", "a"}));
    CHECK(pipeline.next() == Command({"PING"}));
    CHECK(tokenize("SET k \"a b\\n\"") == Command({"SET", "k", "a b\n"}));
    for (const auto& invalid : {"\r\n", "*0\r\n", "*-1\r\n", "*9999999999\r\n", "*1\r\n$-1\r\n", "*1\r\n$1\r\naXX", "*1\r\n+foo\r\n", "SET x \"\r\n"}) {
        throws([&] { RequestParser p; p.append(invalid); p.next(); });
    }
    throws([] { RequestParser p(64); p.append("*1\r\n$100\r\n"); p.next(); });
    CHECK(!parse_integer("9223372036854775808")); CHECK(!parse_integer("1x"));
    CHECK(parse_integer("-9223372036854775808") == std::numeric_limits<std::int64_t>::min());
    CHECK(Reply::array({Reply::integer(1), Reply{}}).encode() == "*2\r\n:1\r\n$-1\r\n");
    std::mt19937 rng(42);
    for (int trial = 0; trial < 200; ++trial) {
        Command random_command{"MSET"};
        for (int arg = 0; arg < 20; ++arg) {
            std::string value(rng() % 200, '\0');
            for (auto& ch : value) ch = static_cast<char>(rng() & 255);
            random_command.push_back(std::move(value));
        }
        const auto wire = encode_command(random_command);
        RequestParser p;
        for (std::size_t i = 0; i < wire.size();) {
            const auto n = std::min<std::size_t>(rng() % 50 + 1, wire.size() - i);
            p.append(std::string_view(wire).substr(i, n)); i += n;
            if (i == wire.size()) CHECK(p.next() == random_command); else CHECK(!p.next());
        }
    }
    throws([] {
        RequestParser p(64); p.append("*3\r\n$20\r\n" + std::string(20, 'x') + "\r\n"); CHECK(!p.next());
        p.append("$20\r\n" + std::string(20, 'x') + "\r\n"); CHECK(!p.next());
        p.append("$20\r\n"); p.next();
    });
}
void storage_tests() {
    using namespace forgedb;
    Store store;
    store.apply({"x", Entry{"value", 100}});
    CHECK(store.get("x", 99)->value == "value"); CHECK(!store.get("x", 100));
    CHECK(store.expired() == 1);
    store.apply({"x", Entry{"old", 100}}); store.apply({"x", Entry{"new", 200}});
    store.expire(150); CHECK(store.get("x", 150)->value == "new");
    store.apply({"x", Entry{"persist", -1}}); store.expire(300); CHECK(store.size() == 1);
    store.apply({"x", {}}); CHECK(store.size() == 0);
}
void engine_tests() {
    using namespace forgedb;
    Temp temp;
    std::atomic<std::int64_t> now{10000};
    Engine engine(temp.path, FsyncPolicy::always, false, [&] { return now.load(); });
    Session s;
    auto call = [&](Command c) { return s.execute(engine, c); };
    CHECK(call({"SET", "x"}).type == Reply::Type::error);
    CHECK(call({"WHAT"}).type == Reply::Type::error);
    CHECK(call({"MSET", "a", "1", "b", "two"}).text == "OK");
    CHECK(call({"MGET", "a", "b", "c"}).elements[2].type == Reply::Type::null);
    CHECK(call({"INCR", "a"}).number == 2);
    CHECK(call({"INCR", "b"}).type == Reply::Type::error);
    call({"SET", "max", "9223372036854775807"});
    CHECK(call({"INCR", "max"}).type == Reply::Type::error);
    call({"SET", "min", "-9223372036854775808"});
    CHECK(call({"DECR", "min"}).type == Reply::Type::error);
    CHECK(call({"INCRBY", "min", "-9223372036854775808"}).type == Reply::Type::error);
    CHECK(call({"EXPIRE", "a", "2"}).number == 1); CHECK(call({"TTL", "a"}).number == 2);
    now = 11500; CHECK(call({"TTL", "a"}).number == 0);
    CHECK(call({"INCR", "a"}).number == 3); now = 12000; CHECK(call({"GET", "a"}).type == Reply::Type::null);
    CHECK(call({"TTL", "a"}).number == -2);
    call({"SET", "a", "new"}); call({"EXPIRE", "a", "1"}); call({"PERSIST", "a"});
    now = 15000; CHECK(call({"TTL", "a"}).number == -1);
    call({"EXPIRE", "a", "0"}); CHECK(call({"EXISTS", "a"}).number == 0);
    CHECK(call({"MULTI"}).text == "OK"); call({"SET", "tx", "1"}); call({"INCR", "tx"});
    CHECK(engine.execute({"GET", "tx"}).type == Reply::Type::null);
    auto result = call({"EXEC"}); CHECK(result.elements.size() == 2); CHECK(result.elements[1].number == 2);
    call({"MULTI"}); call({"SET", "discard", "x"}); call({"BOGUS"}); CHECK(call({"EXEC"}).type == Reply::Type::error);
    CHECK(call({"GET", "discard"}).type == Reply::Type::null);
    call({"MULTI"}); call({"INCR", "b"}); call({"SET", "after-error", "yes"});
    result = call({"EXEC"}); CHECK(result.elements[0].type == Reply::Type::error); CHECK(result.elements[1].text == "OK");
    call({"MULTI"}); call({"SET", "discard", "x"}); CHECK(call({"DISCARD"}).text == "OK");
    CHECK(call({"EXEC"}).type == Reply::Type::error);
    call({"MULTI"}); CHECK(call({"EXEC"}).elements.empty());
    std::vector<std::thread> threads;
    for (int i = 0; i < 100; ++i) threads.emplace_back([&] {
        for (int j = 0; j < 1000; ++j) {
            if (engine.execute({"INCR", "counter"}).type != Reply::Type::integer) std::terminate();
        }
    });
    for (auto& t : threads) t.join();
    CHECK(call({"GET", "counter"}).text == "100000");
}
void persistence_tests() {
    using namespace forgedb;
    CHECK(crc32("123456789") == 0xcbf43926U);
    const std::vector<Mutation> mutations{{"key\n", Entry{std::string("a\0b", 3), 123456}}, {"gone", {}}};
    const auto record = encode_record(mutations);
    const auto decoded = decode_payload(std::string_view(record).substr(16));
    CHECK(decoded.size() == 2); CHECK(decoded[0].entry == mutations[0].entry); CHECK(!decoded[1].entry);
    throws([&] { decode_payload(std::string_view(record).substr(16, 9)); });
    Temp temp;
    {
        Engine engine(temp.path, FsyncPolicy::always);
        CHECK(engine.execute({"SET", "binary", std::string("a\0\nb", 4)}).text == "OK");
        CHECK(engine.transaction({{"SET", "a", "1"}, {"INCR", "a"}}).elements[1].number == 2);
        throws([&] { Aof duplicate(temp.path, FsyncPolicy::always, [](const Mutation&) {}); });
        engine.shutdown();
    }
    { Engine engine(temp.path, FsyncPolicy::always); CHECK(engine.execute({"GET", "a"}).text == "2"); CHECK(engine.execute({"GET", "binary"}).text == std::string("a\0\nb", 4)); }
    const auto path = temp.path / "append.aof";
    auto original = std::filesystem::file_size(path);
    { File file(path, File::Mode::append); file.append(record.substr(0, record.size() - 2)); }
    { Engine engine(temp.path, FsyncPolicy::always); CHECK(engine.execute({"GET", "a"}).text == "2"); }
    CHECK(std::filesystem::file_size(path) == original);
    { File file(path, File::Mode::append); auto broken = record; broken.back() ^= 1; file.append(broken); }
    throws([&] { Engine engine(temp.path, FsyncPolicy::always); });
    CHECK(std::filesystem::file_size(path) == original + record.size());
    Temp compact_temp;
    {
        Aof log(compact_temp.path, FsyncPolicy::none, [](const Mutation&) {});
        const auto big_record = encode_record({{"large", Entry{std::string(1024 * 1024, 'x'), -1}}});
        log.append(big_record);
        log.begin_compaction();
        auto candidate = log.write_snapshot({{"large", Entry{"snapshot", -1}}});
        for (int i = 0; i < 33; ++i) log.append(big_record);
        throws([&] { log.finish_compaction(std::move(candidate)); });
        CHECK(log.healthy()); log.cancel_compaction();
        log.append(encode_record({{"large", Entry{"final", -1}}}));
        log.begin_compaction();
        auto second = log.write_snapshot({{"large", Entry{"final", -1}}});
        log.finish_compaction(std::move(second));
    }
    {
        Store store;
        Aof log(compact_temp.path, FsyncPolicy::always, [&](const Mutation& m) { store.apply(m); });
        CHECK(store.get("large", 0)->value == "final"); CHECK(log.size() < 100);
    }
}
}
int main() {
    try {
        parser_tests(); std::cout << "parser: passed\n";
        storage_tests(); std::cout << "storage: passed\n";
        engine_tests(); std::cout << "engine + 100-thread stress: passed\n";
        persistence_tests(); std::cout << "persistence: passed\n";
        std::cout << checks << " checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
