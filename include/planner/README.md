# `planner/` — logical & physical plans, optimizer

The planner turns a `parser::Stmt` into a tree of `PhysicalPlan` nodes that
the executor can run directly. The optimizer is the cost-based decision
maker that picks scan vs index scan and join order.

## Files in this folder

| Header | Responsibility |
|---|---|
| `include/planner/logical_plan.h`  | Relational-algebra nodes: `Scan`, `Filter`, `Project`, `Join`, `Aggregate`, `Sort`, `Limit`, `Insert`, `Delete`. |
| `include/planner/physical_plan.h` | Volcano operator tree (one class per operator). |
| `include/planner/optimizer.h`     | `Optimizer` — converts a `Stmt` (via a `LogicalPlan`) into a `PhysicalPlan`. |

## Public API (contract)

```cpp
namespace minidb::planner {

// Logical plan node kinds (relational algebra).
enum class LogicalKind { SCAN, INDEX_SCAN, FILTER, PROJECT, NESTED_LOOP_JOIN,
                         HASH_JOIN, AGGREGATE, SORT, LIMIT, INSERT, DELETE };

struct LogicalPlan {
    LogicalKind                          kind;
    std::string                          table;
    std::unique_ptr<parser::Expr>        predicate;
    std::vector<std::unique_ptr<LogicalPlan>> children;
    std::vector<std::string>             outputColumns;
    std::vector<parser::Expr>            groupBy;
    std::vector<parser::Expr>            orderBy;
    bool orderDesc = false;
    int  limit = -1;
};

// Physical plan node kinds.
enum class PhysicalKind { SEQ_SCAN, INDEX_SCAN, FILTER, PROJECT,
                          NESTED_LOOP_JOIN, HASH_JOIN, AGGREGATE, SORT,
                          LIMIT, INSERT, DELETE };

struct PhysicalPlan {
    PhysicalKind kind;
    std::string table;
    std::string indexName;            // INDEX_SCAN only
    std::unique_ptr<parser::Expr> predicate;
    std::vector<std::unique_ptr<PhysicalPlan>> children;
    std::vector<std::string> outputColumns;
    std::vector<parser::Expr> groupBy;
    std::vector<parser::Expr> orderBy;
    bool orderDesc = false;
    int  limit = -1;
};

class Optimizer {
public:
    Optimizer(catalog::CatalogManager* cat,
              index::IndexManager*     idx,
              transaction::TransactionManager* txn);

    std::unique_ptr<PhysicalPlan> optimize(const parser::Stmt& s);
};

}
```

## Cost model

- `cost(op) = pagesRead(op) * w_io + cpuCost(op) * w_cpu`
- `w_io = 100`, `w_cpu = 1` (so an extra page read is worth 100 CPU ops).
- `pagesRead(SeqScan(t))  = ceil(|t| * rowSize / pageSize)`.
- `pagesRead(IndexScan)   = ceil(log_F(|t|)) + 1`  (root-to-leaf + 1 page fetch for the row).
- `cpuCost(SeqScan)        = |t|`.
- `cpuCost(IndexScan)      = log_F(|t|) + 1`.

## Optimizer rules (v1)

1. **Predicate pushdown** — for every `SELECT … WHERE a.x = c`, push the
   predicate below the join it belongs to.
2. **Scan selection** — if a predicate is on a column that has an index and
   the predicate is `=`, `<`, `<=`, `>`, `>=`, or `BETWEEN`, prefer
   `IndexScan` over `SeqScan + Filter`.
3. **Join order** — for two-way joins, drive from the smaller estimated
   relation (left-deep).
4. **Projection pushdown** — keep only the columns the user asked for
   (passes through to the executor).

## How other modules use the planner

| Module | Calls |
|---|---|
| `executor/QueryEngine` | `Optimizer::optimize(stmt)` then walks the `PhysicalPlan`. |

## Rules

- The planner is the **only** module that talks to `CatalogManager` to
  validate column names.
- The planner never opens pages; it works on metadata only.
- The physical plan is a **value type** — no shared state between
  optimisations. The executor can re-walk it freely.