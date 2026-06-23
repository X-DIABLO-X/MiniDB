# MiniDB

A lightweight relational database engine built as the capstone project for the
**Advanced DBMS** course. MiniDB is intentionally small but covers the full
modern-DB stack: page storage, buffer pool, B+ tree indexes, SQL parser,
cost-based optimizer, executor, 2PL transactions, WAL-based recovery, and a
pluggable **extension track** (we are using Track B — MVCC, see below).

> Scope: implement what `docs/README.md` lists. Do **not** implement things that
> are not in this README or in the per-folder READMEs. Anything outside the
> listed contract is out of scope and should be left as a stub.

---

## 1. Project Overview

**Problem statement.** Build a working relational database engine from
foundational components and demonstrate understanding of database internals
(storage, indexing, query processing, transactions, recovery).

**Goals.**

- Implement the **required core features** listed in section 5 of the project
  brief (and nothing more).
- Produce a clean modular codebase so each of the 4 team members can work
  independently after the contracts are frozen.
- Provide a benchmark report and a live demo.

**Chosen extension track.** **Track B — Concurrency (MVCC).** We will replace
the 2PL-based lock manager with multi-version concurrency control. The lock
manager remains in the tree as a *baseline* (Track A comparison) but the
default isolation level will be MVCC snapshot isolation.

**Non-goals.**

- No networking, no client/server, no auth, no JDBC.
- No DDL beyond `CREATE TABLE` / `DROP TABLE` (catalog bootstrapping).
- No views, no subqueries, no aggregations beyond `COUNT/SUM/AVG/MIN/MAX`
  with `GROUP BY`.
- No stored procedures, no triggers, no UDFs.
- No data types beyond `INT`, `FLOAT`, `VARCHAR(n)`, `BOOL`.

---

## 2. Repository Layout

```text
minidb/
├── CMakeLists.txt                # Top-level build file (use this, see §11)
├── README.md                     # This file
│
├── docs/                         # Design docs - contract for the whole team
│   ├── README.md                 #   doc index
│   ├── architecture.md           #   module diagram + responsibilities
│   ├── interfaces.md             #   public C++ interfaces (THE contract)
│   ├── dataflow.md               #   request paths through the system
│   ├── benchmarks.md             #   what to measure and how
│   └── catalog.md                #   table metadata schema
│
├── include/                      # Public headers, one folder per module
│   ├── common/                   #   types shared by every module
│   ├── storage/                  #   page, disk, buffer pool, heap file
│   ├── index/                    #   B+ tree, node, index manager
│   ├── catalog/                  #   schema, table info, catalog manager
│   ├── parser/                   #   lexer, parser, AST
│   ├── planner/                  #   logical + physical plan, optimizer
│   ├── executor/                 #   seq scan, index scan, join, ins, del
│   ├── transaction/              #   txn, lock manager, txn manager
│   └── recovery/                 #   WAL, log record, recovery manager
│
├── src/                          # Implementations (one .cpp per header)
│   └── ...                        #   mirror of include/ structure
│
├── tests/                        # One test binary per module
│   └── ...                        #   mirrors include/ structure
│
├── benchmark/                    # Standalone perf programs (not in ctest)
│   ├── read_benchmark.cpp
│   ├── write_benchmark.cpp
│   └── join_benchmark.cpp
│
├── data/                         # Runtime data (gitignored contents)
│   ├── pages/                    #   heap + index pages
│   ├── wal/                      #   write-ahead log segments
│   └── metadata/                 #   catalog snapshot
│
├── bin/                          # (legacy target dir - actual build goes to build/)
│
└── build/                        # CMake build tree (created by `cmake -B`)
```

---

## 3. Module Map (one-line summary)

