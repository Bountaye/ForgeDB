# Concurrency contract

## Thread ownership

| State | Owner / synchronization |
|---|---|
| Listener and round-robin index | Main acceptor thread |
| Each client socket/parser/output/session | One worker only |
| Pending accepted sockets | Per-worker mutex, moved into worker-owned connections |
| Base Store, expiry index, AOF state, compaction delta, diagnostics | Engine mutex |
| Immutable compaction snapshot and candidate file | Compaction thread |
| Stop/failure flags, client and command counters | Atomics |
| Worker count | Set before worker publication; immutable thereafter |

The model has one acceptor, W workers, one maintenance thread, and at most one
compaction thread. Client count does not determine thread count. Worker connection
assignment is stable and round-robin, not dynamically balanced. Poll/WSAPoll rebuilds
a descriptor array per pass. Empty workers wait on a condition variable; populated
workers poll for up to 20 ms. A future socketpair/eventfd wakeup would reduce new
connection assignment latency without changing command semantics.

## Lock discipline

Pending-queue locks cover moving sockets only; command execution does not hold them.
The engine mutex protects evaluation, staging, WAL append/sync, memory publication,
and shared metrics. There is no nested Store or AOF lock. Network I/O and response
encoding happen after this mutex is released. There is no path from an engine lock
back into a pending-queue lock.

The maintenance thread may wait for an active command. `always` deliberately holds
the engine mutex through a disk sync: releasing it would allow later commands to
observe uncommitted or out-of-order state. This is a real contention point, not an
accidental lock around network I/O. `everysec` also pauses commands during its sync.
Snapshot capture and final compaction publication are other documented long sections.

## Atomicity and expiration

All commands are serialized relative to other commands; a successful mutation's
visible linearization point is publication under the engine lock after the WAL
requirement. Read commands see one consistent state. A transaction takes one time
sample and one lock across its whole evaluation. Keys cannot expire in the middle
of that transaction merely because the wall clock advances during an fsync.

INCR reads, validates, checks overflow, logs the final number, and replaces the entry
within this serialization. The 100-thread unit stress asserts exactly 100,000
increments, and real TCP clients independently test the network path. Paired-key
transaction writers and an MGET reader verify that intermediate pair states are
never visible. Arithmetic preserves an existing TTL; SET/MSET clear it.

## Fairness, bounds, and slow peers

A connection gets up to 64 commands per service pass and one bounded recv/send
chunk. Input is not read when output backlog reaches 64 KiB; a single larger reply
can exceed that high-water mark but must fit the 16 MiB hard response budget.
Partially sent output retains an offset and compacts consumed prefixes. Fragmented
incomplete input does not force zero-timeout polling; only runnable buffered work
does. Connections at max-clients receive a best-effort error and are closed.

These are per-connection limits, not global resource accounting. Many clients can
still consume substantial aggregate memory. Idle clients have no timeout. A command
such as exact DBSIZE after mass expiry, a large transaction, or a slow fsync can delay
others. The design does not claim real-time fairness.

## Shutdown and errors

Signal handlers set a flag only. Main stops accepting, sets the worker stop flag,
and joins workers; an already executing operation finishes, while queued/pipelined
work may be cancelled. Sessions vanish on disconnect. No waiting for a client to
read replies is required. The engine then joins maintenance/compaction and syncs.

Expected framing failures affect one client. Aborted accepts, would-block results,
and interrupted I/O are handled without terminating the database. An unexpected
worker exception stops the server and is surfaced as a nonzero exit. An append/sync
failure faults mutations, keeps reads of the last published state available, and
requires restart. Socket errors after a commit cannot undo that commit.

## Audit notes

- Owned copies cross parser and command boundaries; no borrowed Store references
  escape the lock. Moving sockets transfers ownership exactly once.
- Snapshot disk work reads only immutable paths/data and its own File handle.
- The candidate cannot replace the log before captured deltas are appended.
- The previous compactor's join occurs only after its inactive flag is published
  under the mutex and its last engine access has ended.
- Parser state distinguishes incomplete work from runnable pipelines to avoid
  spinning on a slowly arriving bulk body.
- Tests use barriers or explicit fault checkpoints for concurrency overlap. Sleeps
  are limited to polling conditions and advancing real expiration deadlines.

See [testing](testing.md) for the sanitizer execution record; configurations alone
are not evidence that a race detector ran.
