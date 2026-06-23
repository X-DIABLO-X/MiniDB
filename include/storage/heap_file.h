// =============================================================================
// include/storage/heap_file.h
// -----------------------------------------------------------------------------
// A heap file is a chain of pages (linked through a "next" pointer in the
// page header) holding variable-size records. The first page of a heap
// file is stored in TableInfo::firstPageId; new pages are allocated via
// the DiskManager and appended to the chain.
//
// RecordId = (pageId, slotIdx). A deleted slot is left in the slot
// directory marked as "tombstone" so the slot id stays stable for indexes
// that still hold a reference.
// =============================================================================
#pragma once

#include <memory>
#include <span>

#include "common/status.h"
#include "common/types.h"
#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace minidb::catalog { struct TableInfo; }

namespace minidb::storage {

class HeapFile {
public:
    HeapFile(BufferPool* bp, const catalog::TableInfo* info);
    ~HeapFile();

    HeapFile(const HeapFile&)            = delete;
    HeapFile& operator=(const HeapFile&) = delete;

    // Inserts `record` into the table and returns its RecordId.
    Status insertRecord(std::span<const std::uint8_t> record, RecordId& outRid);

    // Materialises the record at `rid` into `out`. The span is valid until
    // the next fetchPage/unpinPage on the underlying page.
    Status getRecord(RecordId rid, std::span<const std::uint8_t>& out) const;

    // Tombstones the record at `rid`. Storage is not reclaimed until a
    // future page-level compaction (out of scope for the demo).
    Status deleteRecord(RecordId rid);

    // Returns a forward iterator over all *live* records. Used by SeqScan.
    class Iterator {
    public:
        bool next(RecordId& rid, std::span<const std::uint8_t>& bytes);
        void close();
        ~Iterator();
    private:
        friend class HeapFile;
        BufferPool*         bp_   = nullptr;
        PageId              page_ = 0;
        std::uint16_t       slot_ = 0;
    };

    std::unique_ptr<Iterator> scan();

private:
    BufferPool*                    bp_;
    const catalog::TableInfo*      info_;     // firstPageId is set by the catalog
};

} // namespace minidb::storage
