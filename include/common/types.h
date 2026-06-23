// =============================================================================
// include/common/types.h
// -----------------------------------------------------------------------------
// Common ID aliases used everywhere in MiniDB.
//
// RULE: a value of 0 is always "invalid" for the IDs declared here. The
// constexpr INVALID_* aliases encode that convention. Code that needs to
// "no page / no txn / no rid" should compare against these constants, not
// against 0 directly, so a future refactor stays safe.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace minidb {

// PageId         — physical page number inside the data file.
// TransactionId  — monotonically increasing transaction identifier.
// RecordId       — packed (pageId << 32) | slotIdx. Use makeRid() / ridPage()
//                  / ridSlot() from <common/record_id.h>.
// FrameId        — index of a buffer-pool frame. Negative means "not in pool".
// LSN            — log sequence number; also used as the redo marker on a
//                  page (pageLSN).
// Key            — generic B+ tree key. The index layer treats it as a
//                  length-prefixed byte string; typed values are encoded
//                  into a Key by the index wrapper (see IndexManager).
using PageId        = std::uint32_t;
using TransactionId = std::uint64_t;
using RecordId      = std::uint64_t;
using FrameId       = std::int32_t;
using LSN           = std::uint64_t;
using Key           = std::string;

constexpr PageId       INVALID_PAGE_ID  = 0;
constexpr FrameId      INVALID_FRAME_ID = -1;
constexpr TransactionId INVALID_TXN_ID  = 0;
constexpr RecordId     INVALID_RID      = 0;
constexpr LSN          INVALID_LSN      = 0;

} // namespace minidb
