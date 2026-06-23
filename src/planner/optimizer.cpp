// =============================================================================
// src/planner/optimizer.cpp
// -----------------------------------------------------------------------------
// Optimizer implementation. See include/planner/README.md for the cost model
// and the four v1 rules:
//   1. predicate pushdown
//   2. scan selection (seq vs index)
//   3. join order (left-deep, smaller build side)
//   4. projection pushdown
// =============================================================================
#include "planner/optimizer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog/catalog_manager.h"
#include "catalog/schema.h"
#include "catalog/table_info.h"
#include "index/index_manager.h"
#include "transaction/transaction.h"

namespace minidb::planner {

namespace {

// Cost model weights. One extra page read is worth 100 CPU ops.
constexpr double W_IO  = 100.0;
constexpr double W_CPU = 1.0;
constexpr double PAGE_SIZE = 4096.0;

// Default fanout assumed for the B+ tree when we don't know the exact
// value. Used by estimateCost for index access.
constexpr double INDEX_FANOUT = 64.0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Heuristic row-size estimate. If we know the table's schema, use that;
// otherwise assume 64 bytes per row.
double estimateRowSize(const catalog::CatalogManager* cat,
                       const std::string& table) {
    if (cat == nullptr) return 64.0;
    const auto* info = cat->getTable(table);
    if (info == nullptr) return 64.0;
    double sz = static_cast<double>(info->schema.rowSize());
    if (sz <= 0.0) sz = 64.0;
    return sz;
}

// Pull the qualifier from a column reference. parser::Expr stores column
// refs as ExprKind::COLUMN with `text` = "name" or "table.name". Returns
// the qualifier (everything before the dot) or empty when there is none.
std::string columnQualifier(const parser::Expr& e) {
    if (e.kind != parser::ExprKind::COLUMN) return {};
    auto dot = e.text.find('.');
    if (dot == std::string::npos) return {};
    return e.text.substr(0, dot);
}

// Bare column name (the part after the dot, or the whole string if
// un-qualified).
std::string columnName(const parser::Expr& e) {
    if (e.kind != parser::ExprKind::COLUMN) return {};
    auto dot = e.text.find('.');
    if (dot == std::string::npos) return e.text;
    return e.text.substr(dot + 1);
}

// Recursively deep-clone an Expr into a unique_ptr.
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

// Build an Expr by value (move-only). Returns an rvalue.
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

// ---------------------------------------------------------------------------
// Predicate classification
// ---------------------------------------------------------------------------

// True if the Expr is a simple binary comparison of the form
// <column> <op> <literal> where op is one of the recognised comparators.
struct SimplePredicate {
    std::string table;        // empty if column is unqualified
    std::string column;
    std::string op;           // "=", "<", "<=", ">", ">=", "BETWEEN"
    bool        ok = false;
};

SimplePredicate classifySimplePredicate(const parser::Expr& e) {
    SimplePredicate out;
    if (e.kind != parser::ExprKind::BINARY_OP) return out;
    out.op = e.op;
    if (out.op != "=" && out.op != "<" && out.op != "<=" &&
        out.op != ">" && out.op != ">=" && out.op != "BETWEEN") {
        return out;
    }
    if (e.args.size() != 2) return out;

    const auto* lhs = e.args[0].get();
    const auto* rhs = e.args[1].get();
    if (lhs == nullptr || rhs == nullptr) return out;

    // Both sides must be simple — column or literal. No function calls.
    auto isSimple = [](const parser::Expr* x) {
        if (x == nullptr) return false;
        return x->kind == parser::ExprKind::COLUMN ||
               x->kind == parser::ExprKind::INT_LIT ||
               x->kind == parser::ExprKind::FLOAT_LIT ||
               x->kind == parser::ExprKind::STR_LIT ||
               x->kind == parser::ExprKind::BOOL_LIT;
    };
    if (!isSimple(lhs) || !isSimple(rhs)) return out;

    const parser::Expr* col = nullptr;
    if (lhs->kind == parser::ExprKind::COLUMN) col = lhs;
    else if (rhs->kind == parser::ExprKind::COLUMN) col = rhs;
    else return out;

    out.table  = columnQualifier(*col);
    out.column = columnName(*col);
    out.ok     = !out.column.empty();
    return out;
}

// Walk an Expr tree and collect the set of table qualifiers it references.
void collectReferencedTables(const parser::Expr& e,
                             const std::string& defaultTable,
                             std::vector<std::string>& out) {
    if (e.kind == parser::ExprKind::COLUMN) {
        std::string t = columnQualifier(e);
        if (t.empty()) t = defaultTable;
        if (!t.empty()) out.push_back(t);
        return;
    }
    for (const auto& a : e.args) {
        if (a) collectReferencedTables(*a, defaultTable, out);
    }
}

// ---------------------------------------------------------------------------
// Logical-plan construction from parser::Stmt
// ---------------------------------------------------------------------------

// Build the source-relation subtree for a SELECT. Handles the
// single-table and two-table-join shapes the parser produces.
std::unique_ptr<LogicalPlan> buildFrom(const parser::SelectStmt& s) {
    auto left = std::make_unique<LogicalPlan>();
    left->kind  = LogicalKind::SCAN;
    left->table = s.fromTable;

    if (s.joinTable.empty()) {
        return left;
    }

    auto right = std::make_unique<LogicalPlan>();
    right->kind  = LogicalKind::SCAN;
    right->table = s.joinTable;

    auto join = std::make_unique<LogicalPlan>();
    join->kind = LogicalKind::NESTED_LOOP_JOIN;
    if (s.joinOn) join->predicate = cloneExpr(*s.joinOn);
    join->children.push_back(std::move(left));
    join->children.push_back(std::move(right));
    return join;
}

// Compose a SELECT into a logical pipeline. Shape:
//   source (Scan [+ Join])
//   -> optional Filter (WHERE)
//   -> optional Aggregate (GROUP BY / aggregates in projection)
//   -> optional Sort  (ORDER BY)
//   -> optional Limit (LIMIT)
//   -> Project (column list)
std::unique_ptr<LogicalPlan> buildSelect(const parser::SelectStmt& s) {
    auto cur = buildFrom(s);

    // Filter (WHERE).
    if (s.where) {
        auto flt = std::make_unique<LogicalPlan>();
        flt->kind      = LogicalKind::FILTER;
        flt->predicate = cloneExpr(*s.where);
        flt->children.push_back(std::move(cur));
        cur = std::move(flt);
    }

    // Aggregate (GROUP BY) — or any function call in the projection.
    bool hasAgg = !s.groupBy.empty();
    if (!hasAgg) {
        for (const auto& p : s.projection) {
            if (p && p->kind == parser::ExprKind::FUNCTION_CALL) {
                hasAgg = true;
                break;
            }
        }
    }
    if (hasAgg) {
        auto agg = std::make_unique<LogicalPlan>();
        agg->kind = LogicalKind::AGGREGATE;
        agg->groupBy.reserve(s.groupBy.size());
        for (const auto& g : s.groupBy) {
            if (g) agg->groupBy.push_back(cloneExprValue(*g));
        }
        agg->children.push_back(std::move(cur));
        cur = std::move(agg);
    }

    // Sort (ORDER BY).
    if (!s.orderBy.empty()) {
        auto sr = std::make_unique<LogicalPlan>();
        sr->kind      = LogicalKind::SORT;
        sr->orderDesc = s.orderDesc;
        sr->orderBy.reserve(s.orderBy.size());
        for (const auto& o : s.orderBy) {
            if (o) sr->orderBy.push_back(cloneExprValue(*o));
        }
        sr->children.push_back(std::move(cur));
        cur = std::move(sr);
    }

    // Limit.
    if (s.limit >= 0) {
        auto lm = std::make_unique<LogicalPlan>();
        lm->kind   = LogicalKind::LIMIT;
        lm->limit  = s.limit;
        lm->children.push_back(std::move(cur));
        cur = std::move(lm);
    }

    // Project (column list). Empty projection means "*".
    auto proj = std::make_unique<LogicalPlan>();
    proj->kind = LogicalKind::PROJECT;
    if (!s.projection.empty()) {
        for (const auto& p : s.projection) {
            if (p && p->kind == parser::ExprKind::COLUMN) {
                proj->outputColumns.push_back(columnName(*p));
            }
            // Function-call projections ride through untouched; the
            // executor handles them in its Project operator.
        }
    }
    proj->children.push_back(std::move(cur));
    return proj;
}

// ---------------------------------------------------------------------------
// Predicate-pushdown rewrite (Rule 1)
//
// For each Filter sitting above a Scan, move the predicate onto the Scan.
// For each Filter above a Join, split by referenced table and drop pieces
// closer to the data source. Cross-table predicates are kept on the join.
// ---------------------------------------------------------------------------

// Decide which side of a two-way join a predicate belongs to. Returns:
//   0 = left only, 1 = right only, 2 = both (cross-table).
int classifyByTable(const parser::Expr& e,
                    const std::string& leftTable,
                    const std::string& rightTable) {
    std::vector<std::string> tabs;
    collectReferencedTables(e, leftTable, tabs);
    if (tabs.empty()) return 0;
    bool hasL = false, hasR = false;
    for (const auto& t : tabs) {
        if (!leftTable.empty()  && t == leftTable)  hasL = true;
        if (!rightTable.empty() && t == rightTable) hasR = true;
    }
    if (hasL && hasR) return 2;
    if (hasL)         return 0;
    if (hasR)         return 1;
    return 0; // default to left when qualifiers can't be resolved.
}

// Combine a list of simple predicates with AND. Returns a single Expr
// that is a left-deep AND chain.
std::unique_ptr<parser::Expr>
andTogether(std::vector<parser::Expr> parts) {
    if (parts.empty()) return nullptr;
    auto cur = std::make_unique<parser::Expr>(std::move(parts.front()));
    parts.erase(parts.begin());
    for (auto& p : parts) {
        auto next = std::make_unique<parser::Expr>();
        next->kind = parser::ExprKind::BINARY_OP;
        next->op   = "AND";
        next->args.push_back(std::move(cur));
        next->args.push_back(std::make_unique<parser::Expr>(std::move(p)));
        cur = std::move(next);
    }
    return cur;
}

// Walk the predicate expression. For AND nodes, recurse and collect.
// Otherwise, classify as left-only / right-only / cross.
void collectSplit(const parser::Expr& e,
                  const std::string& leftTable,
                  const std::string& rightTable,
                  std::vector<parser::Expr>& left,
                  std::vector<parser::Expr>& right,
                  std::vector<parser::Expr>& cross) {
    if (e.kind == parser::ExprKind::BINARY_OP && e.op == "AND") {
        if (e.args.size() == 2 && e.args[0] && e.args[1]) {
            collectSplit(*e.args[0], leftTable, rightTable, left, right, cross);
            collectSplit(*e.args[1], leftTable, rightTable, left, right, cross);
            return;
        }
    }
    int cls = classifyByTable(e, leftTable, rightTable);
    if      (cls == 0) left.push_back(cloneExprValue(e));
    else if (cls == 1) right.push_back(cloneExprValue(e));
    else               cross.push_back(cloneExprValue(e));
}

std::unique_ptr<LogicalPlan>
pushdown(std::unique_ptr<LogicalPlan> node) {
    if (!node) return node;

    // Recurse into children first (bottom-up).
    for (auto& c : node->children) {
        c = pushdown(std::move(c));
    }

    if (node->kind != LogicalKind::FILTER || !node->predicate) {
        return node;
    }

    // Filter with a single Scan child — push predicate down and drop
    // the Filter.
    if (node->children.size() == 1 &&
        node->children[0] &&
        node->children[0]->kind == LogicalKind::SCAN) {
        // If the Scan already had a predicate, AND them.
        if (node->children[0]->predicate) {
            auto combined = std::make_unique<parser::Expr>();
            combined->kind = parser::ExprKind::BINARY_OP;
            combined->op   = "AND";
            combined->args.push_back(std::move(node->children[0]->predicate));
            combined->args.push_back(std::move(node->predicate));
            node->children[0]->predicate = std::move(combined);
        } else {
            node->children[0]->predicate = std::move(node->predicate);
        }
        return std::move(node->children[0]);
    }

    // Filter with a Join child — split per side.
    if (node->children.size() == 1 &&
        node->children[0] &&
        (node->children[0]->kind == LogicalKind::NESTED_LOOP_JOIN ||
         node->children[0]->kind == LogicalKind::HASH_JOIN)) {
        auto& join = node->children[0];
        std::string leftTable  = (join->children.size() > 0 && join->children[0])
                                 ? join->children[0]->table : std::string();
        std::string rightTable = (join->children.size() > 1 && join->children[1])
                                 ? join->children[1]->table : std::string();

        std::vector<parser::Expr> leftP, rightP, crossP;
        collectSplit(*node->predicate, leftTable, rightTable, leftP, rightP, crossP);

        // Attach left pieces onto the left scan.
        if (join->children.size() > 0 && join->children[0] &&
            join->children[0]->kind == LogicalKind::SCAN) {
            if (auto p = andTogether(std::move(leftP))) {
                if (join->children[0]->predicate) {
                    auto combined = std::make_unique<parser::Expr>();
                    combined->kind = parser::ExprKind::BINARY_OP;
                    combined->op   = "AND";
                    combined->args.push_back(std::move(join->children[0]->predicate));
                    combined->args.push_back(std::move(p));
                    join->children[0]->predicate = std::move(combined);
                } else {
                    join->children[0]->predicate = std::move(p);
                }
            }
        }
        // Attach right pieces onto the right scan.
        if (join->children.size() > 1 && join->children[1] &&
            join->children[1]->kind == LogicalKind::SCAN) {
            if (auto p = andTogether(std::move(rightP))) {
                if (join->children[1]->predicate) {
                    auto combined = std::make_unique<parser::Expr>();
                    combined->kind = parser::ExprKind::BINARY_OP;
                    combined->op   = "AND";
                    combined->args.push_back(std::move(join->children[1]->predicate));
                    combined->args.push_back(std::move(p));
                    join->children[1]->predicate = std::move(combined);
                } else {
                    join->children[1]->predicate = std::move(p);
                }
            }
        }
        // Cross-table pieces become an additional join predicate.
        if (!crossP.empty()) {
            if (auto p = andTogether(std::move(crossP))) {
                if (join->predicate) {
                    auto combined = std::make_unique<parser::Expr>();
                    combined->kind = parser::ExprKind::BINARY_OP;
                    combined->op   = "AND";
                    combined->args.push_back(std::move(join->predicate));
                    combined->args.push_back(std::move(p));
                    join->predicate = std::move(combined);
                } else {
                    join->predicate = std::move(p);
                }
            }
        }
        return std::move(node->children[0]);
    }

    return node;
}

// ---------------------------------------------------------------------------
// Scan selection (Rule 2)
//
// If a Scan has a simple comparison predicate and the table has a
// primary index, replace the SeqScan with an IndexScan.
// ---------------------------------------------------------------------------

bool hasPrimaryIndex(catalog::CatalogManager* cat,
                     index::IndexManager* idx,
                     const std::string& table,
                     std::string& outIndexName) {
    if (cat == nullptr || idx == nullptr) return false;
    const auto* info = cat->getTable(table);
    if (info == nullptr) return false;
    if (info->primaryIndexName.empty()) return false;
    // Probe via the public API: any index name that opens successfully
    // exists.
    for (const auto& name : idx->list()) {
        if (name == info->primaryIndexName && idx->open(name) != nullptr) {
            outIndexName = name;
            return true;
        }
    }
    return false;
}

std::unique_ptr<LogicalPlan>
chooseScan(std::unique_ptr<LogicalPlan> node,
           catalog::CatalogManager* cat,
           index::IndexManager* idx) {
    if (!node) return node;
    for (auto& c : node->children) {
        c = chooseScan(std::move(c), cat, idx);
    }
    if (node->kind == LogicalKind::SCAN && node->predicate) {
        auto sp = classifySimplePredicate(*node->predicate);
        if (sp.ok) {
            std::string tableForPred = sp.table.empty() ? node->table : sp.table;
            std::string idxName;
            if (hasPrimaryIndex(cat, idx, tableForPred, idxName)) {
                // Switch to IndexScan. The predicate is kept so the
                // executor can extract the search key.
                node->kind      = LogicalKind::INDEX_SCAN;
                node->indexName = idxName;
            }
        }
    }
    return node;
}

// ---------------------------------------------------------------------------
// Join-order rewrite (Rule 3)
//
// For a two-way join, swap the children so the smaller (estimated
// cardinality) side is on the left.
// ---------------------------------------------------------------------------

std::unique_ptr<LogicalPlan>
reorderJoin(std::unique_ptr<LogicalPlan> node,
            catalog::CatalogManager* cat) {
    if (!node) return node;
    for (auto& c : node->children) {
        c = reorderJoin(std::move(c), cat);
    }
    if ((node->kind == LogicalKind::NESTED_LOOP_JOIN ||
         node->kind == LogicalKind::HASH_JOIN) &&
        node->children.size() == 2 &&
        node->children[0] && node->children[1]) {

        double leftRows  = node->children[0]->estRows;
        double rightRows = node->children[1]->estRows;
        if (leftRows <= 0.0 && cat != nullptr) {
            leftRows = static_cast<double>(cat->cardinality(node->children[0]->table));
        }
        if (rightRows <= 0.0 && cat != nullptr) {
            rightRows = static_cast<double>(cat->cardinality(node->children[1]->table));
        }
        if (leftRows <= 0.0)  leftRows  = 100.0;
        if (rightRows <= 0.0) rightRows = 100.0;
        if (rightRows < leftRows) {
            std::swap(node->children[0], node->children[1]);
        }
    }
    return node;
}

// ---------------------------------------------------------------------------
// Projection pushdown (Rule 4)
//
// For v1 the executor honors outputColumns directly, so this is a
// placeholder. The pass is wired in so future versions can prune columns
// at the scan level without touching the optimizer entry point.
// ---------------------------------------------------------------------------

std::unique_ptr<LogicalPlan>
projectPushdown(std::unique_ptr<LogicalPlan> node) {
    if (!node) return node;
    for (auto& c : node->children) {
        c = projectPushdown(std::move(c));
    }
    return node;
}

// ---------------------------------------------------------------------------
// Cost / row estimation
// ---------------------------------------------------------------------------

double estimateCardinality(const catalog::CatalogManager* cat,
                           const std::string& table) {
    if (cat == nullptr) return 100.0;
    std::uint64_t n = cat->cardinality(table);
    if (n == 0) return 100.0; // unknown — assume 100
    return static_cast<double>(n);
}

} // anonymous namespace

// =============================================================================
// Optimizer member functions
// =============================================================================

Optimizer::Optimizer(catalog::CatalogManager* cat,
                     index::IndexManager* idx,
                     transaction::TransactionManager* txn)
    : cat_(cat), idx_(idx), txn_(txn) {}

std::unique_ptr<PhysicalPlan> Optimizer::optimize(const parser::Stmt& s) {
    auto logical = toLogical(s);
    if (!logical) return nullptr;
    return toPhysical(std::move(logical));
}

std::unique_ptr<LogicalPlan> Optimizer::toLogical(const parser::Stmt& s) {
    switch (s.kind) {
        case parser::StmtKind::SELECT: {
            if (!s.select) return nullptr;
            return buildSelect(*s.select);
        }
        case parser::StmtKind::INSERT: {
            if (!s.insert) return nullptr;
            auto p = std::make_unique<LogicalPlan>();
            p->kind  = LogicalKind::INSERT;
            p->table = s.insert->table;
            return p;
        }
        case parser::StmtKind::DELETE: {
            if (!s.del) return nullptr;
            auto p = std::make_unique<LogicalPlan>();
            p->kind  = LogicalKind::DELETE;
            p->table = s.del->table;
            if (s.del->where) p->predicate = cloneExpr(*s.del->where);
            return p;
        }
        case parser::StmtKind::CREATE:
        case parser::StmtKind::DROP:
        case parser::StmtKind::TXN:
            // DDL and transaction control produce no query plan; the
            // executor handles them directly.
            return nullptr;
    }
    return nullptr;
}

double Optimizer::estimateRows(const LogicalPlan& p) const {
    switch (p.kind) {
        case LogicalKind::SCAN:
        case LogicalKind::INDEX_SCAN:
            return estimateCardinality(cat_, p.table);

        case LogicalKind::FILTER: {
            double in = 0.0;
            if (!p.children.empty() && p.children[0]) in = estimateRows(*p.children[0]);
            if (p.predicate) return in * 0.1;   // rough selectivity
            return in;
        }
        case LogicalKind::PROJECT: {
            if (!p.children.empty() && p.children[0]) return estimateRows(*p.children[0]);
            return 0.0;
        }
        case LogicalKind::NESTED_LOOP_JOIN:
        case LogicalKind::HASH_JOIN: {
            double l = (p.children.size() > 0 && p.children[0]) ? estimateRows(*p.children[0]) : 0.0;
            double r = (p.children.size() > 1 && p.children[1]) ? estimateRows(*p.children[1]) : 0.0;
            return l * r;
        }
        case LogicalKind::AGGREGATE: {
            if (p.children.empty() || !p.children[0]) return 1.0;
            double in = estimateRows(*p.children[0]);
            if (!p.groupBy.empty()) return std::max(1.0, in * 0.01);
            return 1.0; // global aggregate
        }
        case LogicalKind::SORT: {
            if (p.children.empty() || !p.children[0]) return 0.0;
            return estimateRows(*p.children[0]);
        }
        case LogicalKind::LIMIT: {
            if (p.children.empty() || !p.children[0]) return 0.0;
            double in = estimateRows(*p.children[0]);
            if (p.limit >= 0) return std::min(in, static_cast<double>(p.limit));
            return in;
        }
        case LogicalKind::INSERT:
        case LogicalKind::DELETE:
            return 1.0;
    }
    return 0.0;
}

double Optimizer::estimateCost(const LogicalPlan& p) const {
    double selfPages = 0.0;
    double selfCpu   = 0.0;

    switch (p.kind) {
        case LogicalKind::SCAN: {
            double rows = estimateRows(p);
            double rowSize = estimateRowSize(cat_, p.table);
            selfPages = std::ceil((rows * rowSize) / PAGE_SIZE);
            if (selfPages < 1.0) selfPages = 1.0;
            selfCpu   = rows;
            break;
        }
        case LogicalKind::INDEX_SCAN: {
            double rows = estimateRows(p);
            // log_F(|t|) + 1 page fetches.
            double h = std::log(rows) / std::log(INDEX_FANOUT);
            if (h < 1.0) h = 1.0;
            selfPages = std::ceil(h) + 1.0;
            selfCpu   = h + 1.0;
            break;
        }
        case LogicalKind::FILTER:
            selfCpu = estimateRows(p);
            break;
        case LogicalKind::PROJECT:
            selfCpu = estimateRows(p);
            break;
        case LogicalKind::NESTED_LOOP_JOIN: {
            double l = (p.children.size() > 0 && p.children[0]) ? estimateRows(*p.children[0]) : 1.0;
            double r = (p.children.size() > 1 && p.children[1]) ? estimateRows(*p.children[1]) : 1.0;
            selfCpu = l * r;
            break;
        }
        case LogicalKind::HASH_JOIN: {
            double l = (p.children.size() > 0 && p.children[0]) ? estimateRows(*p.children[0]) : 1.0;
            double r = (p.children.size() > 1 && p.children[1]) ? estimateRows(*p.children[1]) : 1.0;
            selfCpu   = l + r;     // build + probe
            selfPages = std::ceil((r * 32.0) / PAGE_SIZE);
            break;
        }
        case LogicalKind::AGGREGATE:
            selfCpu = estimateRows(p);
            break;
        case LogicalKind::SORT:
            selfCpu = estimateRows(p) * std::log2(std::max(2.0, estimateRows(p)));
            break;
        case LogicalKind::LIMIT:
            selfCpu = 1.0;
            break;
        case LogicalKind::INSERT:
        case LogicalKind::DELETE:
            selfCpu = 1.0;
            selfPages = 1.0;
            break;
    }

    double childCost = 0.0;
    for (const auto& c : p.children) {
        if (c) childCost += estimateCost(*c);
    }
    return childCost + selfPages * W_IO + selfCpu * W_CPU;
}

std::unique_ptr<PhysicalPlan> Optimizer::toPhysical(std::unique_ptr<LogicalPlan> in) {
    if (!in) return nullptr;

    // Rule 1: predicate pushdown.
    in = pushdown(std::move(in));

    // Rule 2: scan selection (seq vs index).
    in = chooseScan(std::move(in), cat_, idx_);

    // Bottom-up stamp of estRows/estCost so Rule 3 has fresh numbers.
    std::function<void(LogicalPlan&)> stamp = [&](LogicalPlan& n) {
        for (auto& c : n.children) if (c) stamp(*c);
        n.estRows = estimateRows(n);
        n.estCost = estimateCost(n);
    };
    stamp(*in);

    // Rule 3: join order.
    in = reorderJoin(std::move(in), cat_);

    // Rule 4: projection pushdown (no-op for v1).
    in = projectPushdown(std::move(in));

    // Translate the (now optimised) logical tree into a physical tree.
    // We must preserve the kind of the top node — wrappers like
    // PROJECT / FILTER / SORT / LIMIT / AGGREGATE don't change to
    // SEQ_SCAN. Only logical SCAN nodes become physical scans.
    auto out = std::make_unique<PhysicalPlan>();
    switch (in->kind) {
        case LogicalKind::SCAN:
            out->kind = PhysicalKind::SEQ_SCAN;
            break;
        case LogicalKind::INDEX_SCAN:
            out->kind = PhysicalKind::INDEX_SCAN;
            break;
        case LogicalKind::NESTED_LOOP_JOIN:
            out->kind = PhysicalKind::NESTED_LOOP_JOIN;
            break;
        case LogicalKind::HASH_JOIN:
            out->kind = PhysicalKind::HASH_JOIN;
            break;
        case LogicalKind::FILTER:
            out->kind = PhysicalKind::FILTER;
            break;
        case LogicalKind::PROJECT:
            out->kind = PhysicalKind::PROJECT;
            break;
        case LogicalKind::AGGREGATE:
            out->kind = PhysicalKind::AGGREGATE;
            break;
        case LogicalKind::SORT:
            out->kind = PhysicalKind::SORT;
            break;
        case LogicalKind::LIMIT:
            out->kind = PhysicalKind::LIMIT;
            break;
        case LogicalKind::INSERT:
        case LogicalKind::DELETE:
            out->kind = PhysicalKind::SEQ_SCAN;  // unused at top — handled in engine
            break;
    }
    out->table         = in->table;
    out->indexName     = in->indexName;
    out->predicate     = std::move(in->predicate);
    out->outputColumns = in->outputColumns;
    // vector<Expr> is move-only — transfer the whole vector.
    out->groupBy       = std::move(in->groupBy);
    out->orderBy       = std::move(in->orderBy);
    out->orderDesc     = in->orderDesc;
    out->limit         = in->limit;

    for (auto& c : in->children) {
        if (c) {
            out->children.push_back(toPhysical(std::move(c)));
        } else {
            out->children.push_back(nullptr);
        }
    }
    return out;
}

} // namespace minidb::planner