| Module | Folder | What it owns | Who needs it |
|---|---|---|---|
| **Common** | `include/common` | `PageId`, `TxnId`, `RecordId`, `Key`, `Status` | Everyone |
| **Storage** | `include/storage` | `Page`, `DiskManager`, `BufferPool`, `HeapFile` | Catalog, Index, Executor, Recovery |
| **Catalog** | `include/catalog` | `Schema`, `TableInfo`, `CatalogManager` | Planner, Executor, Index |
| **Index** | `include/index` | `BPlusTree`, `Node`, `IndexManager` | Executor (IndexScan) |
| **Parser** | `include/parser` | `Lexer`, `Parser`, `AST` | CLI |
| **Planner** | `include/planner` | `LogicalPlan`, `PhysicalPlan`, `Optimizer` | Executor |
| **Executor** | `include/executor` | `SeqScan`, `IndexScan`, `Join`, `InsertExec`, `DeleteExec` | CLI, Benchmarks |
| **Transaction** | `include/transaction` | `Transaction`, `LockManager`, `TransactionManager` | Executor, Recovery |
| **Recovery** | `include/recovery` | `WAL`, `LogRecord`, `RecoveryManager` | Startup, Transaction |

> **Rule of the road.** Cross-module calls go *only* through the interfaces in
> `docs/interfaces.md`. Reaching into another module's private members (e.g.
> `bplus.rootNode.children[0]`) is a merge-conflict generator and is banned.
> Each module's `README.md` lists the public functions it exposes — call those
> and only those.

---

## 4. Storage Layer (summary — see `include/storage/README.md`)

- **Page** — fixed-size (4 KB) byte buffer with a slot directory.
- **DiskManager** — read/write/allocate/free pages on disk.
- **BufferPool** — caches pages in memory, LRU eviction, page-level pinning.
- **HeapFile** — slotted pages, free-page list, `RecordId` = (pageId, slotIdx).

---

## 5. Indexing (summary — see `include/index/README.md`)

- **B+ Tree** with leaf-level sibling pointers for range scans.
- **IndexManager** — creates/drops/opens indexes for a table+column.
- Operations: `search`, `insert`, `remove`, `rangeScan`.

---

## 6. Query Execution (summary — see `include/{parser,planner,executor}/README.md`)

- **Parser** — hand-written recursive-descent. Supports `SELECT` (with
  `WHERE`, `JOIN`, `GROUP BY`, `ORDER BY`, `LIMIT`), `INSERT`, `DELETE`,
  `CREATE TABLE`, `DROP TABLE`, `BEGIN`/`COMMIT`/`ROLLBACK`.
- **Planner** — produces a `LogicalPlan` tree, then the optimizer produces a
  `PhysicalPlan` tree.
- **Optimizer** — selectivity estimation, choice of seq vs index scan, join
  order (left-deep).
- **Executor** — Volcano-style `Init/Next/Close` operators.

---

## 7. Optimizer (summary — see `include/planner/README.md`)

- Cost = `pagesRead * w_io + cpu_cost`.
- Selectivity: uniform assumption with optional histogram hooks (stubs OK).
- Rules: push down selections, choose index when `|R|` is small, single-level
  join reorder by estimated cardinality.

---

## 8. Transactions & Concurrency (summary — see `include/transaction/README.md`)

- **Baseline:** Strict 2PL, deadlock detection via wait-for graph, S/X locks
  on RecordIds. Used for the benchmark comparison.
- **Track B default:** MVCC, snapshot isolation, write-write conflict
  detection at commit time. `LockManager` is kept for the baseline run.
- Isolation guarantee: `Serializable` under 2PL, `Snapshot Isolation` under
  MVCC (we will document SI anomalies in the report).

---

## 9. Recovery (summary — see `include/recovery/README.md`)

- **WAL** with `LogRecord` kinds: `BEGIN`, `COMMIT`, `ABORT`, `INSERT`,
  `UPDATE`, `DELETE`, `CHECKPOINT`.
- **ARIES-style** recovery: analysis → redo → undo.
- Pages are flushed lazily; the WAL is `fsync`'d **before** a page is written
  to disk (write-ahead rule).

