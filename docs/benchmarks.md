# Measured performance

## Environment and method

Measured on 2026-09-13 using **Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz**, 12 logical CPUs, 15.85 GiB usable RAM, **Windows-11-10.0.26200-SP0**, **MSVC 19.29.30157**, Release (/O2), and four server workers.

Server and native C++ benchmark client ran on the same Windows host over IPv4 loopback. Every row uses 10,000 total timed operations, 64-byte SET/GET values, a deterministic 1,000-key space, one outstanding command per client, and three trials. INCR uses one shared counter; its value is a decimal number rather than a 64-byte payload. MIXED alternates approximately 50% GET and 50% SET. Each trial starts a fresh server and disposable local temporary database. Server and client share CPU resources.

Connections complete a PING handshake and GET keys are preseeded before the timed phase. Latency includes request encoding, socket I/O, server work, and response parsing. It excludes connection establishment and seeding. This is a closed-loop test without coordinated-omission correction; it is not an open-loop saturation study. Short everysec trials can finish before a periodic sync; raw sync counters are retained. There is no CPU pinning, thermal control, remote network, or hardware power-failure experiment. Treat these as local measurements, not service guarantees.

All **1,080,000 timed operations across 108 trials** completed without reported errors. Values/types are validated rather than counting arbitrary replies as success. `baseline.json` is an earlier exploratory sweep (3,000 operations, one trial) and is not substituted for this final table.

## Results

Each row selects the **median-throughput trial** out of three. Its mean/p50/p95/p99 are from that same trial, so the table does not mix the best throughput and best latency from different runs. The throughput range shows all three trials.

| Policy | Workload | Clients | Ops/s | Trial range (ops/s) | Mean ms | p50 ms | p95 ms | p99 ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| disabled | SET | 1 | 10,242 | 10,099–10,860 | 0.096 | 0.083 | 0.155 | 0.226 |
| disabled | SET | 8 | 49,163 | 47,565–50,376 | 0.159 | 0.154 | 0.223 | 0.278 |
| disabled | SET | 32 | 47,729 | 44,303–49,092 | 0.595 | 0.555 | 1.156 | 1.387 |
| disabled | GET | 1 | 9,748 | 9,323–9,934 | 0.102 | 0.085 | 0.172 | 0.246 |
| disabled | GET | 8 | 48,268 | 47,583–48,342 | 0.162 | 0.158 | 0.223 | 0.275 |
| disabled | GET | 32 | 50,094 | 49,832–50,672 | 0.569 | 0.534 | 1.111 | 1.367 |
| disabled | MIXED | 1 | 9,901 | 9,783–9,927 | 0.100 | 0.084 | 0.167 | 0.246 |
| disabled | MIXED | 8 | 48,582 | 46,110–48,977 | 0.161 | 0.155 | 0.224 | 0.293 |
| disabled | MIXED | 32 | 49,151 | 48,563–49,919 | 0.581 | 0.543 | 1.121 | 1.371 |
| disabled | INCR | 1 | 9,873 | 8,798–10,497 | 0.100 | 0.084 | 0.170 | 0.238 |
| disabled | INCR | 8 | 45,465 | 43,818–49,122 | 0.172 | 0.166 | 0.241 | 0.308 |
| disabled | INCR | 32 | 46,282 | 42,973–46,735 | 0.622 | 0.580 | 1.205 | 1.488 |
| everysec | SET | 1 | 8,503 | 8,326–8,599 | 0.116 | 0.097 | 0.193 | 0.262 |
| everysec | SET | 8 | 26,687 | 26,485–27,399 | 0.297 | 0.279 | 0.487 | 0.646 |
| everysec | SET | 32 | 27,148 | 27,070–27,634 | 1.054 | 0.968 | 2.085 | 2.643 |
| everysec | GET | 1 | 9,063 | 6,912–9,760 | 0.110 | 0.099 | 0.156 | 0.226 |
| everysec | GET | 8 | 45,207 | 38,729–46,488 | 0.175 | 0.173 | 0.225 | 0.292 |
| everysec | GET | 32 | 50,076 | 49,639–50,421 | 0.564 | 0.529 | 1.093 | 1.289 |
| everysec | MIXED | 1 | 9,089 | 8,968–9,186 | 0.109 | 0.100 | 0.163 | 0.239 |
| everysec | MIXED | 8 | 36,050 | 34,398–37,163 | 0.218 | 0.206 | 0.343 | 0.454 |
| everysec | MIXED | 32 | 35,160 | 31,169–35,632 | 0.810 | 0.758 | 1.573 | 1.901 |
| everysec | INCR | 1 | 8,869 | 8,771–9,115 | 0.112 | 0.104 | 0.163 | 0.228 |
| everysec | INCR | 8 | 26,806 | 26,668–27,266 | 0.294 | 0.279 | 0.459 | 0.586 |
| everysec | INCR | 32 | 26,943 | 25,858–27,791 | 1.062 | 0.979 | 2.078 | 2.655 |
| always | SET | 1 | 1,070 | 1,065–1,149 | 0.933 | 0.922 | 1.273 | 1.506 |
| always | SET | 8 | 1,155 | 1,138–1,273 | 6.918 | 6.793 | 8.364 | 9.677 |
| always | SET | 32 | 1,108 | 1,105–1,223 | 26.108 | 25.193 | 51.769 | 56.270 |
| always | GET | 1 | 10,290 | 9,034–10,682 | 0.097 | 0.083 | 0.176 | 0.248 |
| always | GET | 8 | 41,296 | 37,344–49,030 | 0.189 | 0.182 | 0.279 | 0.401 |
| always | GET | 32 | 48,790 | 43,833–49,316 | 0.583 | 0.533 | 1.120 | 1.452 |
| always | MIXED | 1 | 1,811 | 1,811–1,839 | 0.550 | 0.601 | 1.223 | 1.483 |
| always | MIXED | 8 | 2,124 | 2,114–2,124 | 3.751 | 3.630 | 6.707 | 7.919 |
| always | MIXED | 32 | 2,128 | 2,120–2,130 | 13.547 | 13.047 | 26.896 | 31.352 |
| always | INCR | 1 | 1,026 | 1,006–1,038 | 0.973 | 0.941 | 1.374 | 1.695 |
| always | INCR | 8 | 1,209 | 1,128–1,216 | 6.604 | 6.487 | 8.279 | 9.879 |
| always | INCR | 32 | 1,230 | 1,189–1,231 | 23.328 | 22.190 | 46.196 | 52.668 |

