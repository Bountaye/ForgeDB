"""End-to-end checks: real HTTP bridge, TCP clients, disk log, and forced restart."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from app import Board, Database, INDEX, PREFIX, make_http

BINARY = None


class DemoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="forgeboard-demo-test-")
        cls.database = Database(BINARY, cls.directory.name)
        cls.addClassCleanup(cls.directory.cleanup)
        cls.addClassCleanup(cls.database.stop)
        cls.database.start()
        cls.board = Board(cls.database)
        cls.board.seed()
        cls.http = make_http(cls.board, 0)
        cls.thread = threading.Thread(target=cls.http.serve_forever, daemon=True)
        cls.thread.start()
        cls.addClassCleanup(cls.close_http)
        cls.base = f"http://127.0.0.1:{cls.http.server_port}"

    @classmethod
    def close_http(cls):
        cls.http.shutdown()
        cls.http.server_close()
        cls.thread.join(timeout=10)

    def api(self, action=None, body=None, token=True):
        headers = {"Content-Type": "application/json"}
        if token:
            headers["X-Demo-Token"] = self.http.token
        req = Request(self.base + ("/api/" + action if action else "/api/state"),
                      data=json.dumps(body or {}).encode() if action else None,
                      headers=headers)
        with urlopen(req, timeout=30) as response:
            return json.load(response)

    def test_page_and_seed(self):
        with urlopen(self.base, timeout=5) as response:
            page = response.read().decode()
        self.assertIn("ForgeBoard", page)
        self.assertIn(self.http.token, page)
        self.assertNotIn("__TOKEN__", page)
        self.assertEqual(self.api()["info"]["fsync_policy"], "always")
        before = self.api()["tasks"]
        self.board.seed()
        self.assertEqual(before, self.api()["tasks"])

    def test_crud_unicode_and_atomic_revision(self):
        before = self.api()
        title = 'Ship café 🚀 <script>alert("hello")</script>'
        state = self.api("add", {"title": title})["state"]
        task = next(task for task in state["tasks"] if task["title"] == title)
        self.assertEqual(state["revision"], before["revision"] + 1)
        changed = self.api("toggle", {"id": task["id"]})["state"]
        self.assertTrue(next(t for t in changed["tasks"] if t["id"] == task["id"])["done"])
        self.api("delete", {"id": task["id"]})
        self.api("restart")
        with self.board.client() as client:
            self.assertIsNone(client.command("GET", PREFIX + task["id"]))
        self.assertNotIn(task["id"], [task["id"] for task in self.api()["tasks"]])

    def test_parallel_additions_keep_index(self):
        titles = [f"Parallel task {i}" for i in range(8)]
        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(lambda title: self.api("add", {"title": title}), titles))
        state = self.api()
        self.assertTrue(set(titles).issubset({task["title"] for task in state["tasks"]}))
        with self.board.client() as client:
            self.assertEqual(len(json.loads(client.command("GET", INDEX))), len(state["tasks"]))

    def test_concurrent_counter_compaction_and_recovery(self):
        before = self.api()
        traffic = self.api("traffic")
        self.assertEqual(traffic["state"]["counter"], before["counter"] + 200)
        compacted = self.api("compact")["state"]
        self.assertLess(int(compacted["info"]["aof_bytes"]), int(traffic["state"]["info"]["aof_bytes"]))
        recovered = self.api("restart")["state"]
        for field in ("tasks", "revision", "counter"):
            self.assertEqual(compacted[field], recovered[field])

    def test_presence_expiry_pin_and_clear(self):
        state = self.api("presence")["state"]
        self.assertIn(state["ttl"], range(0, 16))
        pinned = self.api("persist")["state"]
        self.assertEqual(pinned["ttl"], -1)
        self.assertEqual(self.api("restart")["state"]["ttl"], -1)
        self.assertEqual(self.api("clear")["state"]["ttl"], -2)
        self.api("presence")
        # Observe the actual 15-second application behavior, including an intervening crash.
        self.api("restart")
        deadline = time.monotonic() + 18
        while time.monotonic() < deadline:
            if self.api()["ttl"] == -2:
                break
            time.sleep(0.2)
        self.assertEqual(self.api()["ttl"], -2)
        self.assertIsNone(self.api()["presence"])

    def test_invalid_input_does_not_mutate(self):
        before = self.api()
        for action, body in (("add", {"title": " "}), ("add", {"title": "x" * 121}),
                             ("add", {"title": 42}), ("toggle", {"id": "missing"}),
                             ("delete", {"id": "missing"}), ("unknown", {})):
            with self.subTest(action=action, body=body):
                with self.assertRaises(HTTPError) as error:
                    self.api(action, body)
                self.assertEqual(error.exception.code, 400)
        after = self.api()
        self.assertEqual(before["tasks"], after["tasks"])
        self.assertEqual(before["revision"], after["revision"])

    def test_cross_origin_write_and_host_rejected(self):
        with self.assertRaises(HTTPError) as error:
            self.api("traffic", token=False)
        self.assertEqual(error.exception.code, 403)
        request = Request(self.base + "/api/state", headers={"Host": "untrusted.example"})
        with self.assertRaises(HTTPError) as error:
            urlopen(request, timeout=5)
        self.assertEqual(error.exception.code, 403)

    def test_relaunch_reads_existing_database(self):
        before = self.api()
        with self.board.lock:
            self.database.restart()
            # Same initialization path as a fresh invocation of app.py.
            self.board.seed()
        after = self.api()
        for field in ("tasks", "revision", "counter"):
            self.assertEqual(before[field], after[field])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    args = parser.parse_args()
    BINARY = args.server.resolve()
    unittest.main(argv=["test_demo.py"], verbosity=2)
