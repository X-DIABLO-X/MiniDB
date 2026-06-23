// =============================================================================
// src/common/record_id.cpp
// -----------------------------------------------------------------------------
// Implementations of makeRid / ridPage / ridSlot / ridVersion.
//
// RecordId layout (64 bits):
//   [ 63 .. 32 ]  pageId   (32 bits)
//   [ 31 .. 16 ]  slotIdx  (16 bits)
//   [ 15 ..  0 ]  version  (16 bits)  — 0 = current, reserved for MVCC
//
// All packing/unpacking goes through these helpers so the layout lives in
// exactly one place.
// =============================================================================
#include "common/record_id.h"

namespace minidb {

// Pack (pageId, slotIdx) into a RecordId. Version is set to 0.
// Cast to RecordId (uint64_t) first so the 32-bit shift is well-defined.
RecordId makeRid(PageId pageId, std::uint16_t slotIdx) {
    return (static_cast<RecordId>(pageId) << 32)
         | (static_cast<RecordId>(slotIdx) << 16)
         | 0ULL;
}

// Top 32 bits are the pageId.
PageId ridPage(RecordId rid) {
    return static_cast<PageId>(rid >> 32);
}

// Middle 16 bits are the slotIdx.
std::uint16_t ridSlot(RecordId rid) {
    return static_cast<std::uint16_t>((rid >> 16) & 0xFFFFu);
}

// Low 16 bits are the version (0 = current).
std::uint16_t ridVersion(RecordId rid) {
    return static_cast<std::uint16_t>(rid & 0xFFFFu);
}

} // namespace minidb
