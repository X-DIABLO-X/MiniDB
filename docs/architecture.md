# MiniDB Architecture

> Status: **Frozen at M0.** Any change after Day 1 must be agreed by the
> whole team and reflected in `interfaces.md` and the per-module READMEs.

## Overview

MiniDB is a single-process, in-memory-pages-plus-disk relational database
implemented in modern C++17. It is intentionally small — the goal is *clarity
and correctness of the internal architecture*, not feature breadth.

### What MiniDB supports

- **Storage**: 4 KB paged heap files, page-level disk manager, LRU buffer
  pool.
- **Indexing**: clustered B+ tree primary key plus one optional secondary
  index per table.
- **SQL surface**: `SELECT` (with `WHERE`, `JOIN`, `GROUP BY`, `ORDER BY`,
  `LIMIT`), `INSERT`, `DELETE`, `CREATE TABLE`, `DROP TABLE`,
  `BEGIN`/`COMMIT`/`ROLLBACK`.
- **Optimizer**: selectivity-based choice between sequential scan and index
  scan, plus a single-level join-order heuristic.
- **Transactions**: Strict 2PL baseline + MVCC snapshot isolation
  (extension track).
- **Recovery**: ARIES-style redo/undo over a single WAL file.

### What MiniDB does not support

Networking, authentication, prepared statements, views, subqueries, DDL
beyond `CREATE TABLE` / `DROP TABLE`, types beyond `INT`, `FLOAT`,
`VARCHAR(n)`, `BOOL`, stored procedures, triggers, replication.

---

## High-level architecture

```text
+----------------------+
|      SQL Client      |   <-- src/cli/main.cpp (REPL / file mode)
+----------+-----------+
           |
           v
+----------------------+
|      SQL Parser      |   include/parser
+----------+-----------+
           |
           v
+----------------------+
|   Logical Planner    |   include/planner (LogicalPlan)
+----------+-----------+
           |
           v
+----------------------+
|    Optimizer         |   include/planner (Optimizer)
+----------+-----------+
           |
           v
+----------------------+
|   Physical Plan      |   include/planner (PhysicalPlan)
+----------+-----------+
           |
           v
+----------------------+
|     Executor         |   include/executor (Volcano)
+---+----------+------+
    |          |
    v          v
+-------+  +-----------+
| Index |  | Heap File |   include/index  include/storage
+---+---+  +-----+-----+
    |            |
    +-----+------+
          |
          v
   +--------------+
   | Buffer Pool  |   include/storage
   +------+-------+
          |
          v
   +--------------+
   | Disk Manager |   include/storage
   +--------------+

+-----------------------+      +-----------------------+
| Transaction Manager   |----->|   Lock Manager        |  (2PL baseline)
| include/transaction   |      |   include/transaction |
+----------+------------+      +-----------------------+
           |
           v
+-----------------------+
|   Recovery Manager    |   include/recovery
+----------+------------+
           |
           v
+-----------------------+
|   Write-Ahead Log     |   include/recovery
+-----------------------+
```

---

## Module responsibilities (one paragraph each)

### `common`
No behaviour — only `using` aliases and the `Status` enum that every
function returns. Lives in `include/common/`.

### `storage`
- `Page` — fixed-size byte buffer with a slot directory header.
- `DiskManager` — maps `PageId` ↔ file offset, owns `data/pages/`.
- `BufferPool` — in-memory page cache, LRU, `pin`/`unpin`/`flush`.
- `HeapFile` — slotted pages organised into a free-page list, hands out
  `RecordId`s.

### `catalog`
- `Schema` — column list for one table.
- `TableInfo` — schema + heap-file handle + indexes.
- `CatalogManager` — opens/creates/drops tables, persists to
  `data/metadata/catalog.dat`.

### `index`
- `Node` — internal + leaf node layout.
- `BPlusTree` — search, insert, delete, range scan.
- `IndexManager` — owns B+ tree instances keyed by `(table, column)`.

### `parser`
- `Lexer` — tokenises SQL.
- `Parser` — recursive-descent into an `AST`.
- `AST` — node types for `SELECT`, `INSERT`, `DELETE`, `CREATE`, `DROP`,
  transaction statements.

### `planner`
- `LogicalPlan` — relational algebra tree.
- `PhysicalPlan` — operator tree (SeqScan, IndexScan, Filter, Project,
  NestedLoopJoin, HashJoin, Aggregate, Sort, Limit, Insert, Delete).
- `Optimizer` — converts logical → physical, cost-based.

### `executor`
Volcano-style `Init/Next/Close` executors — one class per physical operator.
Holds references to `BufferPool`, `CatalogManager`, `TransactionManager`,
`IndexManager` via interfaces only.

### `transaction`
- `Transaction` — id, state, snapshot, local write set.
- `LockManager` — shared/exclusive locks on `RecordId`s, wait-for graph
  deadlock detection. Used in the 2PL baseline.
- `TransactionManager` — `begin/commit/abort`, snapshot bookkeeping for MVCC.

### `recovery`
- `LogRecord` — tagged union of log record kinds.
- `WAL` — append-only file in `data/wal/`, `fsync` on commit.
- `RecoveryManager` — ARIES analysis → redo → undo on startup.

---

## Design goals (in priority order)

1. **Correctness** — a `SELECT` that returns a wrong answer is worse than
   one that crashes.
2. **Modularity** — one folder per module, interfaces in `interfaces.md`,
   no cross-module private access.
3. **Independent development** — after the contracts are frozen, the four
   of us can work in parallel.
4. **Educational clarity** — the code should be readable by a student who
   has taken the course, not by a database internals expert.
5. **Testability** — every module has a `tests/<module>/` folder with
   unit tests that can run without the rest of the system.

---

## Extension track

**Track B — Concurrency (MVCC).**

We replace the default lock manager with multi-version concurrency control.
The 2PL `LockManager` is kept in the tree as a baseline so the benchmark
report can compare the two. See `docs/dataflow.md` §"MVCC snapshot
visibility" for the rules.

---

## Cross-module call rules

1. Always call through the interface in `docs/interfaces.md`.
2. Never include another module's `src/` files; only its `include/` headers.
3. If you need a new public function, propose it in chat, get two acks,
   update `interfaces.md`, *then* implement.
4. Lower layers never depend on higher layers:
   `common ← storage ← catalog ← index ← planner ← executor`,
   `common ← storage ← transaction ← executor`,
   `common ← storage ← recovery`.

---

## Open architectural questions

- Single buffer pool or per-table pools? — **single** for now.
- MVCC version chain per heap page or per row? — **per row** (slot contains
  a head pointer).
- Where does the WAL live? — `data/wal/minidb.wal` (single file, segment
  rotation is out of scope).
