// =============================================================================
// include/transaction/lock_manager.h
// -----------------------------------------------------------------------------
// Strict 2PL with deadlock detection via a wait-for graph. See
// include/transaction/README.md.
// =============================================================================
#pragma once

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/status.h"
#include "common/types.h"
#include "transaction/transaction.h"   // for LockMode

namespace minidb::transaction {

class LockManager {
public:
    LockManager();
    ~LockManager();

    LockManager(const LockManager&)            = delete;
    LockManager& operator=(const LockManager&) = delete;

    Status acquireShared   (TransactionId txn, RecordId rid);
    Status acquireExclusive(TransactionId txn, RecordId rid);
    void   releaseAll      (TransactionId txn);   // 2PL: only at commit/abort

    // True iff the wait-for graph currently has a cycle. Exposed for
    // tests and for the demo.
    bool   hasCycle();

private:
    // rid -> set of (txn, mode)
    std::unordered_map<RecordId,
        std::unordered_map<TransactionId, LockMode>> holders_;
    // rid -> list of (txn) blocked on it
    std::unordered_map<RecordId, std::vector<TransactionId>> waiters_;
    // txn -> txn (the txn it's waiting on)
    std::unordered_map<TransactionId, TransactionId> waitsFor_;
    std::mutex mu_;
};

} // namespace minidb::transaction