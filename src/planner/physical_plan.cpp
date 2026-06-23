// =============================================================================
// src/planner/physical_plan.cpp
// -----------------------------------------------------------------------------
// PhysicalPlan is a value type like LogicalPlan. This TU provides:
//   - clonePhysicalPlan: deep-copy helper for the optimizer / engine.
//   - kindName / physicalPlanToString: pretty-printers used by EXPLAIN and
//     by error messages when the executor rejects a plan shape.
// =============================================================================
#include "planner/physical_plan.h"

#include <sstream>

namespace minidb::planner {

namespace {

// Recursive deep-copy of an Expr. parser::Expr owns its children through
// unique_ptr, so we need to allocate fresh Expr nodes at every level.
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

// Build an Expr by value (move-only).
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

std::unique_ptr<PhysicalPlan> clonePlan(const PhysicalPlan& p) {
    auto out = std::make_unique<PhysicalPlan>();
    out->kind      = p.kind;
    out->table     = p.table;
    out->indexName = p.indexName;
    if (p.predicate) {
        out->predicate = cloneExpr(*p.predicate);
    }
    for (const auto& c : p.children) {
        if (c) out->children.push_back(clonePlan(*c));
        else   out->children.push_back(nullptr);
    }
    out->outputColumns = p.outputColumns;
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
    return out;
}

} // anonymous namespace

const char* kindName(PhysicalKind k) {
    switch (k) {
        case PhysicalKind::SEQ_SCAN:         return "SEQ_SCAN";
        case PhysicalKind::INDEX_SCAN:       return "INDEX_SCAN";
        case PhysicalKind::FILTER:           return "FILTER";
        case PhysicalKind::PROJECT:          return "PROJECT";
        case PhysicalKind::NESTED_LOOP_JOIN: return "NESTED_LOOP_JOIN";
        case PhysicalKind::HASH_JOIN:        return "HASH_JOIN";
        case PhysicalKind::AGGREGATE:        return "AGGREGATE";
        case PhysicalKind::SORT:             return "SORT";
        case PhysicalKind::LIMIT:            return "LIMIT";
        case PhysicalKind::INSERT:           return "INSERT";
        case PhysicalKind::DELETE:           return "DELETE";
    }
    return "UNKNOWN";
}

std::string physicalPlanToString(const PhysicalPlan& p) {
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
        os << "\n  -> " << physicalPlanToString(*c);
    }
    return os.str();
}

} // namespace minidb::planner