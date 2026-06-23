// =============================================================================
// src/planner/logical_plan.cpp
// -----------------------------------------------------------------------------
// LogicalPlan is a value type (POD-ish tree of relational-algebra nodes).
// This TU provides:
//   - cloneLogicalPlan: deep-copy helper used by the optimizer when it
//     rewrites a node into a slightly different shape.
//   - kindName/logicalPlanToString: pretty-printers used by EXPLAIN and by
//     the error path in QueryEngine.
// =============================================================================
#include "planner/logical_plan.h"

#include <sstream>
#include <utility>

namespace minidb::planner {

namespace {

// Recursively clone an Expr. parser::Expr owns its children via
// unique_ptr, so a deep copy needs to allocate fresh Expr objects.
std::unique_ptr<parser::Expr> cloneExpr(const parser::Expr& e) {
    auto out = std::make_unique<parser::Expr>();
    out->kind    = e.kind;
    out->text    = e.text;
    out->op      = e.op;
    out->intVal  = e.intVal;
    out->floatVal = e.floatVal;
    out->boolVal = e.boolVal;
    out->strVal  = e.strVal;
    out->line    = e.line;
    out->col     = e.col;
    for (const auto& a : e.args) {
        if (a) out->args.push_back(cloneExpr(*a));
        else   out->args.push_back(nullptr);
    }
    return out;
}

// Build an Expr by value. parser::Expr is move-only (it owns unique_ptr
// children), so we return an rvalue suitable for push_back into
// vector<Expr>.
parser::Expr cloneExprValue(const parser::Expr& e) {
    parser::Expr out;
    out.kind    = e.kind;
    out.text    = e.text;
    out.op      = e.op;
    out.intVal  = e.intVal;
    out.floatVal = e.floatVal;
    out.boolVal = e.boolVal;
    out.strVal  = e.strVal;
    out.line    = e.line;
    out.col     = e.col;
    for (const auto& a : e.args) {
        if (a) out.args.push_back(cloneExpr(*a));
        else   out.args.push_back(nullptr);
    }
    return out;
}

// Recursively clone a LogicalPlan tree. Both predicate and children need
// fresh allocations because the optimizer may keep both the old and new
// trees alive across a rewrite.
std::unique_ptr<LogicalPlan> clonePlan(const LogicalPlan& p) {
    auto out  = std::make_unique<LogicalPlan>();
    out->kind          = p.kind;
    out->table         = p.table;
    out->indexName     = p.indexName;
    if (p.predicate) {
        out->predicate = cloneExpr(*p.predicate);
    }
    for (const auto& c : p.children) {
        if (c) out->children.push_back(clonePlan(*c));
        else   out->children.push_back(nullptr);
    }
    out->outputColumns = p.outputColumns;
    // vector<Expr> is move-only, so push by-value (moved from clone).
    out->groupBy.reserve(p.groupBy.size());
    for (const auto& e : p.groupBy) {
        out->groupBy.push_back(cloneExprValue(e));
    }
    out->orderBy.reserve(p.orderBy.size());
    for (const auto& e : p.orderBy) {
        out->orderBy.push_back(cloneExprValue(e));
    }
    out->orderDesc     = p.orderDesc;
    out->limit         = p.limit;
    out->estCost       = p.estCost;
    out->estRows       = p.estRows;
    return out;
}

} // anonymous namespace

const char* kindName(LogicalKind k) {
    switch (k) {
        case LogicalKind::SCAN:             return "SCAN";
        case LogicalKind::INDEX_SCAN:       return "INDEX_SCAN";
        case LogicalKind::FILTER:           return "FILTER";
        case LogicalKind::PROJECT:          return "PROJECT";
        case LogicalKind::NESTED_LOOP_JOIN: return "NESTED_LOOP_JOIN";
        case LogicalKind::HASH_JOIN:        return "HASH_JOIN";
        case LogicalKind::AGGREGATE:        return "AGGREGATE";
        case LogicalKind::SORT:             return "SORT";
        case LogicalKind::LIMIT:            return "LIMIT";
        case LogicalKind::INSERT:           return "INSERT";
        case LogicalKind::DELETE:           return "DELETE";
    }
    return "UNKNOWN";
}

std::string logicalPlanToString(const LogicalPlan& p) {
    std::ostringstream os;
    os << kindName(p.kind);
    if (!p.table.empty())     os << "(" << p.table << ")";
    if (!p.indexName.empty()) os << "[idx=" << p.indexName << "]";
    if (!p.outputColumns.empty()) {
        os << " out=[";
        for (std::size_t i = 0; i < p.outputColumns.size(); ++i) {
            if (i) os << ",";
            os << p.outputColumns[i];
        }
        os << "]";
    }
    if (p.predicate) {
        os << " where=" << parser::toString(*p.predicate);
    }
    if (p.limit >= 0) os << " limit=" << p.limit;
    for (const auto& c : p.children) {
        if (!c) continue;
        os << "\n  -> " << logicalPlanToString(*c);
    }
    return os.str();
}

} // namespace minidb::planner