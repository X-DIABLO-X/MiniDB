// =============================================================================
// include/recovery/recovery_manager.h
// -----------------------------------------------------------------------------
// ARIES recovery: analysis -> redo -> undo.
// =============================================================================
#pragma once

#include "common/status.h"
#include "recovery/wal.h"

namespace minidb::storage     { class BufferPool; }
namespace minidb::catalog     { class CatalogManager; }
namespace minidb::index       { class IndexManager; }
namespace minidb::transaction { class TransactionManager; }

namespace minidb::recovery {

class RecoveryManager {
public:
    RecoveryManager(WAL* wal,
                    storage::BufferPool* bp,
                    catalog::CatalogManager* cat,
                    index::IndexManager* idx,
                    transaction::TransactionManager* txn);

    // Called once at startup, BEFORE any other module accepts input.
    Status runAtStartup();

private:
    WAL*                          wal_;
    storage::BufferPool*          bp_;
    catalog::CatalogManager*      cat_;
    index::IndexManager*          idx_;
    transaction::TransactionManager* txn_;

    Status analysisPass();
    Status redoPass();
    Status undoPass();
};

} // namespace minidb::recovery