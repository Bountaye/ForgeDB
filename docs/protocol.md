# Wire protocol and command semantics

ForgeDB accepts a RESP2 request subset: a nonempty array of non-null bulk strings.
The first string is an ASCII command name, case-insensitive. Keys and values are
byte strings; neither UTF-8 nor a text terminator is required. For manual use, CRLF
terminated inline commands also support spaces, single/double quotes, and backslash
escapes (`\n`, `\r`, `\t`, escaped quotes/backslash). RESP is preferred for arbitrary
binary data. Bare LF is not a frame delimiter.

```text
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$7\r\nNishant\r\n
```

## Replies

| Reply type | Example encoding |
|---|---|
| Simple success | `+OK\r\n` |
| Error | `-ERR wrong number of arguments\r\n` |
| Signed integer | `:42\r\n` |
| Bulk string | `$5\r\nhello\r\n` |
| Missing value | `$-1\r\n` |
| Array | `*2\r\n$1\r\na\r\n$-1\r\n` |

Framing errors produce an error and connection closure; no following request is
executed. Command/arity/type errors produce an error and leave framing synchronized.
Multiple frames can be pipelined; replies remain in request order per connection.
TCP half-close on the request direction allows complete buffered requests to drain.
An incomplete trailing request at EOF is cancelled after earlier responses finish.

Limits are applied to cumulative frame bytes, not recv chunk boundaries. The parser
retains both parse state and completed arguments across reads. It rejects excessive
array/bulk sizes before reserving attacker-specified memory. The receive buffer permits
one bounded recv chunk of spillover so the next pipelined request does not make a
valid preceding frame appear oversized.

## Semantics worth knowing

- SET/MSET replace values and clear old TTLs. Duplicate MSET keys take the last value.
- DEL counts distinct keys actually removed. EXISTS counts arguments that exist,
  including duplicates. MGET preserves request order and missing-value positions.
- Numeric commands accept a fully consumed signed decimal int64, with no leading
  `+`, spaces, fractions, or trailing characters. A missing key starts at zero.
  Overflow returns an error without mutation. Existing TTLs are preserved.
- EXPIRE accepts signed seconds; zero/negative deletes an existing key immediately.
  Missing keys return 0. TTL uses floored remaining seconds; -2 is absent, -1 has no
  expiration. PERSIST returns 1 only when an existing TTL was removed.
- INFO is a bulk string of CRLF-separated `name:value` fields. Counts are process-local
  except reconstructed live state/AOF bytes; they are not invented estimates.

## Transactions

MULTI starts a session-local queue, returning OK. Ordinary valid commands return
QUEUED; they have not executed or been logged yet. DISCARD clears the queue. A client
disconnect implicitly discards it. Nested MULTI, wrong arity, unknown commands,
administrative INFO/COMPACT in a queue, and queue-size violations mark the transaction
dirty; EXEC then aborts without applying queued writes.

EXEC returns an array of per-command results. Runtime numeric/type/overflow errors
are returned as elements while other commands continue. All successful mutations
publish together, after one complete WAL record. A hard result/WAL limit failure
aborts the entire staged batch with an error. There is no WATCH, conflict retry,
savepoint, rollback-on-runtime-error, or exactly-once command identifier.

Transactions fix their time sample at EXEC start. The engine lock prevents all
interleaving, including active expiration. This is strict serialization at one node,
not a distributed ACID or multi-version system. A successful acknowledgment's
durability still depends on the configured fsync policy.
