// =============================================================================
// src/storage/page.cpp
// -----------------------------------------------------------------------------
// Implementation of the Page class. See include/storage/README.md for the
// on-disk layout.
//
// Layout (4 KB page):
//   [ pageId    : u32    ] @ 0
//   [ pageLSN   : u64    ] @ 4
//   [ slotCount : u16    ] @ 12
//   [ freePtr   : u16    ] @ 14   (offset of the next free byte; grows down)
//   [ dirty     : u8     ] @ 16
//   [ padding   : 47*u8  ] @ 17
//   [ slot dir  : n * 4B ] @ 64   (n = slotCount; entry = [offset:u16, length:u16])
//   ... free space ...
//   ... live records, growing from the end of the page toward the middle ...
//
// The slot directory grows from the start (just after the header) and the
// record payload grows from the end. They meet in the middle.
// =============================================================================
#include "storage/page.h"

#include <cstring>
#include <stdexcept>

namespace minidb::storage {

namespace {

// Header offsets within a page (see include/storage/README.md).
constexpr std::size_t OFF_PAGE_ID   = 0;
constexpr std::size_t OFF_PAGE_LSN  = 4;
constexpr std::size_t OFF_SLOT_CNT  = 12;
constexpr std::size_t OFF_FREE_PTR  = 14;
constexpr std::size_t OFF_DIRTY     = 16;
constexpr std::size_t OFF_SLOT_BASE = 64;   // start of slot directory

// Each slot entry: [offset : u16, length : u16].
struct SlotEntry {
    std::uint16_t offset;
    std::uint16_t length;
};

constexpr std::size_t SLOT_ENTRY_SIZE = sizeof(SlotEntry);
constexpr std::uint16_t TOMBSTONE_LEN  = 0;
constexpr std::uint16_t NO_SLOT        = 0xFFFFu;

// Read/write POD into a raw byte buffer at `off`.
template <typename T>
T readAt(const std::uint8_t* buf, std::size_t off) {
    T v{};
    std::memcpy(&v, buf + off, sizeof(T));
    return v;
}

template <typename T>
void writeAt(std::uint8_t* buf, std::size_t off, const T& v) {
    std::memcpy(buf + off, &v, sizeof(T));
}

// Load a slot entry from the page. Returns the (offset, length) pair.
SlotEntry slotOf(const Page& p, std::uint16_t slot) {
    const std::uint8_t* base = p.data() + OFF_SLOT_BASE + slot * SLOT_ENTRY_SIZE;
    return SlotEntry{readAt<std::uint16_t>(base, 0),
                      readAt<std::uint16_t>(base, sizeof(std::uint16_t))};
}

void writeSlot(Page& p, std::uint16_t slot, const SlotEntry& e) {
    std::uint8_t* base = p.data() + OFF_SLOT_BASE + slot * SLOT_ENTRY_SIZE;
    writeAt<std::uint16_t>(base, 0, e.offset);
    writeAt<std::uint16_t>(base, sizeof(std::uint16_t), e.length);
}

// Compute the highest byte offset the slot directory can reach. If we
// already use more than this, there is no room for another record.
std::size_t dirEnd(const Page& p) {
    return OFF_SLOT_BASE + static_cast<std::size_t>(p.slotCount()) * SLOT_ENTRY_SIZE;
}

} // namespace

Page::Page() {
    reset();
}

void Page::reset() {
    std::memset(buf_.data(), 0, SIZE);
    setPageId(INVALID_PAGE_ID);
    setPageLSN(INVALID_LSN);
    setDirty(false);
}

PageId Page::getPageId() const {
    return readAt<PageId>(buf_.data(), OFF_PAGE_ID);
}

void Page::setPageId(PageId id) {
    writeAt<PageId>(buf_.data(), OFF_PAGE_ID, id);
}

std::uint8_t* Page::data()             { return buf_.data(); }
const std::uint8_t* Page::data() const { return buf_.data(); }

std::uint64_t Page::getPageLSN() const {
    return readAt<std::uint64_t>(buf_.data(), OFF_PAGE_LSN);
}

void Page::setPageLSN(std::uint64_t lsn) {
    writeAt<std::uint64_t>(buf_.data(), OFF_PAGE_LSN, lsn);
}

std::uint16_t Page::slotCount() const {
    return readAt<std::uint16_t>(buf_.data(), OFF_SLOT_CNT);
}

bool Page::isDirty() const {
    return buf_[OFF_DIRTY] != 0;
}

void Page::setDirty(bool d) {
    buf_[OFF_DIRTY] = d ? 1 : 0;
}

// Return the smallest unused slot index. If the directory is dense, append.
std::uint16_t Page::firstFreeSlot() const {
    const std::uint16_t n = slotCount();
    for (std::uint16_t i = 0; i < n; ++i) {
        const SlotEntry e = slotOf(*this, i);
        if (e.length == TOMBSTONE_LEN) return i;
    }
    return n;  // caller will append a new entry
}

// Insert `bytes` at `slot`. The slot must currently be empty (tombstone or
// past end of the directory). The payload is written at the current freePtr
// and the freePtr moves down. Returns false if there is no room.
bool Page::insertRecord(std::uint16_t slot, std::span<const std::uint8_t> bytes) {
    if (slot >= MAX_SLOTS) return false;
    const std::uint16_t n = slotCount();
    if (slot < n) {
        const SlotEntry existing = slotOf(*this, slot);
        if (existing.length != TOMBSTONE_LEN) return false;  // slot in use
    }
    const std::size_t dirEndOff = (slot < n) ? dirEnd(*this)
                                             : OFF_SLOT_BASE + (static_cast<std::size_t>(slot) + 1) * SLOT_ENTRY_SIZE;
    if (dirEndOff + 1 >= SIZE) return false;

    std::uint16_t freePtr = readAt<std::uint16_t>(buf_.data(), OFF_FREE_PTR);
    if (freePtr == 0) {
        // First insert into this page: freePtr starts at SIZE.
        freePtr = SIZE;
    }
    if (bytes.size() > static_cast<std::size_t>(freePtr) - dirEndOff) {
        return false;  // not enough contiguous space
    }
    freePtr = static_cast<std::uint16_t>(freePtr - bytes.size());
    if (bytes.size() > 0) {
        std::memcpy(buf_.data() + freePtr, bytes.data(), bytes.size());
    }

    writeAt<std::uint16_t>(buf_.data(), OFF_FREE_PTR, freePtr);
    const SlotEntry e{freePtr, static_cast<std::uint16_t>(bytes.size())};
    writeSlot(*this, slot, e);
    if (slot >= n) {
        writeAt<std::uint16_t>(buf_.data(), OFF_SLOT_CNT, slot + 1);
    }
    setDirty(true);
    return true;
}

std::span<const std::uint8_t> Page::getRecord(std::uint16_t slot) const {
    if (slot >= slotCount()) return {};
    const SlotEntry e = slotOf(*this, slot);
    if (e.length == TOMBSTONE_LEN) return {};
    return std::span<const std::uint8_t>(buf_.data() + e.offset, e.length);
}

bool Page::deleteRecord(std::uint16_t slot) {
    if (slot >= slotCount()) return false;
    const SlotEntry e = slotOf(*this, slot);
    if (e.length == TOMBSTONE_LEN) return false;
    writeSlot(*this, slot, SlotEntry{0, TOMBSTONE_LEN});
    setDirty(true);
    return true;
}

} // namespace minidb::storage
