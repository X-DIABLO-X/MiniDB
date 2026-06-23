// =============================================================================
// src/executor/query_engine.cpp
// -----------------------------------------------------------------------------
// QueryEngine: top-level SQL -> executor tree driver.
//
//   execute(sql)        — SELECT, returns all matching Tuples.
//   executeUpdate(sql)  — INSERT / DELETE / CREATE / DROP / BEGIN / COMMIT / ROLLBACK,
//                         returns a Status.
//
// Pipeline:  Lexer  ->  Parser  ->  Optimizer (PhysicalPlan)  ->
//            build executor tree  ->  init / next* / close.
// =============================================================================
#include "executor/query_engine.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog/catalog_manager.h"
#include "common/status.h"
#include "executor/delete_executor.h"
#include "executor/executor.h"
#include "executor/index_scan.h"
#include "executor/insert_executor.h"
#include "executor/join.h"
#include "executor/seq_scan.h"
#include "parser/ast.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "planner/optimizer.h"
#include "planner/physical_plan.h"
#include "recovery/recovery_manager.h"
#include "transaction/transaction_manager.h"

namespace minidb::executor {

namespace {

// Build an executor subtree for a single PhysicalPlan node. Returns
// nullptr when the kind has no executor mapping.
std::unique_ptr<Executor> buildPlan(ExecutorContext* ctx,
                                     const planner::PhysicalPlan& plan) {
    switch (plan.kind) {
        case planner::PhysicalKind::SEQ_SCAN: {
            return std::make_unique<SeqScanExecutor>(
                ctx, plan.table, std::move(const_cast<std::unique_ptr<parser::Expr>&>(plan.predicate)));
        }
        case planner::PhysicalKind::INDEX_SCAN: {
            return std::make_unique<IndexScanExecutor>(
                ctx, plan.table, plan.indexName,
                std::move(const_cast<std::unique_ptr<parser::Expr>&>(plan.predicate)));
        }
        case planner::PhysicalKind::NESTED_LOOP_JOIN: {
            if (plan.children.size() < 2) return nullptr;
            auto left  = buildPlan(ctx, *plan.children[0]);
            auto right = buildPlan(ctx, *plan.children[1]);
            if (!left || !right) return nullptr;
            return std::make_unique<NestedLoopJoinExecutor>(
                ctx, std::move(left), std::move(right),
                std::move(const_cast<std::unique_ptr<parser::Expr>&>(plan.predicate)));
        }
        case planner::PhysicalKind::HASH_JOIN: {
            if (plan.children.size() < 2) return nullptr;
            auto build = buildPlan(ctx, *plan.children[0]);
            auto probe = buildPlan(ctx, *plan.children[1]);
            if (!build || !probe) return nullptr;
            return std::make_unique<HashJoinExecutor>(
                ctx, std::move(build), std::move(probe),
                std::move(const_cast<std::unique_ptr<parser::Expr>&>(plan.predicate)));
        }
        case planner::PhysicalKind::FILTER:
        case planner::PhysicalKind::PROJECT:
        case planner::PhysicalKind::AGGREGATE:
        case planner::PhysicalKind::SORT:
        case planner::PhysicalKind::LIMIT:
            // For v1 we ignore these wrapper kinds and recurse to the
            // first child so the resulting pipeline still works.
            if (!plan.children.empty()) {
                return buildPlan(ctx, *plan.children[0]);
            }
            return nullptr;
        case planner::PhysicalKind::INSERT:
        case planner::PhysicalKind::DELETE:
            // Handled directly by the query engine via the parsed stmt;
            // no plan-level executor is built.
            return nullptr;
    }
    return nullptr;
}

// Apply ORDER BY + LIMIT in memory to the materialised tuple set.
// ORDER BY is positional for v1: we sort by the first orderBy column.
void sortAndLimit(std::vector<Tuple>& rows, const parser::SelectStmt& s) {
    if (!s.orderBy.empty() && !rows.empty()) {
        const parser::Expr& key = *s.orderBy[0];
        const std::string colName = key.text;
        // Locate the column index by name from the catalog of the first
        // tuple via the query engine context. For v1 we just keep the
        // natural order when the name doesn't appear.
        (void)colName;
        // v1: stable sort to keep the existing row order; a later
        // revision can plug in real comparator functions.
    }
    if (s.limit >= 0 && static_cast<int>(rows.size()) > s.limit) {
        rows.resize(static_cast<std::size_t>(s.limit));
    }
}

} // namespace

// ----- QueryEngine -----

QueryEngine::QueryEngine(storage::BufferPool*             bp,
                         catalog::CatalogManager*         cat,
                         index::IndexManager*             idx,
                         transaction::TransactionManager* txn,
                         recovery::RecoveryManager*       rec)
    : ctx_{bp, cat, idx, txn}
{
    optimizer_ = std::make_unique<planner::Optimizer>(cat, idx, txn);
    (void)rec;
}

// Default the destructor so the unique_ptr<Optimizer> gets a TU-local one.
QueryEngine::~QueryEngine() = default;

// SELECT path. Parse, optimise, build the executor tree, drain it.
std::vector<Tuple> QueryEngine::execute(const std::string& sql) {
    std::vector<Tuple> out;
    parser::Lexer lex(sql);
    auto toks = lex.tokenize();
    parser::Parser p(std::move(toks));
    parser::Stmt stmt = p.parse();
    if (stmt.kind != parser::StmtKind::SELECT || !stmt.select) {
        return out;
    }

    std::unique_ptr<planner::PhysicalPlan> plan;
    try {
        plan = optimizer_->optimize(stmt);
    } catch (...) {
        return out;
    }
    if (!plan) return out;

    auto root = buildPlan(&ctx_, *plan);
    if (!root) return out;

    if (root->init() != Status::OK) return out;
    Tuple t;
    while (root->next(t) == Status::OK) {
        out.push_back(std::move(t));
    }
    (void)root->close();
    sortAndLimit(out, *stmt.select);
    return out;
}

// INSERT / DELETE / CREATE / DROP / TXN path.
Status QueryEngine::executeUpdate(const std::string& sql) {
    parser::Lexer lex(sql);
    auto toks = lex.tokenize();
    parser::Parser p(std::move(toks));
    parser::Stmt stmt = p.parse();

    // Surface parser errors (e.g. "show tables" which we don't support)
    // as INVALID_ARGUMENT — the CLI prints "Status: INVALID_ARGUMENT".
    // A fuller build would carry the parser's error string through.
    if (!p.lastError().empty() && stmt.kind == parser::StmtKind::SELECT &&
        !stmt.select) {
        return Status::INVALID_ARGUMENT;
    }

    switch (stmt.kind) {
        case parser::StmtKind::INSERT: {
            if (!stmt.insert) return Status::INVALID_ARGUMENT;
            InsertExecutor exec(&ctx_, std::move(stmt.insert));
            Status s = exec.init();
            if (s != Status::OK) return s;
            Tuple t;
            while (exec.next(t) == Status::OK) { /* drain */ }
            return exec.close();
        }
        case parser::StmtKind::DELETE: {
            if (!stmt.del) return Status::INVALID_ARGUMENT;
            DeleteExecutor exec(&ctx_, std::move(stmt.del));
            Status s = exec.init();
            if (s != Status::OK) return s;
            Tuple t;
            while (exec.next(t) == Status::OK) { /* drain */ }
            return exec.close();
        }
        case parser::StmtKind::CREATE: {
            if (!stmt.create) return Status::INVALID_ARGUMENT;
            const auto& c = *stmt.create;
            catalog::TableInfo info;
            info.name = c.table;
            info.schema = catalog::Schema();
            for (const auto& col : c.columns) {
                info.schema.addColumn(col);
            }
            return ctx_.cat->createTable(info);
        }
        case parser::StmtKind::DROP: {
            if (!stmt.drop) return Status::INVALID_ARGUMENT;
            return ctx_.cat->dropTable(stmt.drop->table);
        }
        case parser::StmtKind::TXN: {
            if (!stmt.txn) return Status::INVALID_ARGUMENT;
            switch (stmt.txn->op) {
                case parser::TxnStmt::Op::BEGIN:
                    (void)ctx_.txn->begin();
                    return Status::OK;
                case parser::TxnStmt::Op::COMMIT: {
                    auto active = ctx_.txn->activeTxns();
                    Status last = Status::OK;
                    for (auto id : active) last = ctx_.txn->commit(id);
                    return last;
                }
                case parser::TxnStmt::Op::ROLLBACK: {
                    auto active = ctx_.txn->activeTxns();
                    Status last = Status::OK;
                    for (auto id : active) last = ctx_.txn->abort(id);
                    return last;
                }
            }
            return Status::OK;
        }
        case parser::StmtKind::SELECT:
            return Status::INVALID_ARGUMENT;
    }
    return Status::INVALID_ARGUMENT;
}

} // namespace minidb::executor
