// =============================================================================
// include/common/status.h
// -----------------------------------------------------------------------------
// The Status enum returned by every public MiniDB function.
//
// Conventions:
//   - OK = 0, so a Status can be tested with `if (status) {...}`.
//   - Status codes are stable — they are persisted in the WAL. DO NOT
//     renumber, only add.
//   - UNIMPLEMENTED is a special "this stub is intentionally a no-op" code.
//     It lets the rest of the team build and run partial code paths.
// =============================================================================
#pragma once

#include <cstdint>

namespace minidb {

enum class Status : int {
    OK                = 0,
    UNIMPLEMENTED     = 1,   // stub: function exists but is not implemented
    NOT_FOUND         = 2,
    DUPLICATE_KEY     = 3,
    FULL              = 4,
    IO_ERROR          = 5,
    INVALID_ARGUMENT  = 6,
    TYPE_MISMATCH     = 7,
    DEADLOCK          = 8,
    ABORTED           = 9,
    TXN_CONFLICT      = 10,  // MVCC write-write conflict at commit
    DONE              = 11,  // Volcano executor: no more tuples
};

// Returns a stable, human-readable name for a Status (e.g. "OK", "NOT_FOUND").
// Useful in error messages and in test asserts. The string is from a static
// table; do not free it.
const char* toString(Status s);

} // namespace minidb
