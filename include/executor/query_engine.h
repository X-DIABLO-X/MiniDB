// =============================================================================
// include/executor/query_engine.h
// -----------------------------------------------------------------------------
// QueryEngine: the façade the CLI and benchmarks call. Hands off SQL to
// the parser, optimizer, and executor pipeline.
// =============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "executor/executor.h"
#include "recovery/recovery_manager.h"

namespace minidb::planner  { class Optimizer; }

namespace minidb::executor {

class QueryEngine {
public:
    QueryEngine(storage::BufferPool*               bp,
                catalog::CatalogManager*           cat,
                index::IndexManager*               idx,
                transaction::TransactionManager*   txn,
                recovery::RecoveryManager*         rec);

    ~QueryEngine();

    QueryEngine(const QueryEngine&)            = delete;
    QueryEngine& operator=(const QueryEngine&) = delete;

    // For SELECT. Returns all matching tuples.
    std::vector<Tuple> execute(const std::string& sql);

    // For INSERT / DELETE / CREATE / DROP / BEGIN / COMMIT / ROLLBACK.
    Status executeUpdate(const std::string& sql);

private:
    ExecutorContext           ctx_;
    std::unique_ptr<planner::Optimizer> optimizer_;
    // executors are constructed per call, not stored.
};

} // namespace minidb::executor