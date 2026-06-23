// =============================================================================
// include/executor/delete_executor.h
// -----------------------------------------------------------------------------
// DeleteExecutor: scans matching rows (via an IndexScan or SeqScan+Filter),
// deletes from the heap file, removes from every index, logs WAL entries.
// =============================================================================
#pragma once

#include <memory>

#include "executor/executor.h"
#include "parser/ast.h"

namespace minidb::executor {

class DeleteExecutor : public Executor {
public:
    DeleteExecutor(ExecutorContext* ctx, std::unique_ptr<parser::DeleteStmt> stmt);
    ~DeleteExecutor() override;

    Status init() override;
    Status next (Tuple& out) override;
    Status close() override;

private:
    std::unique_ptr<parser::DeleteStmt> stmt_;
    std::unique_ptr<Executor>           child_;   // scan that produces the victims
};

} // namespace minidb::executor