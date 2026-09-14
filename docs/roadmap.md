# ForgeDB implementation roadmap

1. Build a C++20 library, TCP server, RESP parser, CLI, and storage vertical slice.
2. Exercise fixed worker reactors and atomic counters with real concurrent clients.
3. Add indexed expiration, a checksummed mutation log, and deterministic recovery.
4. Add background snapshot/delta compaction and connection-local transactions.
5. Verify corruption, forced termination, resource limits, and shutdown behavior.
6. Measure Release workloads, inspect bottlenecks, run available sanitizers.
7. Document guarantees, limitations, measurements, and interview talking points.

Scope: string values, single-node database, IPv4; no embedded database, AI, replication,
clustering, or pub/sub. Correctness and explainable internals take priority.

Status: all seven implementation stages are complete and locally verified on
Windows with MSVC, Clang, and Clang ASan/UBSan. The scripted end-to-end demonstration
also passed. Linux/container/TSan validation remains environment-blocked; the
included CI configurations have not been executed on GitHub. See the engineering
report for evidence, benchmark measurements, and limits.
