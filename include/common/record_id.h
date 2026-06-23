// =============================================================================
// include/common/record_id.h
// -----------------------------------------------------------------------------
// RecordId packing / unpacking helpers.
//
// Layout (64 bits):
//   [ 63 .. 32 ]  pageId   (32 bits)  — see <common/types.h>
//   [ 31 .. 16 ]  slotIdx  (16 bits)
//   [ 15 ..  0 ]  version  (16 bits)  — reserved for MVCC, 0 = "current"
//
// All RecordId construction in MiniDB goes through makeRid() so the layout
// is centralised.
// =============================================================================
#pragma once

#include <cstdint>
#include "common/types.h"

namespace minidb {

// Pack (pageId, slotIdx) into a RecordId. Version is set to 0.
RecordId  makeRid(PageId pageId, std::uint16_t slotIdx);

// Unpack a RecordId.
PageId      ridPage (RecordId rid);
std::uint16_t ridSlot(RecordId rid);
std::uint16_t ridVersion(RecordId rid);

} // namespace minidb
