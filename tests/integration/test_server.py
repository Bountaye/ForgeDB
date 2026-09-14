import concurrent.futures
import json
import os
from pathlib import Path
import random
import socket
import subprocess
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from support import Client, Error, Server, eventually, frame

EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())


class Integration(unittest.TestCase):
    def setUp(self):
        self.server = Server(EXECUTABLE, options=["--no-persistence"]).start()
        self.addCleanup(self.server.close)

    def test_independent_clients_and_binary_fragments(self):
        with Client(self.server.port) as a, Client(self.server.port) as b:
            data = frame("SET", b"binary\0key", b"line\r\n\0value")
            for byte in data:
                a.socket.sendall(bytes([byte]))
            self.assertEqual(a.read(), "OK")
            self.assertEqual(b.command("GET", b"binary\0key"), b"line\r\n\0value")
            self.assertEqual(b.command("DEL", b"binary\0key"), 1)
            self.assertIsNone(a.command("GET", b"binary\0key"))

    def test_large_pipeline_and_half_close(self):
        with Client(self.server.port) as c:
            count = 5000
            c.socket.sendall(frame("INCR", "counter") * count)
            c.socket.shutdown(socket.SHUT_WR)
            self.assertEqual([c.read() for _ in range(count)], list(range(1, count + 1)))
            self.assertEqual(c.file.read(1), b"")

    def test_invalid_and_oversized_frames(self):
        for bad in [b"\r\n", b"*1\r\n$999999999\r\n", b"*1\r\n$1\r\naXX", b"*0\r\n"]:
            with Client(self.server.port) as c:
                c.socket.sendall(bad + frame("SET", "never", "yes"))
                self.assertIsInstance(c.read(), Error)
                self.assertEqual(c.file.read(1), b"")
        with Client(self.server.port) as c:
            self.assertIsNone(c.command("GET", "never"))
            self.assertIsInstance(c.command("SET", "x"), Error)
            self.assertEqual(c.command("PING"), "PONG")

    def test_concurrent_atomic_increments(self):
        barrier = threading.Barrier(32)
        def writer(_):
            with Client(self.server.port) as c:
                barrier.wait()
                for _ in range(300):
                    self.assertIsInstance(c.command("INCR", "counter"), int)
        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
            list(pool.map(writer, range(32)))
        with Client(self.server.port) as c:
            self.assertEqual(c.command("GET", "counter"), b"9600")

    def test_transactions_isolate_and_runtime_errors_continue(self):
        done = threading.Event()
        started = threading.Barrier(9)
        def writer(_):
            with Client(self.server.port) as c:
                started.wait()
                for _ in range(100):
                    replies = c.pipeline([("MULTI",), ("INCR", "a"), ("INCR", "b"), ("EXEC",)])
                    self.assertEqual(replies[0:3], ["OK", "QUEUED", "QUEUED"])
                    self.assertEqual(replies[3][0], replies[3][1])
        def reader():
            with Client(self.server.port) as c:
                started.wait()
                reads = 0
                while not done.is_set():
                    a, b = c.command("MGET", "a", "b")
                    self.assertEqual(a, b)
                    reads += 1
                return reads
        with concurrent.futures.ThreadPoolExecutor(max_workers=9) as pool:
            read = pool.submit(reader)
            writes = [pool.submit(writer, i) for i in range(8)]
            try:
                for future in writes:
                    future.result()
            finally:
                done.set()
            self.assertGreater(read.result(), 0)
        with Client(self.server.port) as c:
            self.assertEqual(c.command("MGET", "a", "b"), [b"800", b"800"])
            c.command("SET", "bad", "word")
            result = c.pipeline([("MULTI",), ("INCR", "bad"), ("SET", "after", "yes"), ("EXEC",)])[-1]
            self.assertIsInstance(result[0], Error)
            self.assertEqual(result[1], "OK")

    def test_expiration_replacement_and_concurrent_readers(self):
        with Client(self.server.port) as c:
            c.command("SET", "ttl", "old")
            c.command("EXPIRE", "ttl", 1)
            c.command("SET", "ttl", "new")
            self.assertEqual(c.command("TTL", "ttl"), -1)
            c.command("EXPIRE", "ttl", 1)
        def reader(_):
            with Client(self.server.port) as c:
                eventually(lambda: c.command("GET", "ttl") is None, timeout=3)
                self.assertEqual(c.command("TTL", "ttl"), -2)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(reader, range(8)))
        with Client(self.server.port) as c:
            self.assertGreaterEqual(int(c.info()["expired_keys"]), 1)

    def test_many_idle_clients_and_slow_consumer(self):
        idle = [Client(self.server.port) for _ in range(128)]
        try:
            with Client(self.server.port) as c:
                self.assertEqual(c.command("PING"), "PONG")
                c.command("SET", "large", "x" * 200000)
                idle[0].socket.sendall(frame("GET", "large") * 200)
                self.assertEqual(c.command("INCR", "healthy"), 1)
                self.assertIsInstance(c.command("MGET", *("large" for _ in range(100))), Error)
                self.assertEqual(c.command("PING"), "PONG")
        finally:
            for c in idle:
                c.close()

    def test_disconnect_discards_transaction(self):
        with Client(self.server.port) as c:
            c.command("MULTI")
            c.command("SET", "abandoned", "yes")
        with Client(self.server.port) as c:
            self.assertIsNone(c.command("GET", "abandoned"))

    def test_random_bad_clients_do_not_kill_server(self):
        rng = random.Random(42)
        for _ in range(50):
            with socket.create_connection(("127.0.0.1", self.server.port), timeout=5) as sock:
                sock.sendall(bytes(rng.randrange(256) for _ in range(rng.randrange(1, 256))) + b"\r\n")
        with Client(self.server.port) as c:
            self.assertEqual(c.command("PING"), "PONG")

    def test_cli_two_processes(self):
        cli = Path(EXECUTABLE).with_name("forgedb-cli.exe" if os.name == "nt" else "forgedb-cli")
        def run(*args):
            return subprocess.run([str(cli), "--port", str(self.server.port), *args], capture_output=True, text=True, timeout=10, check=True).stdout
        self.assertEqual(run("SET", "from-cli", "C++").strip(), "OK")
        self.assertEqual(run("GET", "from-cli").strip(), '"C++"')