---

## 10. Extension Track — MVCC (Track B)

**Motivation.** 2PL is simple but causes read-write blocking; modern OLTP
workloads want non-blocking reads. MVCC gives readers a consistent snapshot
without acquiring read locks.

**Design (high level).**

- Each row version carries `(created_txn, deleted_txn)` columns.
- A reader's snapshot is the set of txn ids active when the reader began.
- Writers append new versions; old versions are garbage-collected by a
  background vacuum (can be a stub for the demo).
- Write-write conflicts resolved at commit using first-updater-wins.

**Benchmark.** Compare TPC-C-like workload (or our `write_benchmark.cpp`)
between the 2PL baseline and MVCC. Report throughput at varying
contention levels.

---

## 11. How to Build

### Prerequisites

- **CMake ≥ 3.16**
- A C++17 compiler:
  - Windows: MSVC 2019+, or MinGW-w64 (the `g++.exe` in `C:\msys64\ucrt64\bin`)
  - Linux/macOS: `g++` or `clang++`

### First-time setup (Windows, MinGW)

```bash
# from inside this directory
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/minidb
```

### First-time setup (Windows, MSVC)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\bin\Release\minidb.exe
```

### If CMake isn't installed

The project also builds without CMake using any C++20 compiler:

```bash
# Linux / macOS / MinGW
find src -name "*.cpp" -not -path "src/cli/*" | xargs g++ -std=c++20 -Iinclude -c
ar rcs libminidb_core.a *.o
g++ -std=c++20 -Iinclude -o minidb src/cli/main.cpp -L. -lminidb_core

# tests
for d in storage index parser executor transaction recovery; do
  g++ -std=c++20 -Iinclude -o "test_$d" "tests/$d/${d}_test.cpp" -L. -lminidb_core
done
```

The library + CLI + all six tests compile and link from day 1 with
**zero warnings** under `-Wall -Wextra` using g++ 15 (verified at
project freeze). Every public function in every module is a stub that
returns `Status::UNIMPLEMENTED` until implemented.

### Run tests

```bash
ctest --test-dir build --output-on-failure
```

### Run a benchmark

```bash
./build/benchmarks/bench_read_benchmark
./build/benchmarks/bench_write_benchmark
./build/benchmarks/bench_join_benchmark
```

### Quick demo

```bash
# Interactive REPL
./build/bin/minidb

# Or run a SQL script
cat > demo.sql <<'EOF'
CREATE TABLE users (id INT, name VARCHAR(64), age INT);
INSERT INTO users VALUES (1, 'alice', 30);
SELECT * FROM users;
EOF
./build/bin/minidb demo.sql
```

---

## 12. Team Workflow

1. **Day 1 — freeze the contracts.** The interfaces in `docs/interfaces.md`
   and the per-folder READMEs are the source of truth. Any change requires
   a team-wide ack.
2. **Module ownership.** Each team member owns one or more `include/<module>`
   folders. You implement the `.cpp` files under `src/<module>` and the tests
   under `tests/<module>`. You do *not* edit other modules' source.
3. **Stub everything else first.** Every header in `include/` already has
   a matching `.cpp` in `src/` that compiles (functions return `UNIMPLEMENTED`
   or sensible defaults). This means the project builds end-to-end from day 1.
4. **Branch per feature.** Branch off `main` as `feat/<module>-<thing>`,
   open a PR, get one review, merge.
5. **No cross-module reach-throughs.** If you need something from another
   module, add it to *its* `README.md` "Public API" list and ping the owner.

---

## 13. Limitations & Future Work

(Filled in during M5. Initial non-goals: see §1.)

- No query optimizer statistics beyond uniform + simple histograms.
- No `ALTER TABLE`.
- Vacuum/GC is single-threaded and best-effort.
- WAL is single-segment file (no log archiving).
- No replication (would be Track D).
