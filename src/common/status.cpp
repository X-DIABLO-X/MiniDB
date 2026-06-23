// =============================================================================
// src/common/status.cpp
// -----------------------------------------------------------------------------
// toString(Status) implementation. Maps each enum value to a stable,
// human-readable name. The table must stay in sync with the declaration
// in <common/status.h>; status codes are persisted in the WAL, so the
// numeric values must not change.
// =============================================================================
#include "common/status.h"

namespace minidb {

// Return a stable name for a Status value. Falls back to "UNKNOWN_STATUS"
// for any value that is not part of the enum (defensive: keeps callers
// safe if a future enum entry is added without updating this switch).
const char* toString(Status s) {
    switch (s) {
        case Status::OK:               return "OK";
        case Status::UNIMPLEMENTED:    return "UNIMPLEMENTED";
        case Status::NOT_FOUND:        return "NOT_FOUND";
        case Status::DUPLICATE_KEY:    return "DUPLICATE_KEY";
        case Status::FULL:             return "FULL";
        case Status::IO_ERROR:         return "IO_ERROR";
        case Status::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case Status::TYPE_MISMATCH:    return "TYPE_MISMATCH";
        case Status::DEADLOCK:         return "DEADLOCK";
        case Status::ABORTED:          return "ABORTED";
        case Status::TXN_CONFLICT:     return "TXN_CONFLICT";
        case Status::DONE:             return "DONE";
    }
    return "UNKNOWN_STATUS";
}

} // namespace minidb
