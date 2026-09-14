"""Run the interview demonstration with real TCP clients and a disposable AOF."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from support import Client, Server, eventually


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", default="build")
    args = parser.parse_args()
    executable = Path(args.bin_dir) / ("forgedb-server.exe" if os.name == "nt" else "forgedb-server")
    with Server(executable) as server:
        print(f"Started a real server on 127.0.0.1:{server.port}; fsync=always", flush=True)
        with Client(server.port) as a, Client(server.port) as b:
            assert a.command("SET", "name", "Nishant") == "OK"
            print("Client A: SET name Nishant -> OK", flush=True)
            assert b.command("GET", "name") == b"Nishant"
            print("Client B: GET name -> Nishant", flush=True)
            a.command("SET", "temporary", "expires")
            a.command("EXPIRE", "temporary", "1")
            eventually(lambda: b.command("GET", "temporary") is None, timeout=3)
            print("TTL expiration observed by independent client", flush=True)
        def increments(_):
            with Client(server.port) as c:
                for _ in range(100):
                    assert isinstance(c.command("INCR", "counter"), int)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(increments, range(8)))
        with Client(server.port) as c:
            assert c.command("GET", "counter") == b"800"
            print("8 concurrent clients × 100 INCR -> 800", flush=True)
            c.command("SET", "deleted", "never resurrect")
            c.command("DEL", "deleted")
        server.kill()
        print("Forcefully terminated server; restarting from the AOF", flush=True)
        server.restart()
        with Client(server.port) as c:
            assert c.command("MGET", "name", "counter", "temporary", "deleted") == [b"Nishant", b"800", None, None]
            print("Recovered values, counter, expiration, and deletion correctly", flush=True)
            result = c.pipeline([("MULTI",), ("SET", "a", "1"), ("INCR", "counter"), ("EXEC",)])[-1]
            assert result == ["OK", 801]
            print("Atomic transaction -> [OK, 801]", flush=True)
            before = int(c.info()["aof_bytes"])
            assert c.command("COMPACT") == "Background compaction started"
            for i in range(100):
                assert c.command("SET", "during-rewrite", str(i)) == "OK"
            eventually(lambda: c.info()["compacting"] == "0")
            assert c.info()["compactions"] == "1", c.info()
            after = int(c.info()["aof_bytes"])
            print(f"Compacted AOF: {before} -> {after} bytes (includes new writes)", flush=True)
        server.restart()
        with Client(server.port) as c:
            assert c.command("MGET", "a", "counter", "during-rewrite") == [b"1", b"801", b"99"]
        print("Second crash/restart verified compacted state and writes around compaction", flush=True)
    print("Demonstration passed; processes and temporary database cleaned up", flush=True)


if __name__ == "__main__":
    main()
