// =============================================================================
// include/planner/logical_plan.h
// -----------------------------------------------------------------------------
// Logical-plan node types. A LogicalPlan is a tree of relational-algebra
// operations produced by the parser-AST -> logical translation step.
// =============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "parser/ast.h"

namespace minidb::planner {

enum class LogicalKind {
    SCAN, INDEX_SCAN, FILTER, PROJECT,
    NESTED_LOOP_JOIN, HASH_JOIN,
    AGGREGATE, SORT, LIMIT,
    INSERT, DELETE
};

struct LogicalPlan {
    LogicalKind                              kind = LogicalKind::SCAN;
    std::string                              table;
    std::string                              indexName;       // INDEX_SCAN only
    std::unique_ptr<parser::Expr>            predicate;
    std::vector<std::unique_ptr<LogicalPlan>> children;
    std::vector<std::string>                 outputColumns;   // empty = "*"
    std::vector<parser::Expr>                groupBy;
    std::vector<parser::Expr>                orderBy;
    bool                                     orderDesc = false;
    int                                      limit = -1;

    // Cost estimates (filled in by the optimizer, used for plan comparison).
    double estCost     = 0.0;     // in cost units
    double estRows     = 0.0;     // output cardinality
};

} // namespace minidb::planner