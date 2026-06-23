// =============================================================================
// include/executor/seq_scan.h
// -----------------------------------------------------------------------------
// SeqScanExecutor: walks every record in a HeapFile via HeapFile::scan().
// Optionally filters with `predicate` (nullptr = pass-through).
// =============================================================================
#pragma once

#include <memory>

#include "executor/executor.h"
#include "parser/ast.h"
#include "storage/heap_file.h"

namespace minidb::executor {

class SeqScanExecutor : public Executor {
public:
    SeqScanExecutor(ExecutorContext* ctx,
                    std::string table,
                    std::unique_ptr<parser::Expr> predicate);
    ~SeqScanExecutor() override;

    Status init() override;
    Status next (Tuple& out) override;
    Status close() override;

private:
    std::string                                table_;
    std::unique_ptr<parser::Expr>              predicate_;
    const catalog::TableInfo*                  info_ = nullptr;
    std::unique_ptr<storage::HeapFile>         file_;
    std::unique_ptr<storage::HeapFile::Iterator> it_;
};

} // namespace minidb::executor