# ForgeDB engineering report

Implementation and local verification: September 13, 2026.

## What was built

A C++20 persistent TCP string database, CMake build, interactive/one-shot C++ CLI,
C++ workload and parser benchmarks, deterministic C++ unit/stress tests, real-process
Python integration/recovery tests, Dockerfile, and GitHub Actions definitions.

Commands include PING, SET/GET/DEL/EXISTS/MGET/MSET, INCR/DECR/INCRBY,
EXPIRE/TTL/PERSIST, MULTI/EXEC/DISCARD, DBSIZE/INFO, and COMPACT. The server implements
its own storage, parser, WAL, replay, expiration, and compaction. It does not call
Redis or embed an existing database. Pub/sub, replication, clustering, and additional
value types were deliberately left outside scope.

## Architecture and concurrency

One nonblocking IPv4 acceptor distributes sockets to a fixed set of poll/WSAPoll
worker reactors. Each worker exclusively owns its connection parsers, send buffers,
and transaction sessions. TCP fragmentation/pipelining, short sends, disconnects,
admission limits, malformed peers, and slow consumers are handled explicitly.

The engine owns an unordered_map store, an ordered expiration index, and a commit
mutex. That mutex serializes evaluation, staged transaction overlays, log append,
and publication. INCR cannot lose updates, and MGET cannot observe half a transaction.
Network I/O and reply encoding occur outside the engine lock. Maintenance and an
optional compactor use separate threads, with documented snapshot/delta ownership.
The tradeoff is serialized execution and blocking disk flushes within the commit lock.

## Persistence and recovery

Versioned records contain a bounded length, header/payload CRC32 checksums, final
key mutations, and absolute expiration deadlines. Each transaction becomes one
record. Default `always` synchronizes before acknowledging a mutation; `everysec`
and `none` make explicit weaker durability choices. All policies sync on successful
graceful shutdown. Exclusive directory locking prevents cooperating concurrent writers.

Recovery replays verified records, reports/truncates an incomplete final record, and
refuses complete corruption without modifying the file. A write/sync failure faults
future mutations and does not publish the failed staged state; a failed client's
outcome can still be uncertain if a complete write reached disk before failure.

Compaction copies a live snapshot, writes it outside the engine mutex, captures
concurrent mutations in a bounded delta, then syncs and atomically replaces the log
during final handoff. A 32 MiB delta limit cancels excessive rewrites safely. Windows
and POSIX have separate file-handoff paths. See [persistence.md](persistence.md) for
record layout, ordering, and crash cases.

## Actual tests and analysis

| Configuration | Result | Final elapsed time |
|---|---|---:|
| Windows x64, MSVC 19.29.30157, Release, /W4 /WX | Unit + integration + recovery all passed | 56.26 s |
| Windows x64, Clang 23.1.1, Release, strong warnings / -Werror | Unit + integration + recovery all passed | 59.82 s |
| Windows x64, Clang 23.1.1, ASan + UBSan, RelWithDebInfo | Unit + integration + recovery all passed; no sanitizer findings | 65.76 s |
| clang-tidy 23, focused analyzer/bug checks | Command engine, AOF, parser, and worker server: no diagnostics | Completed |

Each tested build ran **13 TCP integration cases and 10 recovery cases**, plus the
C++ unit/property/stress suite. The latter includes 100 threads performing exactly
100,000 increments and deterministic fragmented/binary parser cases. The recovery
suite kills actual processes, tests every cut in a final transaction record, corrupts
complete records, verifies deletion/expiration across restart, and controls compaction
overlap with explicit checkpoints. A partial-I/O injection writes half an actual
record, proving that failed transaction state stays invisible and later writes fault.

Raw test logs are retained locally in the ignored validation directory because they
contain machine-specific paths. The table above summarizes those executed runs;
the existence of test files or CI configuration alone is not execution evidence.

A final worker disconnect-counter cleanup correction was rechecked with the full
MSVC suite above and the Clang ASan/UBSan integration suite (13 cases passed,
13.82 seconds total). Its additional sanitizer log is retained locally alongside
the preceding complete-suite sanitizer run.

The older installed MSVC ASan runtime failed before main with Windows status
`0xc0000142`, both inside and outside the sandbox. It was not called a passing check.
A SHA256-verified portable LLVM-MinGW toolchain was downloaded to ignored `.tools/`;
its ASan/UBSan runtime successfully executed the full suites. Windows LeakSanitizer
and ThreadSanitizer were not run. No sanitizer finding was suppressed.

