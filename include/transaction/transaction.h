// =============================================================================
// include/transaction/transaction.h
// -----------------------------------------------------------------------------
// Transaction = id + state + MVCC snapshot. Pure data, no behaviour.
// =============================================================================
#pragma once

#include "common/types.h"

namespace minidb::transaction {

enum class TxnState { ACTIVE, COMMITTED, ABORTED };
enum class LockMode { SHARED, EXCLUSIVE };

class Transaction {
public:
    explicit Transaction(TransactionId id) : id_(id) {}

    TransactionId id()          const { return id_; }
    TxnState      state()       const { return state_; }
    TransactionId snapshotHigh()const { return snapshotHigh_; }

    void setState      (TxnState s)       { state_ = s; }
    void setSnapshotHigh(TransactionId s) { snapshotHigh_ = s; }

private:
    TransactionId id_           = INVALID_TXN_ID;
    TxnState      state_        = TxnState::ACTIVE;
    TransactionId snapshotHigh_ = 0;     // MVCC: max active txn at begin
};

} // namespace minidb::transaction