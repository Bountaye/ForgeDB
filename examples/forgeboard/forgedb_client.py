"""Small synchronous RESP2 client used by the demo; Python standard library only."""
import socket


class ForgeError(RuntimeError):
    """ForgeDB returned a command error."""


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.reader = self.socket.makefile("rb")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.reader.close()
        self.socket.close()

    def _read(self):
        line = self.reader.readline(65536)
        if not line.endswith(b"\r\n"):
            raise ConnectionError("Incomplete ForgeDB reply; write outcome may be uncertain")
        tag, payload = line[:1], line[1:-2]
        if tag == b"+":
            return payload.decode("utf-8")
        if tag == b"-":
            return ForgeError(payload.decode("utf-8", errors="replace"))
        if tag == b":":
            return int(payload)
        if tag == b"$":
            size = int(payload)
            if size == -1:
                return None
            if not 0 <= size <= 16 * 1024 * 1024:
                raise ConnectionError("Invalid bulk reply length")
            value = self.reader.read(size)
            if len(value) != size or self.reader.read(2) != b"\r\n":
                raise ConnectionError("Incomplete bulk reply")
            return value.decode("utf-8")
        if tag == b"*":
            size = int(payload)
            if not 0 <= size <= 4096:
                raise ConnectionError("Invalid array reply length")
            return [self._read() for _ in range(size)]
        raise ConnectionError("Unknown RESP reply type")

    def command(self, *args):
        parts = [str(arg).encode("utf-8") for arg in args]
        self.socket.sendall(b"*%d\r\n" % len(parts) + b"".join(
            b"$%d\r\n" % len(part) + part + b"\r\n" for part in parts))
        result = self._read()
        if isinstance(result, ForgeError):
            raise result
        return result

    def transaction(self, commands):
        self.command("MULTI")
        for command in commands:
            self.command(*command)
        result = self.command("EXEC")
        for reply in result:
            if isinstance(reply, ForgeError):
                # ForgeDB can commit other commands despite a runtime error.
                raise ForgeError(f"Transaction contained a runtime error: {reply}")
        return result

    def info(self):
        return dict(line.split(":", 1) for line in self.command("INFO").splitlines())