Linux GCC/Clang, Linux TSan, and the Docker image could not be executed locally:
the installed Docker Desktop failed during its own startup and its engine API did
not become ready. Its diagnostic named an internal inference-manager socket error;
that is unrelated to ForgeDB, which has no AI subsystem. The attempted startup
processes were cleaned up. WSL exposed only Docker Desktop, not a usable Linux
development distribution. Linux/container/TSan CI jobs are supplied but hosted CI
has not been run or claimed to pass.

## Real benchmarks

Machine: Intel Core i7-9750H @ 2.60 GHz, 12 logical CPUs, 15.85 GiB usable RAM,
Windows 11 build 26200. Native MSVC Release binaries, four server workers, and the
benchmark client shared that host over loopback. The full matrix covers four workloads,
three client counts, three persistence settings, and three trials each. It completed
1,080,000 timed operations without errors.

A longer periodic-persistence run added three 100,000-operation MIXED
trials at 32 clients and 64-byte values, spanning 4–5 background syncs per trial.
Its median-throughput trial measured **22,836 ops/s**, **1.259 ms mean**, **1.186 ms
p50**, **2.442 ms p95**, and **2.807 ms p99**, with zero errors. Total final measured
work was **1,380,000 operations across 111 trials**, all with zero reported errors.

The stronger default durability costs throughput: the median-throughput 32-client
`always` SET trial measured **1,108 ops/s** and **56.270 ms p99**. These are different
workloads/policies and are not presented as interchangeable performance claims.

Profiling found repeated parser work under fragmentation. The same 76,195-byte
request split into 64-byte fragments improved from **39.960 ms/frame** to **0.163
ms/frame** in the median of three 100-frame Release trials, after retaining parser
state and completed arguments. This ~244x microbenchmark improvement does not imply
a comparable server-throughput improvement. WAL timers showed append/sync consuming
98.9% of engine execution time in the selected `always` SET/32-client profile.

The tests are closed-loop and do not correct coordinated omission. Short runs,
shared-host load, scheduling, and thermal behavior limit extrapolation. See the
[full methodology, table, and raw data](benchmarks.md).

## Important problems fixed

- **Windows file replacement:** live compaction initially failed with access denied
  while its destination handle remained open. Closing handles under the commit lock,
  atomically replacing the name, and reopening fixed the tested failure.
- **Fragmentation complexity:** restarting parse/argument construction on every read
  was measured and replaced with a resumable parser.
- **Slow-input spinning:** incomplete input is distinguished from runnable buffered
  commands so workers do not busy-poll a partially received request.
- **Compaction-test nondeterminism:** test-only checkpoints establish actual overlap,
  rather than assuming a sufficiently large snapshot lasts long enough.
- **Partial writes:** append failures fault later mutations, prevent failed staged
  publication, and recover through explicit torn-tail handling.
- **Benchmark error accounting:** a failed handshake discards the connection and counts
  its workload as errors; rejected clients cannot produce unsigned throughput underflow.
- **Physical metrics after faults:** INFO reports the actual AOF size, including a
  partial failed write, rather than only the previous acknowledged size.
- **Worker failure accounting:** disconnected clients are subtracted when removed
  from the owning vector, preventing double subtraction during exceptional cleanup.

The self-review covered ownership, lock order, short I/O, integer bounds, allocation
limits, expiry-index updates, commit ordering, recovery corruption behavior, compaction
handoff, transaction isolation, and shutdown. No unresolved race was discovered by
inspection/stress; that is not a substitute for the unexecuted Linux TSan run.

## Limitations and next improvements

No total-memory cap/eviction, authentication/TLS, replication, high availability,
or disk-backed read path. All live data plus a compaction snapshot must fit RAM.
The global commit lock, snapshot-copy pause, final delta/sync pause, O(connections)
polling, and fixed round-robin assignment limit scale. Compaction is explicit, not
automatically scheduled. Wall-clock adjustments affect TTL. CLI hosts are IPv4
literals. Transactions have documented runtime-error continuation, not rollback on
every error or WATCH. No physical power-cut testing or cross-filesystem certification
was performed. See README for deployment boundaries and shutdown guarantees.

Highest-value next work: first obtain Linux/TSan and container validation; then add
group commit with explicit acknowledgment ordering, process-wide memory limits,
event-based worker wakeups, and a snapshot strategy with shorter lock-held copying.
Expand disk-failure injection and long-duration, remote-client benchmarks before
introducing replication or more data types.

## Demonstration and further reading

Run `python scripts/demo.py --bin-dir build/Release` on Windows, or use `--bin-dir build`
on Linux, for the checked two-client, expiration, crash/restart, transaction, and
compaction demonstration. The [architecture](architecture.md), [concurrency](concurrency.md),
and [persistence](persistence.md) documents explain the design and its tradeoffs.