class LimitsAndShutdown(unittest.TestCase):
    def test_benchmark_counts_rejected_clients_as_errors(self):
        benchmark = Path(EXECUTABLE).with_name("forgedb-benchmark.exe" if os.name == "nt" else "forgedb-benchmark")
        with Server(EXECUTABLE, options=["--no-persistence", "--max-clients", "2"]) as s, Client(s.port) as probe:
            # Keep this connection open; the benchmark seed occupies the second
            # slot. All workload clients must then be rejected deterministically.
            eventually(lambda: probe.info()["connected_clients"] == "1")
            result = subprocess.run([str(benchmark), "--port", str(s.port), "--command", "INCR", "--clients", "2", "--requests", "10"], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 1, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["errors"], 10)
            self.assertEqual(report["error_rate"], 1)
            self.assertEqual(report["ops_per_second"], 0)

    def test_client_limit_and_capacity_recovery(self):
        with Server(EXECUTABLE, options=["--no-persistence", "--max-clients", "4"]) as s:
            clients = []
            try:
                # Readiness connection must leave before we fill all slots.
                with Client(s.port) as probe:
                    eventually(lambda: int(probe.info()["connected_clients"]) == 1)
                for _ in range(4):
                    c = Client(s.port)
                    self.assertEqual(c.command("PING"), "PONG")
                    clients.append(c)
                with Client(s.port) as rejected:
                    self.assertIsInstance(rejected.read(), Error)
                clients.pop().close()
                eventually(lambda: clients[0].info()["connected_clients"] == "3")
                with Client(s.port) as accepted:
                    self.assertEqual(accepted.command("PING"), "PONG")
            finally:
                for c in clients:
                    c.close()

    def test_graceful_shutdown_flushes(self):
        opts = ["--fsync-policy", "none"]
        if os.name == "nt":
            opts += ["--run-for-ms", "1500"]
        with Server(EXECUTABLE, options=opts) as s:
            with Client(s.port) as c:
                c.command("SET", "shutdown", "saved")
            s.graceful()
            s.options = []
            s.restart()
            with Client(s.port) as c:
                self.assertEqual(c.command("GET", "shutdown"), b"saved")


if __name__ == "__main__":
    unittest.main(verbosity=2)
