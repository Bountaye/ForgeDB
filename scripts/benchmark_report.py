"""Render the checked-in measurements without inventing or combining trial metrics."""
import collections
import json
from pathlib import Path
import statistics

root = Path(__file__).resolve().parents[1]
data = json.loads((root / "benchmarks/results/final.json").read_text())
groups = collections.defaultdict(list)
for result in data["results"]:
    groups[result["policy"], result["command"], result["clients"]].append(result)
assert len(data["results"]) == 108 and all(len(v) == 3 for v in groups.values())
assert all(r["errors"] == 0 for r in data["results"])
environment = data["environment"]
lines = ["# Measured performance", "", "## Environment and method", "",
    f"Measured on {environment['timestamp_utc'][:10]} using **{environment['cpu']}**, "
    f"{environment['logical_cpus']} logical CPUs, {environment['memory_bytes'] / 2**30:.2f} GiB "
    f"usable RAM, **{environment['os']}**, **{data['compiler']}**, Release (/O2), and four server workers.", "",
    "Server and native C++ benchmark client ran on the same Windows host over IPv4 loopback. "
    "Every row uses 10,000 total timed operations, 64-byte SET/GET values, a deterministic "
    "1,000-key space, one outstanding command per client, and three trials. INCR uses "
    "one shared counter; its value is a decimal number rather than a 64-byte payload. "
    "MIXED alternates approximately 50% GET and 50% SET. Each trial starts a fresh "
    "server and disposable local temporary database. Server and client share CPU resources.", "",
    "Connections complete a PING handshake and GET keys are preseeded before the timed "
    "phase. Latency includes request encoding, socket I/O, server work, and response "
    "parsing. It excludes connection establishment and seeding. This is a closed-loop "
    "test without coordinated-omission correction; it is not an open-loop saturation "
    "study. Short everysec trials can finish before a periodic sync; raw sync counters "
    "are retained. There is no CPU pinning, thermal control, remote network, or hardware "
    "power-failure experiment. Treat these as local measurements, not service guarantees.", "",
    "All **1,080,000 timed operations across 108 trials** completed without reported "
    "errors. Values/types are validated rather than counting arbitrary replies as "
    "success. `baseline.json` is an earlier exploratory sweep (3,000 operations, one "
    "trial) and is not substituted for this final table.", "",
    "## Results", "",
    "Each row selects the **median-throughput trial** out of three. Its mean/p50/p95/p99 "
    "are from that same trial, so the table does not mix the best throughput and best "
    "latency from different runs. The throughput range shows all three trials.", "",
    "| Policy | Workload | Clients | Ops/s | Trial range (ops/s) | Mean ms | p50 ms | p95 ms | p99 ms |",
    "|---|---|---:|---:|---:|---:|---:|---:|---:|"]
selected = {}
for policy in ["disabled", "everysec", "always"]:
    for command in ["SET", "GET", "MIXED", "INCR"]:
        for clients in [1, 8, 32]:
            trials = sorted(groups[policy, command, clients], key=lambda r: r["ops_per_second"])
            row = trials[1]
            selected[policy, command, clients] = row
            lines.append(f"| {policy} | {command} | {clients} | {row['ops_per_second']:,.0f} | {trials[0]['ops_per_second']:,.0f}–{trials[-1]['ops_per_second']:,.0f} | {row['mean_ms']:.3f} | {row['p50_ms']:.3f} | {row['p95_ms']:.3f} | {row['p99_ms']:.3f} |")
