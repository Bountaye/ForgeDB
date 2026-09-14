import concurrent.futures
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from support import Client, Error, Server, eventually, rejected_start

EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())
TEST_EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())


class Recovery(unittest.TestCase):
    def test_partial_disk_write_faults_writes_and_does_not_publish_transaction(self):
        with Server(TEST_EXECUTABLE, options=["--test-fail-write", "2"]) as s:
            with Client(s.port) as c:
                self.assertEqual(c.command("SET", "base", "kept"), "OK")
                result = c.pipeline([("MULTI",), ("SET", "a", "new"), ("SET", "b", "new"), ("EXEC",)])[-1]
                self.assertIsInstance(result, Error)
                self.assertEqual(c.command("MGET", "base", "a", "b"), [b"kept", None, None])
                self.assertIsInstance(c.command("SET", "later", "rejected"), Error)
                self.assertEqual(c.info()["persistence"], "faulted")
                self.assertEqual(int(c.info()["aof_bytes"]), (s.directory / "append.aof").stat().st_size)
            s.executable = EXECUTABLE
            s.options = []
            s.restart()
            with Client(s.port) as c:
                self.assertEqual(c.command("MGET", "base", "a", "b", "later"), [b"kept", None, None, None])
                self.assertEqual(c.info()["recovered_tail"], "1")
                self.assertEqual(c.command("SET", "after-restart", "works"), "OK")

    def test_kill_restart_preserves_acknowledged_mutations(self):
        with Server(EXECUTABLE) as s:
            with Client(s.port) as c:
                for i in range(250):
                    self.assertEqual(c.command("SET", f"key:{i}", f"value:{i}"), "OK")
                c.command("DEL", "key:3")
                c.command("SET", "ttl", "expires while down")
                c.command("EXPIRE", "ttl", 1)
                c.command("SET", "long-ttl", "survives")
                c.command("EXPIRE", "long-ttl", 60)
            s.kill()  # TerminateProcess on Windows, SIGKILL on POSIX.
            time.sleep(1.05)  # Advance the actual persisted wall-clock deadline while offline.
            s.restart()
            with Client(s.port) as c:
                for i in range(250):
                    self.assertEqual(c.command("GET", f"key:{i}"), None if i == 3 else f"value:{i}".encode())
                self.assertIsNone(c.command("GET", "ttl"))
                self.assertLess(c.command("TTL", "long-ttl"), 60)
                self.assertGreater(c.command("TTL", "long-ttl"), 0)

    def test_everysec_background_sync(self):
        with Server(EXECUTABLE, options=["--fsync-policy", "everysec"]) as s:
            with Client(s.port) as c:
                c.command("SET", "periodic", "yes")
                eventually(lambda: int(c.info()["aof_syncs"]) >= 1, timeout=3)
            s.restart()
            with Client(s.port) as c:
                self.assertEqual(c.command("GET", "periodic"), b"yes")

    def test_every_truncation_of_final_transaction_is_atomic(self):
        with Server(EXECUTABLE) as source:
            with Client(source.port) as c:
                c.command("SET", "base", "kept")
                prefix_size = int(c.info()["aof_bytes"])
                self.assertEqual(c.pipeline([("MULTI",), ("SET", "a", "1"), ("SET", "b", "2"), ("EXEC",)])[-1], ["OK", "OK"])
            source.kill()
            data = (source.directory / "append.aof").read_bytes()
            for cut in range(prefix_size + 1, len(data)):
                with self.subTest(cut=cut), tempfile.TemporaryDirectory(prefix="forgedb-cut-") as directory:
                    path = Path(directory) / "append.aof"
                    path.write_bytes(data[:cut])
                    with Server(EXECUTABLE, directory=directory) as recovered:
                        with Client(recovered.port) as c:
                            self.assertEqual(c.command("GET", "base"), b"kept")
                            self.assertEqual(c.command("MGET", "a", "b"), [None, None])
                            self.assertEqual(c.info()["recovered_tail"], "1")
                        self.assertEqual(path.stat().st_size, prefix_size)

    def test_complete_corruption_is_rejected_without_modifying_log(self):
        with Server(EXECUTABLE) as source:
            with Client(source.port) as c:
                c.command("SET", "base", "kept")
                c.command("SET", "tail", "last")
            source.kill()
            data = (source.directory / "append.aof").read_bytes()
            for offset in [0, 4, 8, 12, 20, len(data) - 1]:
                with self.subTest(offset=offset), tempfile.TemporaryDirectory(prefix="forgedb-corrupt-") as directory:
                    corrupted = bytearray(data)
                    corrupted[offset] ^= 0x40
                    path = Path(directory) / "append.aof"
                    path.write_bytes(corrupted)
                    with rejected_start(EXECUTABLE, directory) as error:
                        self.assertIn("AOF", error)
                    self.assertEqual(path.read_bytes(), corrupted)

    def test_valid_checksum_malformed_payload_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="forgedb-malformed-") as directory:
            payload = struct.pack("<I", 1) + b"\x7f" + struct.pack("<I", 0)
            header = b"FDB1" + struct.pack("<II", len(payload), zlib.crc32(payload))
            (Path(directory) / "append.aof").write_bytes(header + struct.pack("<I", zlib.crc32(header)) + payload)
            with rejected_start(EXECUTABLE, directory) as error:
                self.assertIn("mutation tag", error)

    def test_abandoned_compaction_file_is_ignored(self):
        with Server(EXECUTABLE) as s:
            with Client(s.port) as c:
                c.command("SET", "authoritative", "yes")
            s.kill()
            (s.directory / "append.compacting").write_bytes(b"FDB1 incomplete garbage")
            s.restart()
            with Client(s.port) as c:
                self.assertEqual(c.command("GET", "authoritative"), b"yes")
            self.assertFalse((s.directory / "append.compacting").exists())

    def test_compaction_with_concurrent_mutations_and_restart(self):
        with tempfile.TemporaryDirectory(prefix="forgedb-gate-") as gate, Server(TEST_EXECUTABLE, options=["--fsync-policy", "everysec", "--test-compaction-gate", gate]) as s:
            with Client(s.port) as c:
                for i in range(600):
                    c.command("SET", f"seed:{i}", "x" * 8192)
                for i in range(1500):
                    c.command("SET", "overwritten", str(i))
                before = int(c.info()["aof_bytes"])
                self.assertEqual(c.command("COMPACT"), "Background compaction started")
                eventually(lambda: (Path(gate) / "entered").exists())
                self.assertEqual(c.info()["compacting"], "1")
            barrier = threading.Barrier(5)
            def writer(id):
                with Client(s.port) as c:
                    barrier.wait()
                    for i in range(200):
                        self.assertEqual(c.command("SET", f"writer:{id}:{i}", str(i)), "OK")
                        self.assertIsInstance(c.command("INCR", "during"), int)
                        if i % 10 == 0:
                            c.command("DEL", f"seed:{id * 100 + i // 10}")
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                futures = [pool.submit(writer, i) for i in range(4)]
                barrier.wait()
                for f in futures:
                    f.result()
            with Client(s.port) as c:
                self.assertEqual(c.info()["compacting"], "1")
                (Path(gate) / "release").touch()
                eventually(lambda: c.info()["compacting"] == "0")
                self.assertEqual(c.info()["compactions"], "1", c.info())
                self.assertEqual(c.info()["background_error"], "")
                self.assertEqual(c.command("GET", "during"), b"800")
                # A second rewrite also exercises joining the previous compactor.
                self.assertEqual(c.command("COMPACT"), "Background compaction started")
                eventually(lambda: c.info()["compacting"] == "0")
                self.assertLess(int(c.info()["aof_bytes"]), before)
            s.restart()
            with Client(s.port) as c:
                self.assertEqual(c.command("GET", "during"), b"800")
                for id in range(4):
                    for i in range(200):
                        self.assertEqual(c.command("GET", f"writer:{id}:{i}"), str(i).encode())
                    self.assertIsNone(c.command("GET", f"seed:{id * 100}"))

    def test_kill_during_compaction_keeps_old_log(self):
        with tempfile.TemporaryDirectory(prefix="forgedb-gate-") as gate, Server(TEST_EXECUTABLE, options=["--fsync-policy", "always", "--test-compaction-gate", gate]) as s:
            with Client(s.port) as c:
                # Several large batches produce a snapshot whose disk rewrite is observable.
                for i in range(100):
                    c.command("SET", f"seed:{i}", "x" * 200000)
                self.assertEqual(c.command("COMPACT"), "Background compaction started")
                eventually(lambda: (Path(gate) / "entered").exists())
                self.assertEqual(c.info()["compacting"], "1")
                s.kill()
            s.restart()
            with Client(s.port) as c:
                self.assertEqual(c.command("DBSIZE"), 100)
                self.assertEqual(c.command("GET", "seed:99"), b"x" * 200000)

    def test_data_directory_lock(self):
        with Server(EXECUTABLE) as s:
            with rejected_start(EXECUTABLE, s.directory):
                pass
            with Client(s.port) as c:
                self.assertEqual(c.command("SET", "still", "healthy"), "OK")


if __name__ == "__main__":
    unittest.main(verbosity=2)
