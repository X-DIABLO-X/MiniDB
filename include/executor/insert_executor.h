// =============================================================================
// include/executor/insert_executor.h
// -----------------------------------------------------------------------------
// InsertExecutor: takes a parsed InsertStmt, builds a row tuple, calls into
// the storage layer and the index manager, logs a WAL INSERT record.
// =============================================================================
#pragma once

#include <memory>

#include "executor/executor.h"
#include "parser/ast.h"
#include "storage/heap_file.h"

namespace minidb::executor {

class InsertExecutor : public Executor {
public:
    InsertExecutor(ExecutorContext* ctx, std::unique_ptr<parser::InsertStmt> stmt);
    ~InsertExecutor() override;

    Status init() override;
    Status next (Tuple& out) override;   // one tuple per call (DONE after all rows)
    Status close() override;

private:
    std::unique_ptr<parser::InsertStmt> stmt_;
    std::size_t                         rowIdx_ = 0;
    const catalog::TableInfo*           info_   = nullptr;
    std::unique_ptr<storage::HeapFile>  file_;
};

} // namespace minidb::executor