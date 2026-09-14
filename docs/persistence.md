# Persistence, commit ordering, and recovery

## Files and ownership

The data directory contains `LOCK`, `append.aof`, and temporarily
`append.compacting`. The process holds an exclusive lock on LOCK for the full engine
lifetime. POSIX uses advisory flock; Windows denies sharing of the lock handle.
The lock file is left in place, so removing/recreating lock-file names is not part
of normal operation. Processes that ignore the lock or modify files externally are
outside the consistency contract. Use a local filesystem, not a network share or
actively synchronized cloud folder, for durability-sensitive operation.

## Version 1 record format

All integers are little-endian, explicitly encoded byte by byte. There are no
native-struct dumps, alignment dependencies, or newline-delimited values.

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 4 | ASCII `FDB1` magic/version |
| 4 | 4 | Unsigned payload length, at most 16 MiB |
| 8 | 4 | IEEE CRC32 of the payload |
| 12 | 4 | IEEE CRC32 of the preceding 12 header bytes |
| 16 | length | Mutation payload |

Payload starts with a uint32 mutation count, 1–65536. Each mutation is:

```text
uint8 tag                 0 = delete, 1 = replace
uint32 key_length
byte[key_length] key
if tag == 1:
    int64 expires_at_ms   -1 = persistent; otherwise absolute Unix milliseconds
    uint32 value_length
    byte[value_length] value
```

Signed expiry bits are round-tripped using `std::bit_cast`, not implementation-defined
out-of-range unsigned-to-signed conversion. Header CRC protects the length before
it is used to allocate. The decoder also bounds mutation counts and string lengths,
rejects unknown tags, invalid deadlines, and trailing bytes, and validates the whole
record before applying any of its mutations. CRC32 is an accidental-error detector,
not a cryptographic integrity or authenticity mechanism.

## Write-ahead order

Evaluation constructs a per-key overlay and all replies before commit. Each final
changed key becomes a replace/delete mutation. The complete record is encoded before
disk is touched. With the engine mutex still held:

1. Reserve/copy any required compaction delta record.
2. Append all bytes, retrying short writes and POSIX EINTR.
3. Under `always`, synchronize the file.
4. Publish all changes in memory.
5. Unlock; encode and deliver the response.

No other client can observe staged state before the required persistence step.
A transaction uses one record, so no verified prefix of its payload can become a
partially recovered transaction. Records store final numeric values and absolute
expiration times, avoiding non-idempotent command replay and renewed TTLs.

If memory allocation fails during publication after durable logging, the process
terminates; continuing with a partially published transaction would violate isolation.
The precomputed log record remains sufficient for replay after restart.

## Durability policies

| Policy | Acknowledgment point | Tradeoff |
|---|---|---|
| `always` (default) | After append and successful file sync | Strongest durability; one sync per mutating command or EXEC |
| `everysec` | After OS write returns; maintenance syncs dirty data roughly once/sec | Higher throughput; a crash can lose recent acknowledgments |
| `none` | After OS write returns | No periodic sync; durability depends on OS writeback until graceful shutdown |

The maintenance period is not a hard real-time promise: scheduler delay, contention,
and a stalled device can extend the periodic-sync window. A successful orderly
shutdown syncs all policies. `always` uses FlushFileBuffers on Windows and fsync on
POSIX. These calls rely on the filesystem and device honoring their contract.

On append or sync failure, memory is not published and the AOF enters a faulted
state. Later mutations are rejected. A complete record may nevertheless have reached
disk before a sync failed, so the failed request's outcome is uncertain across
restart. Reads continue from last published memory; INFO exposes the fault. The
server does not append after a damaged tail or silently downgrade durability.

## Startup recovery

Startup scans sequentially from byte zero. It does not accept connections until
replay and cleanup finish. A physical tail shorter than a full header or its declared
payload is diagnosed on stderr, truncated to the last complete verified boundary,
and synchronized before normal writes begin. INFO sets `recovered_tail:1` for that
process. All preceding verified mutations are retained.

A full header with wrong magic/CRC, an over-limit length, a complete payload with a
wrong CRC, or a semantically invalid payload causes a nonzero startup failure. The
file remains unchanged. Operators should preserve a copy and investigate; there is
no automatic salvage that skips arbitrary corrupt regions. A short final suffix
is classified as a torn write, not proof of the exact physical cause of damage.

Expired entries are removed using the current wall clock after replay. Deletions,
PERSIST, TTL changes, and overwrites have already been encoded as final key states.
An abandoned `append.compacting` is ignored and removed only after the authoritative
AOF has successfully replayed. No uncommitted candidate is promoted at startup.

## Compaction timeline

```text
Engine lock: copy live snapshot S; begin empty delta D
Unlock
Compactor: write records for S to append.compacting; sync snapshot
Workers:   commit normal mutations to append.aof and append records to D
Engine lock: stop new commits
Compactor: append D in commit order; sync candidate
           atomically replace append.aof; install current file handle
           synchronize directory metadata on POSIX; stop delta capture
Unlock
```

The snapshot and beginning of delta capture share one critical section; there is
no gap where a write belongs to neither. Every captured record was also appended to
the original AOF. Deltas are final-state mutations, and their order matches the
engine serialization. Expiration during the rewrite is safe because S retains
absolute deadlines; a key that expires while disk writes run remains logically absent.

Memory overhead is a full live snapshot plus at most 32 MiB of deltas (and temporary
encoding buffers). Crossing the delta bound cancels that rewrite while normal
writes continue on the original log. It is exposed in `background_error`; the
operator can retry when write pressure is lower. Compaction is requested explicitly,
not automatically on file-size thresholds.

POSIX can replace an open log with rename and continue using the candidate descriptor,
then fsync the data directory. Windows testing found that replacing an open destination
failed with access denied; that path closes both handles under the engine lock,
uses MoveFileExW with replace/write-through flags, and reopens the new log. It never
deletes the authoritative name before replacement. A rename/reopen failure on this
path faults further writes. Windows lacks the directory-fsync step used by POSIX;
power-failure equivalence across these platforms is not claimed.

## Crash matrix

| Interruption | Recovery authority |
|---|---|
| Before append | Previous committed AOF |
| During final header/body | Previous complete records; report and truncate torn tail |
| After full write, before sync/reply | Full verified record may replay; client outcome uncertain |
| After `always` success reply | Record was synchronized before acknowledgment |
| During snapshot/delta generation before replacement | Original AOF; ignore temporary file |
| After synchronized candidate replaces name | Complete snapshot plus captured deltas |
| Complete record corruption | Refuse startup; preserve file for investigation |

Process tests actually kill and restart the server. The test-only server also
offers a checkpoint after the first snapshot record so killing an in-progress rewrite
is deterministic. Another injected I/O failure writes and syncs half a real WAL
record before raising a disk-full error, proving that failed transactions are not
published and later writes are rejected. These are controlled fault tests, not a
physical power-loss or filesystem certification test.

Native API references: [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers),
[MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw),
[CreateFileW sharing rules](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew).
