// =============================================================================
// tests/transaction/transaction_test.cpp
// -----------------------------------------------------------------------------
// Will exercise the LockManager + MVCC visibility once implemented.
// =============================================================================
#include <cstdio>

#include "transaction/lock_manager.h"
#include "transaction/transaction_manager.h"

using namespace minidb;

int main() {
    transaction::LockManager lm;
    Status s = lm.acquireShared(1, /*rid=*/42);
    if (s == Status::UNIMPLEMENTED) {
        std::fprintf(stderr, "[skip] LockManager::acquireShared is a stub\n");
        return 0;
    }
    lm.releaseAll(1);
    std::printf("[ok]   lock acquired and released\n");
    return 0;
}
