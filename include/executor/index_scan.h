// =============================================================================
// include/executor/index_scan.h
// -----------------------------------------------------------------------------
// IndexScanExecutor: looks up a key (or range) in an index, fetches the
// matching rows from the heap file, materialises a Tuple per row.
// =============================================================================
#pragma once

#include <memory>

#include "executor/executor.h"
#include "index/bplus_tree.h"
#include "parser/ast.h"

namespace minidb::executor {

class IndexScanExecutor : public Executor {
public:
    IndexScanExecutor(ExecutorContext* ctx,
                      std::string table,
                      std::string indexName,
                      std::unique_ptr<parser::Expr> predicate);
    ~IndexScanExecutor() override;

    Status init() override;
    Status next (Tuple& out) override;
    Status close() override;

private:
    std::string                    table_;
    std::string                    indexName_;
    std::unique_ptr<parser::Expr>  predicate_;
    const catalog::TableInfo*      info_ = nullptr;
    index::BPlusTree*              tree_ = nullptr;
    std::vector<RecordId>          rids_;
    std::size_t                   cursor_ = 0;
};

} // namespace minidb::executor