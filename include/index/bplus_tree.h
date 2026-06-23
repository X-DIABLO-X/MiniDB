// =============================================================================
// include/index/bplus_tree.h
// -----------------------------------------------------------------------------
// B+ tree on top of the BufferPool. Keys are opaque byte strings (Key).
// =============================================================================
#pragma once

#include <vector>

#include "common/status.h"
#include "common/types.h"
#include "storage/buffer_pool.h"

namespace minidb::index {

class BPlusTree {
public:
    BPlusTree(storage::BufferPool* bp, PageId rootPageId);
    ~BPlusTree();

    BPlusTree(const BPlusTree&)            = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    Status search        (const Key& key, RecordId& outRid);
    Status insert        (const Key& key, RecordId rid);
    Status remove        (const Key& key);
    Status rangeScan     (const Key& lo, const Key& hi,
                          std::vector<RecordId>& out);

    PageId rootPageId() const { return root_; }

private:
    storage::BufferPool* bp_;
    PageId               root_;
};

} // namespace minidb::index