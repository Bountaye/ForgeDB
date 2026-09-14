# Tests, diagnostics, and reproducibility

CTest groups the work into unit, integration, and recovery suites. C++ unit checks
throw on failure in all build modes; they do not use `assert` that disappears under
NDEBUG. Python unittest fixtures launch built executables, communicate over real
IPv4 TCP, and own disposable data directories. Startup readiness uses PING and a
deadline, not a fixed startup sleep. All fixtures terminate/reap their processes.

## Coverage

| Suite | Meaningful assertions |
|---|---|
| C++ unit | Every two-part split of a binary frame; one-byte fragments; deterministic randomized binary argument/chunk cases; pipelines; malformed headers/terminators/lengths; cumulative frame caps |
| C++ unit | Exact TTL boundaries with an injected clock; index replacement/removal; validation; missing keys; numeric bounds; queue/discard/error semantics |
| C++ unit | 100 threads × 1000 increments; CRC standard vector; binary codec round trip; replay; torn tail; corruption rejection; directory lock; bounded-delta cancellation and retry |
| TCP integration | Two independent clients; separate CLI processes; binary byte fragments; 5000-request pipeline and half-close; malformed/oversized peers; 32-client increments; concurrent paired-key transactions/readers |
| TCP integration | Expiration with concurrent readers; 128 idle clients; slow consumer; oversized MGET results; disconnect/discard; random bad peers; admission limit and capacity reuse; graceful flush/restart |
| Recovery | Kill/restart acknowledged values; deleted and expired state; persisted remaining TTL; actual periodic sync; every truncation byte in the final transaction; complete header/body corruption; valid-CRC malformed payload |
| Recovery | Concurrent writes during an explicitly paused rewrite; two successive rewrites; kill during partially written snapshot; abandoned candidate; exclusive lock; injected partial disk write with transaction non-publication and write-fault behavior |

The additional `forgedb-test-server` target uses the same engine and OS I/O, with
test-only CLI switches compiled into its main. Production `forgedb-server` rejects
these switches. Its compaction checkpoint creates `entered` after writing the first
snapshot record and waits for `release`. Tests can then prove that writes occur
within the capture interval or kill a rewrite while it is incomplete. The wait has
a 30-second failure deadline. It is not a sleep used to hide a database race.

The I/O hook writes half a real record and synchronizes it before throwing an
injected disk-full error. It tests uncertain I/O and fail-closed behavior without
filling the user's disk. Separate tests perform unmodified process termination and
byte corruption/truncation. Hardware power loss and arbitrary kernel I/O failures
are not simulated by these tests.

## Commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure

# Individual suites
ctest --test-dir build -R unit --output-on-failure
ctest --test-dir build -R integration --output-on-failure
ctest --test-dir build -R recovery --output-on-failure
```

On a multi-config Windows build, add `--config Release` to build and `-C Release`
to CTest. Keep CTest's failure logs in `build/Testing/Temporary/`. For an AddressSanitizer
MSVC build, use `-DFORGEDB_SANITIZER=address`, a Release/RelWithDebInfo configuration,
and a developer environment with the matching sanitizer runtime DLL on PATH. Do not
mix ASan and TSan. Linux's `address,undefined` and `thread` configurations are shown
in the README and CI.

For clang-tidy, use a Ninja/Makefiles compilation database:

```bash
cmake -S . -B build-tidy -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-tidy
clang-tidy -p build-tidy src/commands/engine.cpp src/persistence/aof.cpp \
  src/protocol/resp.cpp src/server/server.cpp
```

The focused checked-in tidy configuration includes analyzer diagnostics and
use-after-move checks. Warnings are not blanket-suppressed to make a report green.

## Interpreting the execution record

See [engineering-report.md](engineering-report.md) for actual compilers, suite
results, sanitizer runs, and environment blockers. A supplied CI job is not a
completed hosted CI run. A process-kill recovery pass is not a proof against a
lying disk cache. A successful stress test does not replace a concurrency argument
or a race detector. These distinctions matter when describing the project in an
interview.
