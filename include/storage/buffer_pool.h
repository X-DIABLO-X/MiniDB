// =============================================================================
// include/storage/buffer_pool.h
// -----------------------------------------------------------------------------
// In-memory cache of pages. Pages are referenced by PageId; the pool
// transparently reads from / writes to the DiskManager.
//
// Pins:
//   - Every fetched page is returned with pin count > 0.
//   - Callers MUST call unpinPage() once they are done with the pointer.
//   - The Page* is only valid while the pin is held.
//
// Eviction policy: LRU. A victim must have pin count 0; if all frames are
// pinned the pool is full and fetchPage returns Status::FULL.
// =============================================================================
#pragma once

#include <list>
#include <mutex>
#include <unordered_map>

#include "common/status.h"
#include "common/types.h"
#include "storage/disk_manager.h"
#include "storage/page.h"

namespace minidb::storage {

class BufferPool {
public:
    explicit BufferPool(DiskManager* disk, std::size_t numFrames = 64);
    ~BufferPool();

    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Returns a pinned page in `outPage`. If the page is in cache, just
    // bump the pin count. Otherwise read it from disk, possibly evicting
    // a victim (writes back if dirty).
    Status fetchPage(PageId pageId, Page*& outPage);

    // Decrement pin count. If `isDirty` is true, mark the frame dirty so
    // eviction will write it back. The last unpin (count -> 0) makes the
    // frame evictable.
    Status unpinPage(PageId pageId, bool isDirty);

    // Force a frame to disk. Pin count must be 0 (else INVALID_ARGUMENT).
    Status flushPage(PageId pageId);

    // Flush every dirty frame. Called by the recovery manager at the end
    // of startup, and by the CLI on a graceful shutdown.
    Status flushAll();

    std::size_t numFrames() const { return numFrames_; }

    // Allocates a fresh page id (via the disk manager) and returns it.
    // The new page is not in the buffer pool until the caller fetchPage()s it.
    PageId allocatePage() { return disk_->allocatePage(); }

    // Access to the underlying disk manager (used by HeapFile to allocate
    // new pages for a table's chain when the current page is full).
    DiskManager* diskManager() { return disk_; }

private:
    struct Frame {
        Page        page;
        std::size_t pinCount = 0;
        bool        dirty    = false;
        // iterator into LRU list (front = most recently used). Default-
        // constructed so Frame is default-constructible (required by
        // std::unordered_map::operator[]; we only ever dereference it
        // after we have set it from a real list iterator).
        std::list<PageId>::iterator lruIt;
        Frame() = default;
    };

    DiskManager*                         disk_;
    std::size_t                          numFrames_;
    std::unordered_map<PageId, Frame>    table_;
    std::list<PageId>                    lru_;          // front = MRU
    std::mutex                           mu_;
};

} // namespace minidb::storage