before = json.loads((root / "benchmarks/results/parser-before.json").read_text())
after = json.loads((root / "benchmarks/results/parser-after.json").read_text())
b = statistics.median(r["ms_per_frame"] for r in before)
a = statistics.median(r["ms_per_frame"] for r in after)
row = selected["always", "SET", 32]
p = row["profile"]
long_data = json.loads((root / "benchmarks/results/sustained.json").read_text())
long_trials = sorted(long_data["results"], key=lambda r: r["ops_per_second"])
assert len(long_trials) == 3 and all(r["requests"] == 100000 and r["errors"] == 0 and r["profile"]["aof_syncs"] >= 3 for r in long_trials)
long_row = long_trials[1]
lines += ["", "## Longer periodic-sync workload", "",
    "A separate follow-up run used 100,000 MIXED operations per trial, 32 clients, "
    "64-byte values, everysec persistence, and three trials. Each trial spanned "
    "4–5 actual background syncs. This adds 300,000 operations with zero errors "
    "to the matrix above, for **1,380,000 operations across 111 final trials**.", "",
    f"The median-throughput trial measured **{long_row['ops_per_second']:,.0f} ops/s**, "
    f"mean **{long_row['mean_ms']:.3f} ms**, p50 **{long_row['p50_ms']:.3f} ms**, "
    f"p95 **{long_row['p95_ms']:.3f} ms**, and p99 **{long_row['p99_ms']:.3f} ms**. "
    f"Throughput ranged from {long_trials[0]['ops_per_second']:,.0f} to "
    f"{long_trials[-1]['ops_per_second']:,.0f} ops/s. These short multi-second runs "
    "still do not establish a long-duration capacity limit. They are more representative "
    "of periodic-sync behavior than a subsecond run with no sync event.", "",
    "The 108-trial matrix was measured before final diagnostic/error-path-only fixes "
    "(physical AOF size while faulted and rejected-client benchmark accounting). "
    "Its successful command path is unchanged; the longer run includes those fixes. "
    "A later disconnect-counter cleanup correction does not alter command execution.", "",
    "Raw data: [sustained.json](../benchmarks/results/sustained.json). Reproduce with:", "", "```powershell",
    'python scripts/benchmark.py --bin-dir build/Release --output benchmarks/local-sustained.json --compiler "your compiler" --requests 100000 --repeats 3 --clients 32 --commands MIXED --policies everysec',
    "```"]
lines += ["", "## Profiling and the measured optimization", "",
    "The first parser rebuilt arguments from the beginning whenever a fragmented "
    "request was incomplete. A focused Release microbenchmark sends one 76,195-byte "
    "MSET frame in 64-byte fragments, repeated 100 times per trial. Retaining parse "
    "state, completed arguments, and the header scan position removed repeated work.", "",
    f"Three pre-change trials had a median **{b:.3f} ms/frame**; three post-change "
    f"trials had a median **{a:.3f} ms/frame** (**{b/a:.1f}× faster in this specific "
    "microbenchmark**). This is not a claim of that speedup for ordinary SET/GET "
    "throughput. Reproduce the optimized workload with `forgedb-parser-benchmark`; "
    "raw before/after observations are in the results directory.", "",
    "Engine timers separately accumulate mutex wait, lock-held execution, and AOF "
    "append/sync duration. They are wall-time instrumentation, not CPU sampling; "
    "wait totals sum across workers and may exceed elapsed wall time. Their intervals "
    "include the untimed seed phase and handshake commands, as the raw report notes.", "",
    f"For the selected `always` SET/32-client trial, accumulated execution was "
    f"{p['execution_ns']/1e9:.3f} s, AOF append/sync was {p['aof_append_ns']/1e9:.3f} s "
    f"({100*p['aof_append_ns']/p['execution_ns']:.1f}% of execution), and mutex wait "
    f"summed to {p['lock_wait_ns']/1e9:.3f} s. This directly exposes synchronous WAL "
    "I/O as the main write bottleneck. Increasing clients does not remove a serialized "
    "disk flush. Group commit is a high-value next design; it was not implemented or "
    "simulated by changing the default durability policy.", "",
    "## Reproduce", "", "```powershell",
    'python scripts/benchmark.py --bin-dir build/Release --output benchmarks/local-run.json --compiler "MSVC 19.29.30157" --requests 10000 --repeats 3',
    '.\\build\\Release\\forgedb-parser-benchmark.exe', "```", "",
    "On Linux use the appropriate binary directory and compiler label. The benchmark "
    "overwrites `bench:*` keys during seeding; use the disposable sweep script or an "
    "instance reserved for measurements. The sweep saves JSON after every trial. "
    "`scripts/benchmark_report.py` derives this table from the checked-in raw results.", "",
    "Raw evidence: [full sweep](../benchmarks/results/final.json), "
    "[exploratory sweep](../benchmarks/results/baseline.json), "
    "[parser before](../benchmarks/results/parser-before.json), "
    "[parser after](../benchmarks/results/parser-after.json).", ""]
(root / "docs/benchmarks.md").write_text("\n".join(lines), encoding="utf-8")
for key in [("everysec", "MIXED", 32), ("always", "SET", 32), ("disabled", "GET", 32)]:
    print(key, json.dumps(selected[key]))
print("parser_speedup", b/a)
