# Architecture and invariants

## Data flow and module boundaries

`src/networking/socket.cpp` is the OS adapter. Socket ownership is move-only and
destruction closes the native handle. Windows uses Winsock, WSAPoll, and native file
handles; Linux uses BSD sockets, poll, file descriptors, flock, fsync, and rename.

`src/server/server.cpp` runs one acceptor on main and a fixed number of reactors.
Connections stay with one worker until disconnect. Each owns its parser, response
buffer, send offset, transaction session, and socket. No transaction/session object
is shared between workers. The parser produces owned argument strings, so removing
or compacting its input buffer cannot invalidate a command being evaluated.

`src/protocol/resp.cpp` is independent of sockets. Its state machine resumes at a
line header, bulk length, or bulk body. Parsed arguments are retained across recv
calls. Line searches resume at the previous scan position, with one-byte overlap
for split CRLF. It neither reparses previously accepted arguments nor assumes a
receive corresponds to a request. Complete frames are consumed independently from
later pipelined frames. Each cumulative frame length is checked before allocation.

`src/commands/session.cpp` validates command shape and owns MULTI state. The engine
handles semantics, shared-state serialization, and persistence ordering. Store has
no sockets or files. Aof knows final mutations, not client command names. File handles
short I/O, interruptions, synchronization, replacement, and lifetime.

## Command lifecycle

1. Parse one bounded complete frame; malformed framing generates an error and closes
   that connection so uncertain trailing bytes cannot become commands.
2. Validate command and arity. In MULTI, queue a bounded copy and reply QUEUED.
3. Acquire the engine mutex; record wait time and take one wall-clock timestamp.
4. Evaluate against an overlay of staged values and the base Store. Reads within
   a transaction see earlier staged writes. Numeric errors become replies.
5. Collect final per-key mutations. Multiple writes to a key collapse to one final
   state; this preserves the observable result because there is no interleaving.
6. Encode one checksummed WAL record, append it, and apply the configured sync policy.
7. Publish mutations to Store, release the engine mutex, encode/send the reply.

The log describes outcomes: a counter ends at `42`; it does not ask recovery to run
INCR again. If publish allocation fails after a successful WAL append, terminate
the process rather than report an ordinary rejected write while continuing with
partially published memory. Restart reconstructs the complete committed record.

## Store and ownership

The base store is `unordered_map<string, Entry>`. An Entry owns its value and a signed
64-bit absolute deadline (`-1` means none). Strings returned to evaluation are owned
copies; no iterator/reference survives mutation or unlock. The deliberate copying
makes lifetimes simple and imposes a measurable cost for large values.

The expiration index is an ordered map keyed by `(deadline, key)`. Every expiring
key has one index entry. Applying a new state removes the old deadline before adding
the new one. This costs O(log E) per TTL change and O(E) index space, where E is the
number of expiring keys. It avoids heap tombstones accumulating when one key's TTL
is updated repeatedly. Ordinary key lookup/update is expected O(1), excluding
string bytes, WAL I/O, and expiration-index work.

Maintenance removes at most 256 due keys per 50 ms pass. Lazy access also removes
expired keys, so active cleanup delay does not resurrect expired values. Expiration
deletion need not produce a tombstone: the original absolute deadline remains in
the log and produces the same logical absence after replay. Exact count commands
are exceptional: they drain all due entries under the lock and can take longer.

## Background work

Maintenance owns one thread and uses the engine condition variable for interruptible
waits. Expiration and periodic sync run under the engine mutex. Compaction uses a
separate joinable thread with an immutable snapshot and an independent candidate
file. Only snapshot capture and publication/delta handoff use shared state. An
exclusive data-directory lock is held across recovery, normal writes, and rewrite.

There is at most one active compaction. A completed compactor is joined before
another starts; it performs no shared-state accesses after marking itself inactive.
Shutdown stops network work, joins workers, signals maintenance, joins both engine
threads, syncs the AOF, and then releases files. See the dedicated persistence and
concurrency documents for the interleavings and failure behavior.

## Scope and scaling decisions

One mutex sacrifices concurrent read evaluation for easy-to-defend atomic MSET,
INCR, and MULTI/EXEC semantics. Adding per-shard locks would require ordering locks
for multi-key transactions and coordinating a global WAL order. Group commit can
first reduce expensive syncs while retaining one serial commit order; it requires
acknowledgment tracking and failure fan-out, so periodic sync is not mislabeled as
group commit here.

At 10x load, first measure lock wait, append time, CPU, memory, and response queuing.
Potential improvements are group commit and reactor wakeup descriptors. At 100x
state, copying a snapshot becomes the main memory/pause concern: immutable versions,
copy-on-write snapshots, a bounded memory policy, and a disk-oriented storage layout
deserve consideration. Replication would additionally require sequence numbers,
acknowledgment rules, split-brain prevention, and a tested recovery protocol.
