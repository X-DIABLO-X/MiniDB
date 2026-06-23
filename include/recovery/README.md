# `recovery/` — WAL + ARIES

The recovery subsystem guarantees that committed transactions survive a
crash and uncommitted ones roll back. The implementation is ARIES-style:

1. **Analysis** — scan WAL from the last `CHECKPOINT` forward; compute
   `att` (active-txn table) and `dirtyPageTable`.
2. **Redo** — repeat history from the smallest recLSN.
3. **Undo** — for each txn in `att`, walk its log backwards applying
   compensating log records (CLRs) until it reaches its `BEGIN`.

## Files in this folder

| Header | Responsibility |
|---|---|
| `include/recovery/log_record.h`        | `LogKind` enum + `LogRecord` payload. |
| `include/recovery/wal.h`                | Append-only WAL file with `fsync`. |
| `include/recovery/recovery_manager.h`   | The ARIES orchestrator. |

## Public API (contract)

```cpp
namespace minidb::recovery {

enum class LogKind { BEGIN, COMMIT, ABORT, INSERT, UPDATE, DELETE, CHECKPOINT };

struct LogRecord {
    LogKind       kind;
    TransactionId txnId;
    LSN           prevLSN;
    // payload depends on `kind`:
    RecordId      rid;        // INSERT / UPDATE / DELETE
    std::vector<std::uint8_t> afterImage;  // UPDATE
    std::vector<std::uint8_t> beforeImage; // UPDATE for CLR
};

class WAL {
public:
    explicit WAL(const std::string& path);
    ~WAL();
    LSN    append (const LogRecord& r);
    Status flush ();
    Status read   (LSN from, std::function<bool(const LogRecord&)> visit);
    LSN    currentLSN() const;
};

class RecoveryManager {
public:
    RecoveryManager(WAL* wal,
                    storage::BufferPool* bp,
                    catalog::CatalogManager* cat,
                    index::IndexManager* idx,
                    transaction::TransactionManager* txn);
    Status runAtStartup();
};

}
```

## Log record byte format

```text
| magic | kind | txnId | prevLSN | payloadLen | payloadBytes | crc32 |
| u16   | u8   | u64   | u64     | u32        | ...          | u32   |
```

`payload` is `kind`-dependent:
- `BEGIN`    — empty.
- `COMMIT`   — empty.
- `ABORT`    — empty.
- `INSERT`   — `rid (u64)`, `rowBytes (u32 len + bytes)`.
- `UPDATE`   — `rid (u64)`, `before (u32 len + bytes)`, `after (u32 len + bytes)`.
- `DELETE`   — `rid (u64)`, `rowBytes (u32 len + bytes)`.
- `CHECKPOINT`— `activeTxnCount (u32)`, followed by `txnId (u64)` per active txn.

## WAL on disk

`data/wal/minidb.wal` is a single append-only file. Records are written
sequentially; `flush()` calls `fsync` so the bytes are durable.

## Write-ahead rule

A page may be flushed to disk **only if** every log record whose effect
is in that page has been `fsync`'d. In code terms: `WAL::flush()` must
precede `BufferPool::flushPage()`. The executor enforces this.

## How other modules use the recovery layer

| Module | Calls |
|---|---|
| `cli/main.cpp` | constructs `WAL` + `RecoveryManager` at startup, calls `runAtStartup`. |
| `executor/Insert/Delete` | `WAL::append` + `WAL::flush` per the write-ahead rule. |
| `executor/QueryEngine` | constructs a `RecoveryManager` and passes it in. |

## Rules

- The WAL file is **append-only**. We never rewrite a record; if we need
  to "undo" a record we append a CLR.
- Recovery **must** run before any query is accepted. The CLI's startup
  sequence is: WAL → Recovery → Catalog → Index → BufferPool populate →
  QueryEngine ready.
- The WAL is **single-threaded for writes**. Concurrent appenders
  serialise through the WAL's internal mutex.