// =============================================================================
// src/storage/buffer_pool.cpp
// -----------------------------------------------------------------------------
// Fixed-size LRU cache. Every page in cache lives in a Frame. Frames are
// referenced from two structures:
//   * `table_`  : PageId -> Frame      (for O(1) lookup)
//   * `lru_`    : list of PageIds      (MRU at the front)
//
// `fetchPage` either bumps the pin count of an existing frame, or evicts a
// victim (the LRU unpinned frame), reads the new page from disk, and
// returns a pinned Frame. The caller must call `unpinPage` exactly once
// per successful fetch.
// =============================================================================
#include "storage/buffer_pool.h"

#include <stdexcept>
#include <utility>

namespace minidb::storage {

BufferPool::BufferPool(DiskManager* disk, std::size_t numFrames)
    : disk_(disk), numFrames_(numFrames == 0 ? 1 : numFrames) {}

BufferPool::~BufferPool() {
    // Best-effort: flush any remaining dirty frames so the DB on disk
    // is consistent with the cached state.
    (void)flushAll();
}

Status BufferPool::fetchPage(PageId pageId, Page*& outPage) {
    std::lock_guard<std::mutex> lk(mu_);

    // Hot path: already in cache.
    auto it = table_.find(pageId);
    if (it != table_.end()) {
        it->second.pinCount += 1;
        // Move to MRU.
        lru_.erase(it->second.lruIt);
        lru_.push_front(pageId);
        it->second.lruIt = lru_.begin();
        outPage = &it->second.page;
        return Status::OK;
    }

    // Need a free frame. If we are at capacity, evict.
    if (table_.size() >= numFrames_) {
        auto vit = lru_.rbegin();           // LRU = back of the list
        Frame*  victim = nullptr;
        for (; vit != lru_.rend(); ++vit) {
            auto fit = table_.find(*vit);
            if (fit == table_.end()) continue;
            if (fit->second.pinCount == 0) { victim = &fit->second; break; }
        }
        if (victim == nullptr) {
            outPage = nullptr;
            return Status::FULL;
        }
        // Flush if dirty.
        if (victim->dirty) {
            (void)disk_->writePage(victim->page.getPageId(), victim->page);
            victim->dirty = false;
        }
        const PageId victimId = victim->page.getPageId();
        lru_.erase(victim->lruIt);
        table_.erase(victimId);
    }

    // Allocate a new frame and load the page from disk.
    Frame f;
    f.page.reset();
    Status s = disk_->readPage(pageId, f.page);
    if (s != Status::OK) {
        outPage = nullptr;
        return s;
    }
    f.page.setPageId(pageId);
    f.pinCount = 1;
    f.dirty    = false;
    lru_.push_front(pageId);
    f.lruIt = lru_.begin();

    auto [insIt, ok] = table_.emplace(pageId, std::move(f));
    outPage = &insIt->second.page;
    return Status::OK;
}

Status BufferPool::unpinPage(PageId pageId, bool isDirty) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = table_.find(pageId);
    if (it == table_.end()) return Status::NOT_FOUND;
    if (it->second.pinCount == 0) return Status::INVALID_ARGUMENT;
    it->second.pinCount -= 1;
    if (isDirty) it->second.dirty = true;
    return Status::OK;
}

Status BufferPool::flushPage(PageId pageId) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = table_.find(pageId);
    if (it == table_.end()) return Status::OK;  // not cached: nothing to do
    if (it->second.pinCount != 0) return Status::INVALID_ARGUMENT;
    if (!it->second.dirty) return Status::OK;
    Status s = disk_->writePage(it->second.page.getPageId(), it->second.page);
    if (s == Status::OK) it->second.dirty = false;
    return s;
}

Status BufferPool::flushAll() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [pid, frame] : table_) {
        if (frame.dirty) {
            (void)disk_->writePage(pid, frame.page);
            frame.dirty = false;
        }
    }
    disk_->flush();
    return Status::OK;
}

} // namespace minidb::storage