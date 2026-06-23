// =============================================================================
// include/planner/optimizer.h
// -----------------------------------------------------------------------------
// Optimizer: parser::Stmt -> LogicalPlan -> PhysicalPlan.
//
// The cost model and rules are in include/planner/README.md.
// =============================================================================
#pragma once

#include <memory>

#include "parser/ast.h"
#include "planner/logical_plan.h"
#include "planner/physical_plan.h"

namespace minidb::catalog  { class CatalogManager; }
namespace minidb::index    { class IndexManager; }
namespace minidb::transaction { class TransactionManager; }

namespace minidb::planner {

class Optimizer {
public:
    Optimizer(catalog::CatalogManager*       cat,
              index::IndexManager*           idx,
              transaction::TransactionManager* txn);

    Optimizer(const Optimizer&)            = delete;
    Optimizer& operator=(const Optimizer&) = delete;

    std::unique_ptr<PhysicalPlan> optimize(const parser::Stmt& s);

private:
    catalog::CatalogManager*         cat_;
    index::IndexManager*             idx_;
    transaction::TransactionManager* txn_;

    std::unique_ptr<LogicalPlan>  toLogical(const parser::Stmt& s);
    std::unique_ptr<PhysicalPlan> toPhysical(std::unique_ptr<LogicalPlan> in);

    double estimateCost(const LogicalPlan& p) const;
    double estimateRows(const LogicalPlan& p) const;
};

} // namespace minidb::planner