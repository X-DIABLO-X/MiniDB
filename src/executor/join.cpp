// =============================================================================
// src/executor/join.cpp
// -----------------------------------------------------------------------------
// NestedLoopJoinExecutor and HashJoinExecutor.
//
// NLJ: for every left tuple, scan the entire right child and emit the
// concatenation whenever the ON predicate is satisfied.
//
// HJ : build a multimap keyed by the Value::toString() of the build side's
// leading value, then probe with each tuple from the probe side. The
// pending-match buffer is kept in a file-local thread_local map so we can
// stay within the header's storage without modifying the public API.
// =============================================================================
#include "executor/join.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/status.h"
#include "executor/executor.h"
#include "parser/ast.h"

namespace minidb::executor {

namespace {

// File-local per-instance pending buffer for HashJoinExecutor. The
// header only declares the public state, so we keep this helper here.
std::unordered_map<const HashJoinExecutor*, std::vector<Tuple>>&
pendingStore() {
    thread_local std::unordered_map<const HashJoinExecutor*,
                                    std::vector<Tuple>> store;
    return store;
}

// Concatenate two tuples, left first then right.
Tuple concat(const Tuple& l, const Tuple& r) {
    Tuple out;
    out.values.reserve(l.values.size() + r.values.size());
    for (const auto& v : l.values) out.values.push_back(v);
    for (const auto& v : r.values) out.values.push_back(v);
    return out;
}

// Three-way compare helper for join keys.
int compareValues(const Value& a, const Value& b) {
    if (a.tag == Value::Tag::NULL_ || b.tag == Value::Tag::NULL_) return 0;
    if (a.tag == Value::Tag::INT && b.tag == Value::Tag::INT) {
        return (a.i < b.i) ? -1 : (a.i > b.i ? 1 : 0);
    }
    if (a.tag == Value::Tag::FLOAT && b.tag == Value::Tag::FLOAT) {
        return (a.f < b.f) ? -1 : (a.f > b.f ? 1 : 0);
    }
    if (a.tag == Value::Tag::INT && b.tag == Value::Tag::FLOAT) {
        float av = static_cast<float>(a.i);
        return (av < b.f) ? -1 : (av > b.f ? 1 : 0);
    }
    if (a.tag == Value::Tag::FLOAT && b.tag == Value::Tag::INT) {
        float bv = static_cast<float>(b.i);
        return (a.f < bv) ? -1 : (a.f > bv ? 1 : 0);
    }
    if (a.tag == Value::Tag::STRING && b.tag == Value::Tag::STRING) {
        if (a.s == b.s) return 0;
        return a.s < b.s ? -1 : 1;
    }
    if (a.tag == Value::Tag::BOOL && b.tag == Value::Tag::BOOL) {
        return (a.b == b.b) ? 0 : (a.b ? 1 : -1);
    }
    return 0;
}

// Apply the ON predicate to a single (left, right) candidate. v1 only
// understands comparisons between column references; anything else
// returns true (cartesian product).
bool matchesOn(const parser::Expr& on, const Tuple& l, const Tuple& r) {
    if (on.kind != parser::ExprKind::BINARY_OP) return true;
    if (on.args.size() < 2) return true;
    auto pickValue = [&](const parser::Expr* a, const Tuple& t) -> Value {
        if (!a) return Value::makeNull();
        if (a->kind == parser::ExprKind::COLUMN) {
            if (!t.values.empty()) return t.values.front();
            return Value::makeNull();
        }
        return Value::makeNull();
    };
    Value lv = pickValue(on.args[0].get(), l);
    Value rv = pickValue(on.args[1].get(), r);
    int cmp = compareValues(lv, rv);
    if (on.op == "=")  return cmp == 0;
    if (on.op == "!=") return cmp != 0;
    if (on.op == "<")  return cmp <  0;
    if (on.op == "<=") return cmp <= 0;
    if (on.op == ">")  return cmp >  0;
    if (on.op == ">=") return cmp >= 0;
    return true;
}

} // namespace

// ----- NestedLoopJoinExecutor -----

NestedLoopJoinExecutor::NestedLoopJoinExecutor(ExecutorContext* ctx,
                                               std::unique_ptr<Executor> left,
                                               std::unique_ptr<Executor> right,
                                               std::unique_ptr<parser::Expr> on)
    : Executor(ctx), left_(std::move(left)), right_(std::move(right)),
      on_(std::move(on)) {}

// Out-of-line destructor so the unique_ptr members get a TU-local one.
NestedLoopJoinExecutor::~NestedLoopJoinExecutor() = default;

// Init both children eagerly.
Status NestedLoopJoinExecutor::init() {
    Status s = left_->init();
    if (s != Status::OK) return s;
    s = right_->init();
    if (s != Status::OK) return s;
    leftReady_ = false;
    return Status::OK;
}

// Classic NLJ: for each left tuple, scan every right tuple; emit on match.
Status NestedLoopJoinExecutor::next(Tuple& out) {
    while (true) {
        if (!leftReady_) {
            Status s = left_->next(curLeft_);
            if (s != Status::OK) return Status::DONE;
            // Reset right for the new left tuple.
            (void)right_->close();
            Status rs = right_->init();
            if (rs != Status::OK) return rs;
            leftReady_ = true;
        }
        Tuple r;
        Status rs = right_->next(r);
        if (rs == Status::OK) {
            if (!on_) {
                out = concat(curLeft_, r);
                return Status::OK;
            }
            if (matchesOn(*on_, curLeft_, r)) {
                out = concat(curLeft_, r);
                return Status::OK;
            }
            continue;
        }
        // Right side exhausted; advance left.
        leftReady_ = false;
    }
}

// Close both children.
Status NestedLoopJoinExecutor::close() {
    if (left_)  (void)left_->close();
    if (right_) (void)right_->close();
    return Status::OK;
}

// ----- HashJoinExecutor -----

HashJoinExecutor::HashJoinExecutor(ExecutorContext* ctx,
                                   std::unique_ptr<Executor> build,
                                   std::unique_ptr<Executor> probe,
                                   std::unique_ptr<parser::Expr> on)
    : Executor(ctx), build_(std::move(build)), probe_(std::move(probe)),
      on_(std::move(on)) {}

// Out-of-line destructor: also clear the file-local pending buffer.
HashJoinExecutor::~HashJoinExecutor() {
    pendingStore().erase(this);
}

// Build phase: drain the build side into the multimap, keyed by the
// Value::toString() of the first value of each tuple.
Status HashJoinExecutor::init() {
    Status s = build_->init();
    if (s != Status::OK) return s;
    hash_.clear();
    Tuple t;
    while (build_->next(t) == Status::OK) {
        std::string key = t.values.empty()
            ? std::string()
            : t.values.front().toString();
        hash_.emplace(std::move(key), std::move(t));
    }
    (void)build_->close();
    pendingStore()[this].clear();
    return probe_->init();
}

// Probe phase: look up each probe tuple's first value in the multimap and
// emit concatenated matches. Pending matches for the current probe tuple
// live in the file-local map.
Status HashJoinExecutor::next(Tuple& out) {
    auto& pending = pendingStore()[this];
    while (true) {
        if (!probeReady_) {
            // Fill pending with matches for the next probe tuple.
            pending.clear();
            Status s = probe_->next(curProbe_);
            if (s != Status::OK) return Status::DONE;
            std::string key = curProbe_.values.empty()
                ? std::string()
                : curProbe_.values.front().toString();
            auto range = hash_.equal_range(key);
            for (auto it = range.first; it != range.second; ++it) {
                pending.push_back(it->second);
            }
            if (pending.empty()) continue;     // no match: try next probe
            probeReady_ = true;
        }
        if (!pending.empty()) {
            Tuple left = pending.front();
            pending.erase(pending.begin());
            out = concat(left, curProbe_);
            if (pending.empty()) probeReady_ = false;
            return Status::OK;
        }
        probeReady_ = false;
    }
}

// Tear down the probe side and free the hash table.
Status HashJoinExecutor::close() {
    if (probe_) (void)probe_->close();
    hash_.clear();
    pendingStore().erase(this);
    return Status::OK;
}

} // namespace minidb::executor