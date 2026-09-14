"""Reproducible local benchmark sweep. Uses a disposable database directory."""
import argparse
import ctypes
import datetime
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from support import Client, Server


def environment():
    cpu = platform.processor()
    memory = None
    if os.name == "nt":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            cpu = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
        class Memory(ctypes.Structure):
            _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [(name, ctypes.c_ulonglong) for name in ["total_phys", "available_phys", "total_page", "available_page", "total_virtual", "available_virtual", "extended"]]
        status = Memory()
        status.length = ctypes.sizeof(status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            memory = status.total_phys
    elif Path("/proc/meminfo").exists():
        memory = int(Path("/proc/meminfo").read_text().splitlines()[0].split()[1]) * 1024
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    return {"cpu": cpu, "logical_cpus": os.cpu_count(), "memory_bytes": memory, "os": platform.platform(), "python": platform.python_version(), "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clients", default="1,8,32")
    parser.add_argument("--commands", default="SET,GET,MIXED,INCR")
    parser.add_argument("--policies", default="disabled,everysec,always")
    parser.add_argument("--requests", type=int, default=10000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--value-size", type=int, default=64)
    parser.add_argument("--keyspace", type=int, default=1000)
    args = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    binary = Path(args.bin_dir).resolve()
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    report = {"environment": environment(), "compiler": args.compiler, "build_mode": "Release", "worker_threads": 4, "transport": "IPv4 loopback, one outstanding command per client", "latency": "closed-loop end-to-end including encoding and response parsing; excludes connection and seed; no coordinated-omission correction", "results": []}
    for policy in args.policies.split(","):
        options = ["--no-persistence"] if policy == "disabled" else ["--fsync-policy", policy]
        for command in args.commands.split(","):
            for clients in map(int, args.clients.split(",")):
                for repeat in range(args.repeats):
                    with Server(binary / ("forgedb-server" + suffix), options=options) as server:
                        with Client(server.port) as probe:
                            initial = probe.info()
                            process = subprocess.run([str(binary / ("forgedb-benchmark" + suffix)), "--port", str(server.port), "--command", command, "--clients", str(clients), "--requests", str(args.requests), "--value-size", str(args.value_size), "--keyspace", str(args.keyspace)], capture_output=True, text=True, timeout=180, check=True)
                            measured = json.loads(process.stdout)
                            final = probe.info()
                            measured.update(policy=policy, repeat=repeat + 1, profile={key: int(final[key]) - int(initial[key]) for key in ["lock_wait_ns", "execution_ns", "aof_append_ns", "total_commands", "aof_syncs"]})
                            report["results"].append(measured)
                            out.write_text(json.dumps(report, indent=2) + "\n")
                            print(f"{policy:9} {command:5} c={clients:3} trial={repeat + 1}: {measured['ops_per_second']:.0f} ops/s, p99 {measured['p99_ms']:.3f} ms, errors {measured['errors']}", flush=True)


if __name__ == "__main__":
    main()
