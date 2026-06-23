// =============================================================================
// src/common/types.cpp
// -----------------------------------------------------------------------------
// Translation unit for non-template definitions in <common/types.h>.
//
// The ID aliases (PageId, TransactionId, RecordId, FrameId, LSN, Key) are
// pure type aliases and need no out-of-line definitions. This file exists
// so the build system has a translation unit for the common/ module.
// Future cross-translation-unit helpers (e.g. a process-wide transaction
// id generator) belong here.
// =============================================================================
#include "common/types.h"

namespace minidb {
    // No definitions required: all members of this header are aliases or
    // constexpr values, which the compiler emits on demand at use sites.
} // namespace minidb
