# `executor/` — physical operators (Volcano)

Each `PhysicalPlan` node has a matching executor class. The query engine
walks the plan top-down calling `init()`, then loops `next()` until it
returns `DONE`, then `close()`.

## Files in this folder

| Header | Volcano class |
|---|---|
| `include/executor/executor.h`         | `ExecutorContext`, `Tuple`, `Status` re-export, `init/next/close` contract. |
| `include/executor/seq_scan.h`         | `SeqScanExecutor`. |
| `include/executor/index_scan.h`       | `IndexScanExecutor`. |
| `include/executor/join.h`             | `NestedLoopJoinExecutor`, `HashJoinExecutor`. |
| `include/executor/insert_executor.h`  | `InsertExecutor`. |
| `include/executor/delete_executor.h`  | `DeleteExecutor`. |
| `include/executor/query_engine.h`     | `QueryEngine` façade — the CLI calls this. |

## Volcano contract

```cpp
class Executor {
public:
    Status init();                 // open child executors, allocate state
    Status next(Tuple& out);       // returns OK with tuple, or DONE
    Status close();                // release resources, close children
};
```

`Tuple` is a `std::vector<std::optional<Value>>` where `Value` is a tagged
union over `int32_t, float, std::string, bool, std::monostate` (NULL).

## Public API (contract)

```cpp
namespace minidb::executor {

struct Value {
    enum class Tag { INT, FLOAT, STRING, BOOL, NULL_ };
    Tag tag = Tag::NULL_;
    int32_t     i = 0;
    float       f = 0.0f;
    std::string s;
    bool        b = false;
};

struct Tuple {
    std::vector<Value> values;
};

class QueryEngine {
public:
    QueryEngine(storage::BufferPool* bp,
                catalog::CatalogManager* cat,
                index::IndexManager* idx,
                transaction::TransactionManager* txn,
                recovery::RecoveryManager* rec);

    std::vector<Tuple> execute(const std::string& sql);   // for SELECT
    Status             executeUpdate(const std::string& sql); // for INSERT/DELETE/CREATE/DROP
};

class SeqScanExecutor      { /* init/next/close */ };
class IndexScanExecutor    { /* init/next/close */ };
class FilterExecutor       { /* init/next/close */ };
class ProjectExecutor      { /* init/next/close */ };
class NestedLoopJoinExecutor { /* init/next/close */ };
class HashJoinExecutor     { /* init/next/close */ };
class AggregateExecutor    { /* init/next/close */ };
class SortExecutor         { /* init/next/close */ };
class LimitExecutor        { /* init/next/close */ };
class InsertExecutor       { /* init/next/close */ };
class DeleteExecutor       { /* init/next/close */ };

} // namespace minidb::executor
```

## How other modules use the executor

| Module | Calls |
|---|---|
| `cli/main.cpp` | `QueryEngine::execute` / `executeUpdate` per user line. |
| `benchmark/*.cpp` | `QueryEngine::execute` for steady-state timing. |
| `transaction/TransactionManager` | the executor calls into it (not the other way around) to begin/commit/abort. |
| `recovery/RecoveryManager` | at startup, *before* the executor is reachable, runs analysis/redo/undo. |

## Rules

- Executors hold borrowed pointers to `BufferPool`, `CatalogManager`, etc.
  through the `ExecutorContext`. They do **not** own them.
- An executor never calls `BufferPool::unpinPage` for a page it didn't
  pin. The `HeapFile` Iterator is the canonical example.
- Filter / Project / Aggregate / Sort / Limit are pipeline-friendly
  (push-style). Hash join materialises its build side; nested-loop join
  does not.
- The executor is **schema-aware**: it reads `TableInfo` from the
  catalog to know how to materialise a `Tuple`.