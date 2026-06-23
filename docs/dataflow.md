# MiniDB Data Flow

End-to-end paths through the system. Use this when implementing or debugging
a feature that touches more than one module.

---

## 1. `SELECT` execution

Example: `SELECT id, name FROM users WHERE id = 10;`

1. CLI calls `QueryEngine::execute(sql)`.
2. **Parser** tokenises and parses into a `parser::Stmt`.
3. **Planner** builds a `LogicalPlan`:
   `Project(id, name) → Filter(id = 10) → SeqScan(users)`.
4. **Optimizer** estimates `|users|`, sees the filter on the primary key,
   rewrites the plan to:
   `Project(id, name) → IndexScan(users.idx_pk, key=10)`.
5. **Executor** opens the IndexScan, calls `next()` until DONE.
   - IndexScan asks the **IndexManager** for the B+ tree, calls
     `BPlusTree::search(10, rid)`.
   - It then asks the **HeapFile** to materialise the row: `getRecord(rid)`.
6. The Project operator trims to `(id, name)`.
7. CLI returns the tuple list to the caller.

```text
SQL
 ↓
Parser → Stmt
 ↓
Planner → LogicalPlan
 ↓
Optimizer → PhysicalPlan
 ↓
Executor (Volcano)
   ├── IndexScan   → IndexManager → BPlusTree
   └── HeapFile    → BufferPool → DiskManager
 ↓
Tuple list
```

---

## 2. `INSERT` execution

Example: `INSERT INTO users VALUES (1, 'Alice');`

1. Parser → INSERT Stmt.
2. Planner → `LogicalInsert`.
3. Optimizer → `PhysicalInsert`.
4. Executor::InsertExecutor::init():
   - **TransactionManager::begin()** → txn id.
   - **WAL::append(BEGIN)**.
   - Lock the target page (or the row) — 2PL path; MVCC path skips the lock.
5. Executor::InsertExecutor::next() (called once for a single-row insert):
   - `HeapFile::insertRecord(serialisedRow, rid)`.
   - `BPlusTree::insert(pk, rid)` for every index on the table.
   - **WAL::append(INSERT)** with the rid and the serialised row.
6. Executor finishes:
   - **WAL::append(COMMIT)**.
   - **WAL::flush()** (fsync).
   - **TransactionManager::commit(txn)** → release locks (2PL).

```text
INSERT
 ↓
Parser → Executor.InsertExecutor
 ↓
TransactionManager.begin
 ↓
WAL.append(BEGIN)
 ↓
HeapFile.insertRecord  ──→  BufferPool  ──→  DiskManager
 ↓
BPlusTree.insert (per index)
 ↓
WAL.append(INSERT)
 ↓
WAL.append(COMMIT) + flush
 ↓
TransactionManager.commit
```

---

## 3. `DELETE` execution

Example: `DELETE FROM users WHERE id = 10;`

1. Parser → DELETE Stmt.
2. Optimizer chooses `IndexScan(id=10)` to locate the row.
3. Executor::DeleteExecutor:
   - `TransactionManager::begin()`.
   - **WAL::append(BEGIN)**.
   - For each rid from the scan:
     - **WAL::append(DELETE)** with rid.
     - `HeapFile::deleteRecord(rid)`.
     - `BPlusTree::remove(pk)` for every index.
   - **WAL::append(COMMIT)** + flush.
   - `TransactionManager::commit(txn)`.

---

## 4. `BEGIN` / `COMMIT` / `ROLLBACK`

- `BEGIN`  → `TransactionManager::begin()` → new `Transaction` → `WAL::append(BEGIN)`.
- `COMMIT` → `WAL::append(COMMIT)` + `WAL::flush()` + `TransactionManager::commit(txn)`
  → `LockManager::releaseAll(txn)` (2PL only).
- `ROLLBACK` → `WAL::append(ABORT)` + run undo pass for the txn's last LSN
  → `LockManager::releaseAll(txn)`.

---

## 5. MVCC snapshot visibility

A row is `(key, value, created_txn, deleted_txn)`, where `deleted_txn = 0`
means "still live".

For a reader with snapshot `(snapshotHigh = highest active txn at BEGIN)`:

```text
visible  ⟺  created_txn ≤ snapshotHigh
            AND (deleted_txn == 0 OR deleted_txn > snapshotHigh)
```

Write-write conflict at commit: if another committed txn has already created
a new version of the same key since our snapshot, our commit fails with
`TXN_CONFLICT`. The executor retries up to a configurable bound.

---

## 6. Strict 2PL serializable example

T1: `Read A; Write A; Commit`
T2: `Read A; Write A; Commit`

```text
T1: X-lock A   ─┐
T2:            ─┼─→ blocks on T1
T1: ...        ─┘
T1: commit → release X-lock A
T2: unblocks, X-lock A, write, commit
```

The schedule is serial (T1 before T2), and serializable.

---

## 7. Deadlock detection

```text
T1: X-lock A
T2: X-lock B
T1: request X-lock B  →  waiting for T2
T2: request X-lock A  →  waiting for T1
```

`LockManager` maintains a `waits_for` map `(TxnId → TxnId)` and runs cycle
detection (DFS) whenever a new wait edge is added. On cycle, the txn with
the larger id is aborted (younger dies).

---

## 8. ARIES recovery

A crash leaves the database in some mix of:

- pages flushed to disk (some updates persisted)
- pages still in the buffer pool (lost)
- WAL records on disk (always — every commit is `fsync`'d)

Startup recovery:

1. **Analysis** — scan WAL from the last `CHECKPOINT` forward. Build
   - `att = {txn → last LSN}` for txns active at crash time
   - `dirtyPageTable` (page → first LSN that dirtied it)
2. **Redo** — repeat history. From the smallest recLSN in dirtyPageTable,
   walk WAL, re-apply every update to its page (no-op if the page's
   `pageLSN ≥ record.LSN`).
3. **Undo** — for every txn in `att`, walk its log backwards applying
   compensating log records (CLRs) until it reaches its `BEGIN`. Write
   `ABORT` for the txn.

Result: committed txns are durable, uncommitted ones are rolled back.

---

## 9. Buffer pool page request

```text
fetchPage(10)
   │
   ▼
in cache?  ─── yes ──→ pin it, return Page*
   │
   no
   ▼
free frame? ─── no ──→ pick LRU victim, flush if dirty, then load
   │
   yes
   ▼
DiskManager.readPage(10) → Page*
pin it, return Page*
```

---

## 10. B+ tree lookup

```text
search(100)
   │
   ▼
root (internal)  ──→ choose child by key range
   │
   ▼
internal          ──→ choose child by key range
   │
   ▼
leaf              ──→ linear scan inside the leaf
   │
   ▼
RecordId
```

Time complexity `O(log_F N)` for `N` rows and node fanout `F`.

---

## 11. End-to-end system flow

```text
Client (CLI)
   │
   ▼
Parser
   │
   ▼
Planner → Optimizer
   │
   ▼
Executor
   ├── Index (BPlusTree)
   ├── HeapFile
   │     │
   │     ▼
   │   BufferPool ──→ DiskManager
   │
   ├── TransactionManager ──→ LockManager (2PL baseline)
   │
   └── RecoveryManager ──→ WAL  (fsync on commit)
```
