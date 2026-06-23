// =============================================================================
// src/transaction/transaction_manager.cpp
// -----------------------------------------------------------------------------
// Transaction lifecycle: begin / commit / abort.
//
// MVCC visibility
// ---------------
//   A row is visible to a reader whose snapshot is S (the max active txn
//   id at the reader's BEGIN time) iff:
//
//       row.createdBy <= S  AND  (row.deletedBy == 0 OR row.deletedBy > S)
//
//   This implements snapshot isolation: readers see a consistent snapshot
//   and never block on writers.
//
// Write-write conflict detection
// -------------------------------
//   At commit time, if any other committed txn that started before the
//   committing txn's snapshot wrote the same rid, the commit returns
//   TXN_CONFLICT. The executor then aborts.
// =============================================================================
#include "transaction/transaction_manager.h"

#include <algorithm>

namespace minidb::transaction {

TransactionManager::TransactionManager()  = default;
TransactionManager::~TransactionManager() = default;

// Allocate a new transaction id, record the snapshot high-water mark
// (max active txn id before this one), and return the id.
TransactionId TransactionManager::begin() {
    std::lock_guard<std::mutex> lk(mu_);

    TransactionId id = next_++;
    auto t = std::make_unique<Transaction>(id);
    // Snapshot = max active txn id before we start. next_ - 1 is always
    // >= the id of every txn already in txns_, because ids are monotonic.
    t->setSnapshotHigh(next_ - 1);
    txns_[id] = std::move(t);
    return id;
}

// Commit a transaction.
//   1. Check for write-write conflicts.
//   2. If clean, mark COMMITTED, release 2PL locks, drop write set.
Status TransactionManager::commit(TransactionId id) {
    std::lock_guard<std::mutex> lk(mu_);

    auto it = txns_.find(id);
    if (it == txns_.end()) return Status::NOT_FOUND;

    if (hasConflict(id)) return Status::TXN_CONFLICT;

    it->second->setState(TxnState::COMMITTED);
    lockMgr_.releaseAll(id);
    writes_.erase(id);
    return Status::OK;
}

// Abort a transaction. Mark ABORTED, release 2PL locks, drop write set.
Status TransactionManager::abort(TransactionId id) {
    std::lock_guard<std::mutex> lk(mu_);

    auto it = txns_.find(id);
    if (it == txns_.end()) return Status::NOT_FOUND;

    it->second->setState(TxnState::ABORTED);
    lockMgr_.releaseAll(id);
    writes_.erase(id);
    return Status::OK;
}

// MVCC visibility test.
//
// A row (created by c, deleted by d) is visible to reader r iff:
//   c <= r.snapshotHigh()  AND  (d == 0 OR d > r.snapshotHigh())
//
// deletedBy == 0 means "not deleted yet".
bool TransactionManager::isVisible(TransactionId rowCreator,
                                   TransactionId rowDeleter,
                                   const Transaction& reader) {
    if (rowCreator > reader.snapshotHigh()) return false;
    if (rowDeleter != 0 && rowDeleter <= reader.snapshotHigh()) return false;
    return true;
}

// Record that txn wrote rid (for MVCC conflict detection at commit time).
void TransactionManager::recordWrite(TransactionId txn, RecordId rid) {
    std::lock_guard<std::mutex> lk(mu_);
    writes_[txn].insert(rid);
}

// Return true if any other committed txn (with id < txn's snapshotHigh
// + 1, i.e. that started before or concurrently with txn) also wrote one
// of the same rids. This is the first-updater-wins rule: if there is an
// overlap, the committing txn loses.
bool TransactionManager::hasConflict(TransactionId txn) {
    std::lock_guard<std::mutex> lk(mu_);

    auto myIt = writes_.find(txn);
    if (myIt == writes_.end()) return false;

    auto txnIt = txns_.find(txn);
    if (txnIt == txns_.end()) return false;

    TransactionId mySnapshot = txnIt->second->snapshotHigh();

    for (const auto& [otherId, otherRids] : writes_) {
        if (otherId == txn) continue;

        // Only txns that could be visible in our snapshot matter.
        // An "older" txn (id <= snapshot) that committed already wrote
        // these rids before us -> conflict.
        if (otherId > mySnapshot) continue;

        auto oIt = txns_.find(otherId);
        if (oIt == txns_.end()) continue;
        if (oIt->second->state() != TxnState::COMMITTED) continue;

        for (RecordId rid : myIt->second) {
            if (otherRids.count(rid)) return true;
        }
    }
    return false;
}

// Return all transaction ids that are currently ACTIVE. Used at recovery
// time to know which txns need aborting after a crash.
std::unordered_set<TransactionId> TransactionManager::activeTxns() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::unordered_set<TransactionId> out;
    for (const auto& [id, t] : txns_) {
        if (t->state() == TxnState::ACTIVE) out.insert(id);
    }
    return out;
}

} // namespace minidb::transaction
