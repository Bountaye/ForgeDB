"""Real TCP/process fixtures; no database implementation is shared with tests."""
import contextlib
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


class Error(str):
    pass


def frame(*args):
    parts = [x if isinstance(x, bytes) else str(x).encode() for x in args]
    return b"*%d\r\n" % len(parts) + b"".join(b"$%d\r\n" % len(p) + p + b"\r\n" for p in parts)


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.socket.makefile("rb")

    def read(self):
        line = self.file.readline()
        if not line or not line.endswith(b"\r\n"):
            raise EOFError("incomplete response")
        tag, value = line[:1], line[1:-2]
        if tag == b"+":
            return value.decode()
        if tag == b"-":
            return Error(value.decode())
        if tag == b":":
            return int(value)
        if tag == b"*":
            return [self.read() for _ in range(int(value))]
        if tag == b"$":
            n = int(value)
            if n == -1:
                return None
            data = self.file.read(n)
            assert len(data) == n and self.file.read(2) == b"\r\n"
            return data
        raise AssertionError(f"unknown response: {line!r}")

    def command(self, *args):
        self.socket.sendall(frame(*args))
        return self.read()

    def pipeline(self, commands):
        self.socket.sendall(b"".join(frame(*c) for c in commands))
        return [self.read() for _ in commands]

    def info(self):
        return dict(line.split(":", 1) for line in self.command("INFO").decode().splitlines())

    def close(self):
        self.file.close()
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def eventually(predicate, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)  # Poll a state/deadline; never used to serialize racing writes.
    raise AssertionError("condition did not become true before deadline")


class Server:
    def __init__(self, executable, directory=None, options=()):
        self.executable = str(Path(executable).resolve())
        self.temp = tempfile.TemporaryDirectory(prefix="forgedb-test-") if directory is None else None
        self.directory = Path(self.temp.name if self.temp else directory)
        self.options = list(options)
        self.process = None
        self.log = None
        self.port = 0

    def start(self):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.log = tempfile.TemporaryFile(mode="w+b")
        self.process = subprocess.Popen(
            [self.executable, "--port", str(self.port), "--data-dir", str(self.directory), *self.options],
            stdout=self.log, stderr=self.log,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        def ready():
            if self.process.poll() is not None:
                raise RuntimeError(self.output())
            try:
                with Client(self.port) as client:
                    return client.command("PING") == "PONG"
            except (OSError, EOFError):
                return False
        eventually(ready)
        return self

    def output(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read().decode(errors="replace")

    def kill(self):
        if self.process and self.process.poll() is None:
            self.process.kill()
        if self.process:
            self.process.wait(timeout=15)

    def restart(self):
        self.kill()
        self.log.close()
        return self.start()

    def graceful(self):
        if os.name != "nt":
            self.process.send_signal(signal.SIGTERM)
        # Windows fixture uses --run-for-ms because a headless child has no console.
        self.process.wait(timeout=15)
        assert self.process.returncode == 0, self.output()
        assert "Shutdown complete" in self.output(), self.output()

    def close(self):
        self.kill()
        if self.log:
            output = self.output()
            self.log.close()
            if "ERROR: AddressSanitizer" in output or "runtime error:" in output or "WARNING: ThreadSanitizer" in output:
                raise AssertionError(output)
        if self.temp:
            self.temp.cleanup()

    def __enter__(self):
        return self.start()

    def __exit__(self, *args):
        self.close()


@contextlib.contextmanager
def rejected_start(executable, directory):
    result = subprocess.run([executable, "--data-dir", str(directory)], capture_output=True, timeout=15)
    assert result.returncode != 0, result.stdout
    yield result.stderr.decode(errors="replace")
