// =============================================================================
// src/transaction/lock_manager.cpp
// -----------------------------------------------------------------------------
// Strict 2PL with deadlock detection via a wait-for graph.
//
// Concurrency model
// -----------------
//   * mu_ guards every map. The lock manager is a coarse-grained in-memory
//     structure; contention is low.
//   * We never actually block a calling thread. If a request would have to
//     wait, we record the wait edge in waitsFor_, run a cycle check, and
//     report DEADLOCK right away. The caller is expected to abort the
//     transaction. This keeps the manager simple.
//
// Visibility rules for SHARED vs EXCLUSIVE
//   * Many txns may hold S on the same rid.
//   * Only one txn may hold X; while X is held no S is granted either.
//   * Upgrading S -> X is treated as a fresh request.
// =============================================================================
#include "transaction/lock_manager.h"

#include <algorithm>

namespace minidb::transaction {

LockManager::LockManager()  = default;
LockManager::~LockManager() = default;

namespace {

// DFS over the waitsFor_ graph. A back-edge means a cycle. Operates on a
// const reference; no locking needed if caller already holds mu_.
bool dfsHasCycle(TransactionId start,
                 const std::unordered_map<TransactionId, TransactionId>& edges,
                 std::unordered_set<TransactionId>& onStack,
                 std::unordered_set<TransactionId>& visited) {
    if (onStack.count(start))   return true;
    if (visited.count(start))   return false;
    visited.insert(start);
    onStack.insert(start);

    auto it = edges.find(start);
    if (it != edges.end()) {
        if (dfsHasCycle(it->second, edges, onStack, visited)) return true;
    }

    onStack.erase(start);
    return false;
}

} // anonymous namespace

// Check if some other txn holds an exclusive lock in this holder map.
// Caller must already hold mu_. (rid itself is implicit in the map
// reference; passed-in rid is not needed here.)
static bool holderMapHasOtherExclusive(TransactionId self,
    const std::unordered_map<TransactionId, LockMode>& h) {
    for (const auto& [other, mode] : h) {
        if (other != self && mode == LockMode::EXCLUSIVE) return true;
    }
    return false;
}

// Check for a cycle in waitsFor_, assuming the caller already holds mu_.
static bool hasCycleLocked(
    const std::unordered_map<TransactionId, TransactionId>& waitsFor) {
    std::unordered_set<TransactionId> visited;
    std::unordered_set<TransactionId> onStack;
    for (const auto& [t, _] : waitsFor) {
        if (visited.count(t)) continue;
        if (dfsHasCycle(t, waitsFor, onStack, visited)) return true;
    }
    return false;
}

// Acquire a SHARED lock on rid for txn.
//
// Grant iff no other txn holds an EXCLUSIVE lock on rid. Otherwise add a
// wait edge and run a cycle check. Returns DEADLOCK if the wait would
// form a cycle (v1: no actual blocking).
Status LockManager::acquireShared(TransactionId txn, RecordId rid) {
    std::lock_guard<std::mutex> lk(mu_);

    auto& h = holders_[rid];
    if (holderMapHasOtherExclusive(txn, h)) {
        // Pick the X-holder as the blocker. There can be only one X-holder.
        for (const auto& [other, mode] : h) {
            if (mode == LockMode::EXCLUSIVE) {
                waitsFor_[txn] = other;
                waiters_[rid].push_back(txn);
                break;
            }
        }
        if (hasCycleLocked(waitsFor_)) return Status::DEADLOCK;
        return Status::DEADLOCK;  // v1: no actual blocking; caller aborts.
    }

    h[txn] = LockMode::SHARED;
    waitsFor_.erase(txn);
    waiters_[rid].erase(
        std::remove(waiters_[rid].begin(), waiters_[rid].end(), txn),
        waiters_[rid].end());
    return Status::OK;
}

// Acquire an EXCLUSIVE lock on rid for txn.
//
// Grant iff no other txn holds any lock (S or X) on rid. Otherwise add a
// wait edge and run a cycle check. Returns DEADLOCK on cycle.
Status LockManager::acquireExclusive(TransactionId txn, RecordId rid) {
    std::lock_guard<std::mutex> lk(mu_);

    auto& h = holders_[rid];
    for (const auto& [other, mode] : h) {
        if (other == txn) continue;  // upgrade from S is fine
        waitsFor_[txn] = other;
        waiters_[rid].push_back(txn);
        if (hasCycleLocked(waitsFor_)) return Status::DEADLOCK;
        return Status::DEADLOCK;  // v1: no blocking
    }

    h[txn] = LockMode::EXCLUSIVE;
    waitsFor_.erase(txn);
    waiters_[rid].erase(
        std::remove(waiters_[rid].begin(), waiters_[rid].end(), txn),
        waiters_[rid].end());
    return Status::OK;
}

// Release every lock held by txn. Called at commit / abort.
void LockManager::releaseAll(TransactionId txn) {
    std::lock_guard<std::mutex> lk(mu_);

    for (auto& [rid, h] : holders_) {
        h.erase(txn);
    }
    // Drop empty holder entries to bound memory growth.
    for (auto it = holders_.begin(); it != holders_.end(); ) {
        if (it->second.empty()) it = holders_.erase(it);
        else                    ++it;
    }

    waitsFor_.erase(txn);
    for (auto& [rid, q] : waiters_) {
        q.erase(std::remove(q.begin(), q.end(), txn), q.end());
    }
}

// Walk the wait-for graph. Public entry point (acquires mu_).
// Exposed for tests and the demo.
bool LockManager::hasCycle() {
    std::lock_guard<std::mutex> lk(mu_);
    return hasCycleLocked(waitsFor_);
}

} // namespace minidb::transaction
