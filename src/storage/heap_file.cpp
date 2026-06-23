// =============================================================================
// src/storage/heap_file.cpp
// -----------------------------------------------------------------------------
// Heap file = a chain of pages, each holding a slot directory. The first
// page id of a table is stored in TableInfo::firstPageId; the chain is
// implicit because each page's slot directory has no "next" pointer at the
// v1 level — we discover overflow by failing to find a free slot and
// allocating a new page via the buffer pool.
//
// When the table is freshly created the first page is allocated lazily on
// the first insert, and its id is written back into the TableInfo (the
// caller is responsible for persisting the TableInfo via the catalog).
// =============================================================================
#include "storage/heap_file.h"

#include "catalog/schema.h"
#include "catalog/table_info.h"
#include "common/record_id.h"
#include "common/types.h"
#include <stdexcept>

namespace minidb::storage {

HeapFile::Iterator::~Iterator() = default;

HeapFile::HeapFile(BufferPool* bp, const catalog::TableInfo* info)
    : bp_(bp), info_(info) {}

HeapFile::~HeapFile() = default;

Status HeapFile::insertRecord(std::span<const std::uint8_t> record, RecordId& outRid) {
    outRid = INVALID_RID;
    if (bp_ == nullptr || info_ == nullptr) return Status::INVALID_ARGUMENT;
    if (info_->firstPageId == 0) {
        // The catalog must allocate the first page before the heap file
        // can be used. We surface this as a usage error.
        return Status::INVALID_ARGUMENT;
    }

    // Walk the chain. The chain in v1 is the single page; if it is full we
    // allocate a new one. We do not maintain a "next" pointer on the page
    // because the catalog can be told the head id; v2 may revisit this.
    PageId curr = info_->firstPageId;
    while (true) {
        Page* page = nullptr;
        Status s = bp_->fetchPage(curr, page);
        if (s != Status::OK) return s;
        if (page == nullptr) { (void)bp_->unpinPage(curr, false); return Status::IO_ERROR; }

        const std::uint16_t freeSlot = page->firstFreeSlot();
        if (freeSlot != 0xFFFF) {
            // Try to insert; if it doesn't fit, fall through and allocate a new page.
            if (page->insertRecord(freeSlot, record)) {
                outRid = makeRid(curr, freeSlot);
                return bp_->unpinPage(curr, true);
            }
        }
        // Page is full: append a new page and continue. v1 ignores a real
        // linked-list "next" pointer and just keeps allocating.
        (void)bp_->unpinPage(curr, false);
        curr = bp_->allocatePage();
    }
}

Status HeapFile::getRecord(RecordId rid, std::span<const std::uint8_t>& out) const {
    out = {};
    if (bp_ == nullptr) return Status::INVALID_ARGUMENT;
    const PageId  pid  = ridPage(rid);
    const std::uint16_t slot = ridSlot(rid);
    Page* page = nullptr;
    Status s = bp_->fetchPage(pid, page);
    if (s != Status::OK) return s;
    if (page == nullptr) {
        (void)bp_->unpinPage(pid, false);
        return Status::IO_ERROR;
    }
    out = page->getRecord(slot);
    return bp_->unpinPage(pid, false);
}

Status HeapFile::deleteRecord(RecordId rid) {
    if (bp_ == nullptr) return Status::INVALID_ARGUMENT;
    const PageId  pid  = ridPage(rid);
    const std::uint16_t slot = ridSlot(rid);
    Page* page = nullptr;
    Status s = bp_->fetchPage(pid, page);
    if (s != Status::OK) return s;
    if (page == nullptr) {
        (void)bp_->unpinPage(pid, false);
        return Status::IO_ERROR;
    }
    const bool ok = page->deleteRecord(slot);
    (void)bp_->unpinPage(pid, true);
    return ok ? Status::OK : Status::NOT_FOUND;
}

std::unique_ptr<HeapFile::Iterator> HeapFile::scan() {
    auto it = std::make_unique<Iterator>();
    it->bp_   = bp_;
    it->page_ = info_->firstPageId;
    it->slot_ = 0;
    return it;
}

bool HeapFile::Iterator::next(RecordId& rid, std::span<const std::uint8_t>& bytes) {
    if (bp_ == nullptr || page_ == 0) return false;
    while (true) {
        Page* p = nullptr;
        if (bp_->fetchPage(page_, p) != Status::OK) return false;
        if (p == nullptr) {
            (void)bp_->unpinPage(page_, false);
            return false;
        }
        // We need the slot count to know when to advance pages. We don't
        // unpin until we find a live record or we exhaust the chain.
        const std::uint16_t n = p->slotCount();
        while (slot_ < n) {
            const std::uint16_t here = slot_;
            ++slot_;
            auto sp = p->getRecord(here);
            if (!sp.empty()) {
                rid = makeRid(page_, here);
                bytes = sp;
                return true;  // we keep this page pinned; close() will unpin
            }
        }
        // Out of slots on this page; unpin and stop at the end of the
        // single-page chain (v1).
        (void)bp_->unpinPage(page_, false);
        page_ = 0;
        return false;
    }
}

void HeapFile::Iterator::close() {
    if (bp_ == nullptr) return;
    if (page_ != 0) {
        (void)bp_->unpinPage(page_, false);
        page_ = 0;
    }
}

} // namespace minidb::storage