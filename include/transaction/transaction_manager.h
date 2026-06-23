// =============================================================================
// include/transaction/transaction_manager.h
// -----------------------------------------------------------------------------
// TransactionManager owns the active-transaction set, allocates ids,
// and implements the MVCC visibility test. It also owns a LockManager
// for the 2PL baseline path.
// =============================================================================
#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "common/status.h"
#include "common/types.h"
#include "transaction/lock_manager.h"
#include "transaction/transaction.h"

namespace minidb::transaction {

class TransactionManager {
public:
    TransactionManager();
    ~TransactionManager();

    TransactionManager(const TransactionManager&)            = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    TransactionId begin();
    Status        commit(TransactionId txn);
    Status        abort (TransactionId txn);

    // MVCC visibility.
    bool          isVisible(TransactionId rowCreator,
                            TransactionId rowDeleter,
                            const Transaction& reader);

    // 2PL baseline.
    LockManager&  lockManager() { return lockMgr_; }

    // MVCC write-set bookkeeping.
    void          recordWrite(TransactionId txn, RecordId rid);
    bool          hasConflict(TransactionId txn);   // write-write conflict?

    // For recovery.
    std::unordered_set<TransactionId> activeTxns() const;

private:
    mutable std::mutex                                       mu_;
    TransactionId                                            next_ = 1;
    std::unordered_map<TransactionId, std::unique_ptr<Transaction>> txns_;
    // per-txn write set for MVCC conflict detection
    std::unordered_map<TransactionId, std::unordered_set<RecordId>> writes_;
    LockManager                                              lockMgr_;
};

} // namespace minidb::transaction