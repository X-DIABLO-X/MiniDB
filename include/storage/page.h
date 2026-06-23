// =============================================================================
// include/storage/page.h
// -----------------------------------------------------------------------------
// A 4 KB fixed-size page buffer with a slot directory. The layout is
// described in include/storage/README.md.
//
// The Page class is intentionally *not* aware of transactions or schema. It
// is a raw byte container with a slot directory. The catalog tells callers
// what the bytes inside the slots mean.
// =============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "common/types.h"
#include "common/status.h"

namespace minidb::storage {

class Page {
public:
    static constexpr std::uint32_t SIZE        = 4096;     // page size in bytes
    static constexpr std::uint16_t MAX_SLOTS   = 256;      // slot directory cap
    // Fixed header size = pageId(4) + pageLSN(8) + slotCount(2) +
    //                     freeSpacePtr(2) + dirty(1) + padding(47) = 64
    static constexpr std::uint16_t HEADER_SIZE = 64;

    Page();
    ~Page() = default;

    // Pages are movable (used by BufferPool frames), but copy is disabled
    // because the pin-count / dirty semantics are not copyable.
    Page(const Page&)            = delete;
    Page& operator=(const Page&) = delete;
    Page(Page&&)                 = default;
    Page& operator=(Page&&)      = default;

    // Identity
    PageId  getPageId() const;
    void    setPageId(PageId id);

    // Raw access. The returned span covers the whole 4 KB buffer.
    std::uint8_t*       data();
    const std::uint8_t* data() const;

    // Slot directory
    std::uint16_t slotCount() const;
    bool          insertRecord(std::uint16_t slot, std::span<const std::uint8_t> bytes);
    std::span<const std::uint8_t> getRecord(std::uint16_t slot) const;
    bool          deleteRecord(std::uint16_t slot);
    std::uint16_t firstFreeSlot() const;     // 0xFFFF if none

    // Recovery support
    std::uint64_t getPageLSN() const;
    void         setPageLSN(std::uint64_t lsn);

    // Misc
    void reset();                  // zero out, set pageId = INVALID_PAGE_ID
    bool isDirty() const;
    void setDirty(bool d);

private:
    std::array<std::uint8_t, SIZE> buf_{};
};

} // namespace minidb::storage
