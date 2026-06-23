// =============================================================================
// include/executor/join.h
// -----------------------------------------------------------------------------
// NestedLoopJoin and HashJoin executors. Both consume the left child fully
// per right tuple (NLJ) or after building a hash table (HJ).
// =============================================================================
#pragma once

#include <memory>
#include <unordered_map>

#include "executor/executor.h"
#include "parser/ast.h"

namespace minidb::executor {

class NestedLoopJoinExecutor : public Executor {
public:
    NestedLoopJoinExecutor(ExecutorContext* ctx,
                           std::unique_ptr<Executor> left,
                           std::unique_ptr<Executor> right,
                           std::unique_ptr<parser::Expr> onPredicate);
    ~NestedLoopJoinExecutor() override;

    Status init() override;
    Status next (Tuple& out) override;
    Status close() override;

private:
    std::unique_ptr<Executor> left_, right_;
    std::unique_ptr<parser::Expr> on_;
    Tuple                       curLeft_;
    bool                        leftReady_ = false;
};

class HashJoinExecutor : public Executor {
public:
    HashJoinExecutor(ExecutorContext* ctx,
                     std::unique_ptr<Executor> build,    // typically the right side
                     std::unique_ptr<Executor> probe,
                     std::unique_ptr<parser::Expr> onPredicate);
    ~HashJoinExecutor() override;

    Status init() override;
    Status next (Tuple& out) override;
    Status close() override;

private:
    std::unique_ptr<Executor> build_, probe_;
    std::unique_ptr<parser::Expr> on_;
    // Hash table is built from `build_` in init().
    std::unordered_multimap<std::string, Tuple> hash_;
    Tuple curProbe_;
    bool  probeReady_ = false;
};

} // namespace minidb::executor