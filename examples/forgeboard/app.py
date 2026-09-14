"""ForgeBoard: a local task dashboard backed by a real ForgeDB server."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from uuid import uuid4

from forgedb_client import Client, ForgeError

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PREFIX = "forgeboard:"
INDEX = PREFIX + "tasks"
REVISION = PREFIX + "revision"
COUNTER = PREFIX + "visits"
PRESENCE = PREFIX + "presence"


def wait_for(predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.03)
    raise TimeoutError("ForgeDB operation did not finish within 15 seconds")


class Database:
    """Own only the child process we start, with a dedicated persistent directory."""
    def __init__(self, binary, directory):
        self.binary = Path(binary).resolve()
        self.directory = Path(directory).resolve()
        self.process = None
        self.log = None
        self.port = 0

    def start(self):
        self.directory.mkdir(parents=True, exist_ok=True)
        with socket.socket() as available:
            available.bind(("127.0.0.1", 0))
            self.port = available.getsockname()[1]
        self.log = (self.directory / "server.log").open("ab")
        self.process = subprocess.Popen(
            [str(self.binary), "--host", "127.0.0.1", "--port", str(self.port),
             "--data-dir", str(self.directory), "--fsync-policy", "always"],
            stdout=self.log, stderr=self.log,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        def ready():
            if self.process.poll() is not None:
                raise RuntimeError(f"ForgeDB stopped during startup. See {self.directory / 'server.log'}")
            try:
                with Client(self.port) as client:
                    return client.command("PING") == "PONG"
            except (OSError, ConnectionError):
                return False
        wait_for(ready)

    def stop(self, crash=False):
        if self.process is not None and self.process.poll() is None:
            if crash:
                self.process.kill()
            else:
                self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        if self.log is not None:
            self.log.close()
            self.log = None

    def restart(self):
        self.stop(crash=True)
        self.start()


class Board:
    def __init__(self, database):
        self.database = database
        # Protect the demo's read/modify/write JSON index. ForgeDB has no WATCH.
        # This is one application instance; DB transactions publish related keys atomically.
        self.lock = threading.RLock()

    def client(self):
        return Client(self.database.port)

    def seed(self):
        with self.lock, self.client() as client:
            if client.command("EXISTS", INDEX):
                return
            tasks = [
                {"id": uuid4().hex, "title": "Ship a feature backed by ForgeDB", "done": False},
                {"id": uuid4().hex, "title": "Try a crash and recover this board", "done": False},
                {"id": uuid4().hex, "title": "Connect over real TCP", "done": True},
            ]
            pairs = [INDEX, json.dumps([task["id"] for task in tasks]),
                     REVISION, "1", COUNTER, "0"]
            for task in tasks:
                pairs.extend([PREFIX + task["id"], json.dumps(task)])
            client.command("MSET", *pairs)

    def state(self):
        with self.lock, self.client() as client:
            ids = json.loads(client.command("GET", INDEX))
            values = client.command("MGET", *[PREFIX + task_id for task_id in ids]) if ids else []
            tasks = [json.loads(value) for value in values if value is not None]
            revision, counter, presence = client.command("MGET", REVISION, COUNTER, PRESENCE)
            return {"tasks": tasks, "revision": int(revision), "counter": int(counter),
                    "presence": presence, "ttl": client.command("TTL", PRESENCE),
                    "info": client.info(), "database_port": self.database.port}

    def action(self, name, body):
        with self.lock:
            return self._action(name, body)

    def _action(self, name, body):
        commands = []
        message = ""
        if name in ("add", "toggle", "delete"):
            with self.client() as client:
                ids = json.loads(client.command("GET", INDEX))
                if name == "add":
                    title = body.get("title")
                    if not isinstance(title, str) or not 1 <= len(title.strip()) <= 120:
                        raise ValueError("Use a task title between 1 and 120 characters.")
                    if len(ids) >= 100:
                        raise ValueError("This demo supports up to 100 tasks. Delete a task first.")
                    task = {"id": uuid4().hex, "title": title.strip(), "done": False}
                    ids.append(task["id"])
                    batch = [("SET", PREFIX + task["id"], json.dumps(task)),
                             ("SET", INDEX, json.dumps(ids)), ("INCR", REVISION)]
                    message = "Task and board index saved in one atomic transaction."
                else:
                    task_id = body.get("id")
                    if task_id not in ids:
                        raise ValueError("This task no longer exists. Refresh the board.")
                    key = PREFIX + task_id
                    if name == "toggle":
                        task = json.loads(client.command("GET", key))
                        task["done"] = not task["done"]
                        batch = [("SET", key, json.dumps(task)), ("INCR", REVISION)]
                        message = "Task status and board revision committed together."
                    else:
                        ids.remove(task_id)
                        batch = [("DEL", key), ("SET", INDEX, json.dumps(ids)), ("INCR", REVISION)]
                        message = "Task deleted; its deletion is persisted in the append-only log."
                client.transaction(batch)
                commands = ["MULTI"] + [" ".join(map(str, item)) for item in batch] + ["EXEC"]
        elif name == "presence":
            with self.client() as client:
                batch = [("SET", PRESENCE, "You are here"), ("EXPIRE", PRESENCE, 15)]
                client.transaction(batch)
            commands = ["MULTI", f"SET {PRESENCE} \"You are here\"", f"EXPIRE {PRESENCE} 15", "EXEC"]
            message = "Presence expires in 15 seconds. Watch ForgeDB remove it automatically."
        elif name == "persist":
            with self.client() as client:
                changed = client.command("PERSIST", PRESENCE)
            commands = [f"PERSIST {PRESENCE}"]
            message = "Expiration removed; presence now survives until cleared." if changed else "No expiring presence to pin."
        elif name == "clear":
            with self.client() as client:
                client.command("DEL", PRESENCE)
            commands = [f"DEL {PRESENCE}"]
            message = "Presence cleared."
        elif name == "traffic":
            with self.client() as client:
                before = int(client.command("GET", COUNTER))
            def visitor(_):
                with self.client() as client:
                    return [client.command("INCR", COUNTER) for _ in range(25)]
            started = time.monotonic()
            with ThreadPoolExecutor(max_workers=8) as pool:
                replies = [value for batch in pool.map(visitor, range(8)) for value in batch]
            with self.client() as client:
                after = int(client.command("GET", COUNTER))
            if after != before + 200 or len(set(replies)) != 200:
                raise RuntimeError("Counter verification failed")
            commands = [f"INCR {COUNTER}  [25 times on each of 8 independent TCP clients]",
                        f"GET {COUNTER} -> {after}"]
            message = f"200 increments verified, zero lost updates ({time.monotonic() - started:.2f}s). This is a demo, not a benchmark."
        elif name == "restart":
            with self.client() as client:
                ids = json.loads(client.command("GET", INDEX))
                keys = [INDEX, REVISION, COUNTER] + [PREFIX + task_id for task_id in ids]
                before = client.command("MGET", *keys)
            self.database.restart()
            with self.client() as client:
                after = client.command("MGET", *keys)
            if before != after:
                raise RuntimeError("Recovery verification failed: board state changed")
            commands = ["MGET board keys", "Force-kill owned ForgeDB process", "Restart using the same AOF", "MGET board keys -> identical"]
            message = "Crash recovery verified: every task, board revision, and counter survived."
        elif name == "compact":
            with self.client() as client:
                before = client.info()
                client.command("COMPACT")
                def complete():
                    info = client.info()
                    if info["background_error"]:
                        raise RuntimeError(info["background_error"])
                    return info if info["compacting"] == "0" else None
                after = wait_for(complete)
                if int(after["compactions"]) != int(before["compactions"]) + 1:
                    raise RuntimeError("Compaction did not complete")
            commands = ["INFO", "COMPACT", "INFO -> compacting:0"]
            message = f"AOF compacted: {int(before['aof_bytes']):,} -> {int(after['aof_bytes']):,} bytes. Live state preserved."
        else:
            raise ValueError("Unknown demo action.")
        return {"state": self.state(), "event": {"message": message, "commands": commands}}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, status, body, content_type="application/json"):
        raw = json.dumps(body).encode() if content_type == "application/json" else body
        self.send_response(status)
        self.send_header("Content-Type", content_type + "; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(raw)

    def allowed_host(self):
        return self.headers.get("Host") in (
            f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}")

    def do_GET(self):
        if not self.allowed_host():
            self.send(403, {"error": "Open this demo through localhost."})
            return
        try:
            if self.path == "/":
                html = (HERE / "index.html").read_text(encoding="utf-8").replace("__TOKEN__", self.server.token)
                self.send(200, html.encode(), "text/html")
            elif self.path == "/api/state":
                self.send(200, self.server.board.state())
            else:
                self.send(404, {"error": "Not found"})
        except (OSError, RuntimeError, ValueError) as exc:
            self.send(503, {"error": str(exc)})

    def do_POST(self):
        if not self.allowed_host() or self.headers.get("X-Demo-Token") != self.server.token:
            self.send(403, {"error": "Reload the demo page before making changes."})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 4096:
                raise ValueError("Request must contain at most 4096 bytes of JSON.")
            if self.headers.get_content_type() != "application/json":
                raise ValueError("Content-Type must be application/json.")
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ValueError("Expected a JSON object.")
            if not self.path.startswith("/api/"):
                self.send(404, {"error": "Not found"})
                return
            result = self.server.board.action(self.path[5:], body)
            self.send(200, result)
        except ValueError as exc:
            self.send(400, {"error": str(exc)})
        except (OSError, RuntimeError) as exc:
            self.send(503, {"error": f"{exc}. A disconnected write can have an uncertain outcome; refresh before retrying."})


def make_http(board, port):
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.board = board
    server.token = secrets.token_hex(24)
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, help="Directory containing forgedb-server")
    parser.add_argument("--data-dir", type=Path, default=HERE / ".demo-data",
                        help="Dedicated demo data directory (retained between launches)")
    parser.add_argument("--port", type=int, default=8080, help="Local web port (default: 8080)")
    args = parser.parse_args()
    executable = "forgedb-server.exe" if os.name == "nt" else "forgedb-server"
    candidates = [args.bin_dir / executable] if args.bin_dir else [
        ROOT / "build" / "Release" / executable, ROOT / "build" / executable]
    binary = next((path for path in candidates if path.is_file()), None)
    if binary is None:
        parser.error("Build ForgeDB first with 'python scripts/build.py', or supply --bin-dir.")
    if not 0 <= args.port <= 65535:
        parser.error("--port must be between 0 and 65535")
    database = Database(binary, args.data_dir)
    http = None
    try:
        database.start()
        board = Board(database)
        board.seed()
        http = make_http(board, args.port)
        print(f"ForgeBoard: http://127.0.0.1:{http.server_port}", flush=True)
        print(f"ForgeDB TCP: 127.0.0.1:{database.port} | fsync=always", flush=True)
        print(f"Demo data: {database.directory}\nPress Ctrl+C to stop. Your board is saved.", flush=True)
        http.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        if http is not None:
            http.server_close()
        database.stop()


if __name__ == "__main__":
    main()