## Longer periodic-sync workload

A separate follow-up run used 100,000 MIXED operations per trial, 32 clients, 64-byte values, everysec persistence, and three trials. Each trial spanned 4–5 actual background syncs. This adds 300,000 operations with zero errors to the matrix above, for **1,380,000 operations across 111 final trials**.

The median-throughput trial measured **22,836 ops/s**, mean **1.259 ms**, p50 **1.186 ms**, p95 **2.442 ms**, and p99 **2.807 ms**. Throughput ranged from 21,993 to 23,948 ops/s. These short multi-second runs still do not establish a long-duration capacity limit. They are more representative of periodic-sync behavior than a subsecond run with no sync event.

The 108-trial matrix was measured before final diagnostic/error-path-only fixes (physical AOF size while faulted and rejected-client benchmark accounting). Its successful command path is unchanged; the longer run includes those fixes. A later disconnect-counter cleanup correction does not alter command execution.

Raw data: [sustained.json](../benchmarks/results/sustained.json). Reproduce with:

```powershell
python scripts/benchmark.py --bin-dir build/Release --output benchmarks/local-sustained.json --compiler "your compiler" --requests 100000 --repeats 3 --clients 32 --commands MIXED --policies everysec
```

## Profiling and the measured optimization

The first parser rebuilt arguments from the beginning whenever a fragmented request was incomplete. A focused Release microbenchmark sends one 76,195-byte MSET frame in 64-byte fragments, repeated 100 times per trial. Retaining parse state, completed arguments, and the header scan position removed repeated work.

Three pre-change trials had a median **39.960 ms/frame**; three post-change trials had a median **0.163 ms/frame** (**244.4× faster in this specific microbenchmark**). This is not a claim of that speedup for ordinary SET/GET throughput. Reproduce the optimized workload with `forgedb-parser-benchmark`; raw before/after observations are in the results directory.

Engine timers separately accumulate mutex wait, lock-held execution, and AOF append/sync duration. They are wall-time instrumentation, not CPU sampling; wait totals sum across workers and may exceed elapsed wall time. Their intervals include the untimed seed phase and handshake commands, as the raw report notes.

For the selected `always` SET/32-client trial, accumulated execution was 9.399 s, AOF append/sync was 9.293 s (98.9% of execution), and mutex wait summed to 26.311 s. This directly exposes synchronous WAL I/O as the main write bottleneck. Increasing clients does not remove a serialized disk flush. Group commit is a high-value next design; it was not implemented or simulated by changing the default durability policy.

## Reproduce

```powershell
python scripts/benchmark.py --bin-dir build/Release --output benchmarks/local-run.json --compiler "MSVC 19.29.30157" --requests 10000 --repeats 3
.\build\Release\forgedb-parser-benchmark.exe
```

On Linux use the appropriate binary directory and compiler label. The benchmark overwrites `bench:*` keys during seeding; use the disposable sweep script or an instance reserved for measurements. The sweep saves JSON after every trial. `scripts/benchmark_report.py` derives this table from the checked-in raw results.

Raw evidence: [full sweep](../benchmarks/results/final.json), [exploratory sweep](../benchmarks/results/baseline.json), [parser before](../benchmarks/results/parser-before.json), [parser after](../benchmarks/results/parser-after.json).
