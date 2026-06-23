# `transaction/` — 2PL + MVCC

MiniDB ships **two** concurrency-control implementations:

1. **Strict 2PL** (`LockManager`) — the baseline, used for the Track A
   performance comparison.
2. **MVCC** (in `TransactionManager`) — the default for the demo and the
   Track B extension. `TransactionManager::isVisible` encodes the snapshot
   visibility rules.

Both implementations share the same `Transaction` object (id, state,
snapshot high-water mark).

## Files in this folder

| Header | Responsibility |
|---|---|
| `include/transaction/transaction.h`        | `Transaction`, `TxnState`, `LockMode`. |
| `include/transaction/lock_manager.h`       | `LockManager` (2PL baseline). |
| `include/transaction/transaction_manager.h` | `TransactionManager` (begin/commit/abort, MVCC visibility, owns `LockManager`). |

## Public API (contract)

```cpp
namespace minidb::transaction {

enum class TxnState { ACTIVE, COMMITTED, ABORTED };
enum class LockMode { SHARED, EXCLUSIVE };

class LockManager {
public:
    Status acquireShared   (TransactionId txn, RecordId rid);
    Status acquireExclusive(TransactionId txn, RecordId rid);
    void   releaseAll      (TransactionId txn);    // 2PL: at commit/abort
    bool   hasCycle();                            // exposed for tests
};

class Transaction {
public:
    TransactionId id() const;
    TxnState      state() const;
    TransactionId snapshotHigh() const;            // MVCC
};

class TransactionManager {
public:
    TransactionId begin();
    Status commit  (TransactionId txn);
    Status abort   (TransactionId txn);

    // MVCC visibility test (see docs/dataflow.md §5).
    bool isVisible(TransactionId rowCreator,
                   TransactionId rowDeleter,
                   const Transaction& reader);

    // 2PL baseline entry point.
    LockManager& lockManager();

    // MVCC write-set bookkeeping.
    void recordWrite(TransactionId txn, RecordId rid);
    bool hasConflict(TransactionId txn);
};

}
```

## Locking rules (2PL baseline)

- Every `INSERT` / `UPDATE` / `DELETE` acquires an **exclusive** lock on
  the affected `RecordId` (or the target page if the rid is not yet known).
- Every `SELECT` that goes through a `SeqScan` (not an `IndexScan` with
  isolation guarantees) acquires **shared** locks.
- Locks are released **only** at commit or abort → "strict" 2PL.
- Deadlock detection: when a request would block, insert the
  `(txn → blocker)` edge into a wait-for graph and run a DFS for cycles.
  On cycle, abort the txn with the **higher** id (younger dies).

## MVCC visibility rules

For a reader with snapshot `S = max active txn at BEGIN`:

```text
visible(row)  ⟺  row.createdBy ≤ S
                AND (row.deletedBy == 0  OR  row.deletedBy > S)
```

Writers append new row versions; readers never block on writers and
writers never block on readers. Write-write conflicts are detected at
**commit time** via first-updater-wins on the row's primary key.

## How other modules use the transaction layer

| Module | Calls |
|---|---|
| `executor/Insert/Delete` | `TransactionManager::begin/commit/abort`. |
| `executor/Insert/Delete` | `LockManager::acquireX` (2PL path). |
| `executor/SeqScan/IndexScan` | `TransactionManager::isVisible` (MVCC path). |
| `recovery/RecoveryManager` | `TransactionManager::abort` for every txn active at crash time. |

## Rules

- The transaction layer does **not** own pages; the lock manager holds
  *no* page pins. The lock is a logical lock on `(txn, rid)`. The
  executor is responsible for pinning the page for the duration of the
  lock.
- `TransactionManager` is the **only** place that allocates `TransactionId`s.
- WAL records carry a `txnId` field — this is the source of truth for
  which transactions are in flight at recovery time.