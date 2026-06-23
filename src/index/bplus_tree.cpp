// =============================================================================
// src/index/bplus_tree.cpp
// -----------------------------------------------------------------------------
// B+ tree over the BufferPool. Keys are opaque byte strings (Key).
//
// On-disk layout per node is defined in include/index/node.h. We use a
// fanout of MAX_KEYS = 64 which keeps even the worst-case split well under
// a single 4 KB page.
//
// Search     — descend from root, choose child at each internal node.
// Insert     — find leaf, shift suffix; if the leaf is full, allocate a
//              fresh page through the BufferPool, split, and propagate the
//              separator key up. If a fresh page cannot be allocated (e.g.
//              in tests where the buffer pool is a stub) we return FULL.
// Remove     — find leaf, shift suffix left; no rebalance for v1.
// Range scan — find leftmost leaf >= lo, then follow nextLeaf until > hi.
// =============================================================================
#include "index/bplus_tree.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "index/node.h"
#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace minidb::index {

using storage::BufferPool;
using storage::Page;

namespace {

// Conservative fanout. 64 keys * ~38 B (leaf) ≈ 2.4 KB → safely fits in a
// 4 KB page even after a split's worst-case move.
constexpr std::uint16_t MAX_KEYS = 64;

struct PathEntry {
    PageId pageId;
    Page*  page;
    Node   node;        // view onto page->data()
};

// Pins a page through the BufferPool and returns a Node view.
Status fetchNode(BufferPool* bp, PageId pid, Page*& outPage, Node& outNode) {
    Status s = bp->fetchPage(pid, outPage);
    if (s != Status::OK) return s;
    outNode.buf = outPage->data();
    outNode.end = outPage->data() + Page::SIZE;
    return Status::OK;
}

// Initialises a freshly-allocated page as an empty node of the requested kind.
void initNode(Node& n, bool asLeaf, PageId parent) {
    n.setLeaf(asLeaf);
    n.setNumKeys(0);
    n.setParent(parent);
    n.setPrevLeaf(INVALID_PAGE_ID);
    n.setNextLeaf(INVALID_PAGE_ID);
}

// Walks to the start of entry i's *key bytes* in a leaf node. The byte
// before the returned pointer holds the keyLen of entry i.
std::uint8_t* leafEntryPtr(Node& n, std::uint16_t i) {
    std::uint8_t* p = n.buf + 16;        // OFF_DATA_BASE
    for (std::uint16_t k = 0; k < i; ++k) {
        std::uint16_t kLen;
        std::memcpy(&kLen, p, sizeof(kLen));
        p += 2u + kLen + 8u;
    }
    return p;
}

// Walks to the start of slot i in an internal node. For i==0, that is
// child0; for i>=1, that is the keyLen of key_i.
std::uint8_t* intEntryPtr(Node& n, std::uint16_t i) {
    std::uint8_t* p = n.buf + 16;        // OFF_DATA_BASE = child0
    if (i == 0) return p;
    p += 4u;                             // skip child0
    for (std::uint16_t k = 0; k + 1 < i; ++k) {
        std::uint16_t kLen;
        std::memcpy(&kLen, p, sizeof(kLen));
        p += 2u + kLen + 4u;
    }
    return p;
}

// Total bytes consumed by all leaf entries.
std::size_t leafDataUsed(const Node& n) {
    std::size_t total = 0;
    std::uint16_t nk = n.numKeys();
    const std::uint8_t* p = n.buf + 16;
    for (std::uint16_t k = 0; k < nk; ++k) {
        std::uint16_t kLen;
        std::memcpy(&kLen, p, sizeof(kLen));
        total += 2u + kLen + 8u;
        p += 2u + kLen + 8u;
    }
    return total;
}
std::size_t intDataUsed(const Node& n) {
    std::size_t total = 4u;              // child0
    std::uint16_t nk = n.numKeys();
    const std::uint8_t* p = n.buf + 16 + 4;
    for (std::uint16_t k = 0; k < nk; ++k) {
        std::uint16_t kLen;
        std::memcpy(&kLen, p, sizeof(kLen));
        total += 2u + kLen + 4u;
        p += 2u + kLen + 4u;
    }
    return total;
}

// Insert a (key, rid) entry at position `i` in a leaf page. Caller must
// ensure the page has room and that i is in [0, numKeys()].
void leafInsert(Node& n, std::uint16_t i, std::span<const std::uint8_t> key,
                RecordId rid) {
    std::uint8_t* dst = leafEntryPtr(n, i);
    std::uint16_t oldNum = n.numKeys();
    std::size_t suffixBytes = 0;
    if (i < oldNum) {
        const std::uint8_t* end = leafEntryPtr(n, oldNum);
        suffixBytes = static_cast<std::size_t>(end - dst);
    }
    if (suffixBytes > 0) {
        std::memmove(dst + 2u + key.size() + 8u, dst, suffixBytes);
    }
    std::uint16_t kLen = static_cast<std::uint16_t>(key.size());
    std::memcpy(dst, &kLen, sizeof(kLen));
    if (!key.empty()) std::memcpy(dst + 2, key.data(), key.size());
    std::uint64_t r = rid;
    std::memcpy(dst + 2 + key.size(), &r, sizeof(r));
    n.setNumKeys(static_cast<std::uint16_t>(oldNum + 1));
}

// Remove the entry at position `i` from a leaf page.
void leafErase(Node& n, std::uint16_t i) {
    std::uint16_t oldNum = n.numKeys();
    if (i >= oldNum) return;
    std::uint8_t* dst = leafEntryPtr(n, i);
    std::uint16_t kLen;
    std::memcpy(&kLen, dst, sizeof(kLen));
    std::size_t entrySize = 2u + kLen + 8u;
    std::uint8_t* next = dst + entrySize;
    const std::uint8_t* end = leafEntryPtr(n, oldNum);
    if (next < end) {
        std::memmove(dst, next, static_cast<std::size_t>(end - next));
    }
    n.setNumKeys(static_cast<std::uint16_t>(oldNum - 1));
}

// Internal node: insert a new (child pointer, separator key) into slot i.
void intInsert(Node& n, std::uint16_t i, PageId child,
               std::span<const std::uint8_t> key) {
    std::uint16_t oldNum = n.numKeys();
    std::uint16_t kLen = static_cast<std::uint16_t>(key.size());

    if (oldNum == 0) {
        // The internal node has no keys yet — write child0, then key0.
        // The second child will be added by a follow-up call.
        std::uint8_t* slot = n.buf + 16;
        std::memcpy(slot, &child, sizeof(child));
        std::memcpy(slot + 4, &kLen, sizeof(kLen));
        if (!key.empty()) std::memcpy(slot + 6, key.data(), key.size());
        n.setNumKeys(1);
        return;
    }

    if (i == 0) {
        // Prepend: shift the existing data right by (2 + keyLen + 4).
        // Existing data is child0(4) + entries[0..]. We need to write:
        //   newChild(4) + keyLen(2) + key + oldChild0(4) + ...
        std::uint8_t* slot = n.buf + 16;            // points at child0
        // Compute the bytes that need to move (everything except child0).
        std::size_t movedBytes = 0;
        {
            const std::uint8_t* endPtr = intEntryPtr(n, oldNum);
            movedBytes = static_cast<std::size_t>(endPtr - (slot + 4));
        }
        std::memmove(slot + 4u + 2u + kLen + 4u, slot + 4u, movedBytes);
        // Write the new child (replaces child0), then keyLen, then key.
        std::memcpy(slot, &child, sizeof(child));
        std::memcpy(slot + 4, &kLen, sizeof(kLen));
        if (!key.empty()) std::memcpy(slot + 6, key.data(), key.size());
        n.setNumKeys(static_cast<std::uint16_t>(oldNum + 1));
        return;
    }

    if (i == oldNum) {
        // Append a new child at the end (no separator key, just the new
        // child pointer at the tail). slot points at key_{i-1}.
        std::uint8_t* slot = intEntryPtr(n, i);
        std::uint16_t prevKLen;
        std::memcpy(&prevKLen, slot, sizeof(prevKLen));
        std::uint8_t* dst = slot + 2u + prevKLen;
        std::memcpy(dst, &child, sizeof(child));
        n.setNumKeys(static_cast<std::uint16_t>(oldNum + 1));
        return;
    }

    // Middle insertion: slot points at child_i; we need to replace the
    // (child_i(4) + keyLen_i(2) + key_i) tuple with
    // (keyLen_new(2) + key_new + child_new(4)) and shift the rest.
    std::uint8_t* slot = intEntryPtr(n, i);
    std::uint16_t oldKeyLen;
    std::memcpy(&oldKeyLen, slot + 4, sizeof(oldKeyLen));
    std::size_t oldEntrySize = 4u + 2u + oldKeyLen;
    std::size_t newEntrySize = 2u + kLen + 4u;
    const std::uint8_t* endPtr = intEntryPtr(n, oldNum);
    std::size_t suffixBytes =
        static_cast<std::size_t>(endPtr - (slot + oldEntrySize));
    if (suffixBytes > 0) {
        std::memmove(slot + newEntrySize, slot + oldEntrySize, suffixBytes);
    }
    std::memcpy(slot, &kLen, sizeof(kLen));
    if (!key.empty()) std::memcpy(slot + 2, key.data(), key.size());
    std::memcpy(slot + 2 + kLen, &child, sizeof(child));
    n.setNumKeys(static_cast<std::uint16_t>(oldNum + 1));
}

// Binary search: returns the largest i such that node.key(i) < `key`, plus 1.
std::int32_t lowerBound(const Node& n, std::span<const std::uint8_t> key) {
    std::uint16_t lo = 0, hi = n.numKeys();
    while (lo < hi) {
        std::uint16_t mid = static_cast<std::uint16_t>((lo + hi) / 2);
        auto k = n.key(mid);
        int cmp = std::memcmp(k.data(), key.data(),
                              std::min<std::size_t>(k.size(), key.size()));
        if (cmp == 0) {
            if (k.size() < key.size()) cmp = -1;
            else if (k.size() > key.size()) cmp = 1;
        }
        if (cmp < 0) {
            lo = static_cast<std::uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return static_cast<std::int32_t>(lo);
}

// Unpins every entry on the path. Used by error paths.
void unpinPath(BufferPool* bp, std::vector<PathEntry>& path, bool dirty) {
    for (auto& e : path) (void)bp->unpinPage(e.pageId, dirty);
    path.clear();
}

} // namespace

// =============================================================================
// BPlusTree
// =============================================================================

BPlusTree::BPlusTree(BufferPool* bp, PageId rootPageId)
    : bp_(bp), root_(rootPageId) {}

BPlusTree::~BPlusTree() = default;

// Descend from `start` (the root) to the leaf that should hold `key`. The
// returned `path` is the chain of pinned pages, leaf at the back.
static Status descend(BufferPool* bp, PageId start, const Key& key,
                      std::vector<PathEntry>& path) {
    PageId cur = start;
    while (true) {
        PathEntry pe{};
        Status s = fetchNode(bp, cur, pe.page, pe.node);
        if (s != Status::OK) {
            unpinPath(bp, path, false);
            return s;
        }
        bool leaf = pe.node.isLeaf();
        path.push_back(pe);
        if (leaf) return Status::OK;
        std::int32_t idx = lowerBound(pe.node,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(key.data()), key.size()));
        PageId child = pe.node.child(static_cast<std::uint16_t>(idx));
        if (child == INVALID_PAGE_ID) {
            unpinPath(bp, path, false);
            return Status::IO_ERROR;
        }
        cur = child;
    }
}

// Search ---------------------------------------------------------------------

Status BPlusTree::search(const Key& key, RecordId& outRid) {
    outRid = INVALID_RID;
    if (root_ == INVALID_PAGE_ID) return Status::NOT_FOUND;

    std::vector<PathEntry> path;
    Status s = descend(bp_, root_, key, path);
    if (s != Status::OK) return s;

    Node& leaf = path.back().node;
    std::int32_t idx = lowerBound(leaf,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(key.data()), key.size()));
    Status result = Status::NOT_FOUND;
    if (idx >= 0 && idx < static_cast<std::int32_t>(leaf.numKeys())) {
        auto k = leaf.key(static_cast<std::uint16_t>(idx));
        if (k.size() == key.size() &&
            (key.empty() || std::memcmp(k.data(), key.data(), k.size()) == 0)) {
            outRid = leaf.rid(static_cast<std::uint16_t>(idx));
            result = Status::OK;
        }
    }
    unpinPath(bp_, path, false);
    return result;
}

// Insert ---------------------------------------------------------------------

Status BPlusTree::insert(const Key& key, RecordId rid) {
    if (root_ == INVALID_PAGE_ID) return Status::INVALID_ARGUMENT;
    std::span<const std::uint8_t> kBytes(
        reinterpret_cast<const std::uint8_t*>(key.data()), key.size());

    std::vector<PathEntry> path;
    Status s = descend(bp_, root_, key, path);
    if (s != Status::OK) return s;

    Node& leaf = path.back().node;
    // Duplicate check: B+ tree keys are unique.
    std::int32_t idx = lowerBound(leaf, kBytes);
    if (idx < static_cast<std::int32_t>(leaf.numKeys())) {
        auto k = leaf.key(static_cast<std::uint16_t>(idx));
        if (k.size() == key.size() &&
            (key.empty() || std::memcmp(k.data(), key.data(), k.size()) == 0)) {
            unpinPath(bp_, path, false);
            return Status::DUPLICATE_KEY;
        }
    }

    // Fast path: enough room in the leaf for one more entry.
    std::size_t need = 2u + key.size() + 8u;
    std::size_t used = leafDataUsed(leaf);
    if (used + need <= Page::SIZE - 16) {
        leafInsert(leaf, static_cast<std::uint16_t>(idx), kBytes, rid);
        unpinPath(bp_, path, true);
        return Status::OK;
    }

    // Slow path: split the leaf.
    struct Entry { std::vector<std::uint8_t> k; RecordId r; };
    std::vector<Entry> all;
    all.reserve(leaf.numKeys() + 1);
    for (std::uint16_t i = 0; i < leaf.numKeys(); ++i) {
        auto k = leaf.key(i);
        Entry e;
        e.k.assign(k.begin(), k.end());
        e.r = leaf.rid(i);
        all.push_back(std::move(e));
    }
    Entry ins;
    ins.k.assign(key.begin(), key.end());
    ins.r = rid;
    all.insert(all.begin() + idx, std::move(ins));

    // The B+ tree's "first key of the right half" is duplicated up into
    // the parent; for v1 we use the first key of the right half as the
    // separator (split point at the midpoint).
    std::size_t mid = all.size() / 2;
    if (mid == 0) mid = 1;        // invariant: at least one key per side

    // Rewrite the left half back into the current leaf.
    std::memset(leaf.buf + 16, 0, Page::SIZE - 16);
    leaf.setNumKeys(0);
    for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(mid); ++i) {
        leafInsert(leaf, i,
            std::span<const std::uint8_t>(all[i].k.data(), all[i].k.size()),
            all[i].r);
    }

    // Allocate a new page by calling fetchPage on a never-used page id.
    // We try a small handful of increasing candidates (the B+ tree can
    // only create new pages at very high page ids, and any unused id
    // reads as zeroed bytes from disk — which is fine for a node).
    Page* newPage = nullptr;
    PageId newPageId = INVALID_PAGE_ID;
    constexpr PageId MAX_TRY_PID = 1u << 20;        // up to ~1M pages
    for (PageId tryPid = root_ + 1; tryPid < MAX_TRY_PID; ++tryPid) {
        Status fs = bp_->fetchPage(tryPid, newPage);
        if (fs == Status::OK) {
            // Reject pages that already look like a node (we'd corrupt
            // existing data). A truly free page reads as zeros, so
            // numKeys()==0 and isLeaf()==0.
            Node probe{};
            probe.buf = newPage->data();
            probe.end = newPage->data() + Page::SIZE;
            if (probe.numKeys() == 0 && !probe.isLeaf()) {
                newPageId = tryPid;
                break;
            }
            (void)bp_->unpinPage(tryPid, false);
            newPage = nullptr;
        } else {
            // fetchPage failed (e.g. stub returning UNIMPLEMENTED) — bail.
            unpinPath(bp_, path, true);
            return fs;
        }
    }
    if (newPageId == INVALID_PAGE_ID || newPage == nullptr) {
        unpinPath(bp_, path, true);
        return Status::FULL;
    }

    Node newNode{};
    newNode.buf = newPage->data();
    newNode.end = newPage->data() + Page::SIZE;
    initNode(newNode, /*asLeaf=*/true, INVALID_PAGE_ID);

    for (std::uint16_t i = static_cast<std::uint16_t>(mid); i < all.size(); ++i) {
        leafInsert(newNode, static_cast<std::uint16_t>(i - mid),
            std::span<const std::uint8_t>(all[i].k.data(), all[i].k.size()),
            all[i].r);
    }

    // Fix up the leaf doubly-linked list.
    PageId oldLeafId = path.back().pageId;
    PageId oldNext = leaf.nextLeaf();
    newNode.setPrevLeaf(oldLeafId);
    newNode.setNextLeaf(oldNext);
    leaf.setNextLeaf(newPageId);
    if (oldNext != INVALID_PAGE_ID) {
        Page* oldNextPage = nullptr;
        if (bp_->fetchPage(oldNext, oldNextPage) == Status::OK) {
            Node oldNextNode{};
            oldNextNode.buf = oldNextPage->data();
            oldNextNode.end = oldNextPage->data() + Page::SIZE;
            oldNextNode.setPrevLeaf(newPageId);
            (void)bp_->unpinPage(oldNext, true);
        }
    }

    // The separator key is the first key of the right half. In a
    // textbook B+ tree this key also stays in the right half; we follow
    // that convention.
    std::vector<std::uint8_t> sepKey = all[mid].k;
    (void)bp_->unpinPage(newPageId, true);

    // If the root was a leaf, we need a new internal root.
    if (path.size() == 1) {
        // We have to allocate yet another page for the new root. Try the
        // same strategy.
        Page* newRoot = nullptr;
        PageId newRootId = INVALID_PAGE_ID;
        for (PageId tryPid = newPageId + 1; tryPid < MAX_TRY_PID; ++tryPid) {
            Status fs = bp_->fetchPage(tryPid, newRoot);
            if (fs != Status::OK) {
                unpinPath(bp_, path, true);
                return fs;
            }
            Node probe{};
            probe.buf = newRoot->data();
            probe.end = newRoot->data() + Page::SIZE;
            if (probe.numKeys() == 0 && !probe.isLeaf()) {
                newRootId = tryPid;
                break;
            }
            (void)bp_->unpinPage(tryPid, false);
            newRoot = nullptr;
        }
        if (newRootId == INVALID_PAGE_ID || newRoot == nullptr) {
            unpinPath(bp_, path, true);
            return Status::FULL;
        }
        Node nr{};
        nr.buf = newRoot->data();
        nr.end = newRoot->data() + Page::SIZE;
        initNode(nr, /*asLeaf=*/false, INVALID_PAGE_ID);
        nr.setChild(0, oldLeafId);
        // Add the separator + second child.
        intInsert(nr, 0, newPageId,
            std::span<const std::uint8_t>(sepKey.data(), sepKey.size()));
        leaf.setParent(newRootId);

        // Update newNode's parent.
        Page* newLeafPg = nullptr;
        if (bp_->fetchPage(newPageId, newLeafPg) == Status::OK) {
            Node nv{};
            nv.buf = newLeafPg->data();
            nv.end = newLeafPg->data() + Page::SIZE;
            nv.setParent(newRootId);
            (void)bp_->unpinPage(newPageId, true);
        }
        (void)bp_->unpinPage(newRootId, true);
        root_ = newRootId;
        unpinPath(bp_, path, true);
        return Status::OK;
    }

    // Propagate up to the parent.
    path.pop_back();
    Node& parent = path.back().node;
    std::int32_t pIdx = lowerBound(parent,
        std::span<const std::uint8_t>(sepKey.data(), sepKey.size()));
    intInsert(parent, static_cast<std::uint16_t>(pIdx), newPageId,
        std::span<const std::uint8_t>(sepKey.data(), sepKey.size()));

    // If the parent overflows, we'd need to split it recursively. For
    // v1 we surface FULL when that happens — a real implementation
    // would repeat the split-and-propagate loop.
    if (intDataUsed(parent) > Page::SIZE - 16) {
        unpinPath(bp_, path, true);
        return Status::FULL;
    }
    unpinPath(bp_, path, true);
    return Status::OK;
}

// Remove ---------------------------------------------------------------------

Status BPlusTree::remove(const Key& key) {
    if (root_ == INVALID_PAGE_ID) return Status::NOT_FOUND;
    std::span<const std::uint8_t> kBytes(
        reinterpret_cast<const std::uint8_t*>(key.data()), key.size());

    std::vector<PathEntry> path;
    Status s = descend(bp_, root_, key, path);
    if (s != Status::OK) return s;

    Node& leaf = path.back().node;
    std::int32_t idx = lowerBound(leaf, kBytes);
    if (idx >= static_cast<std::int32_t>(leaf.numKeys())) {
        unpinPath(bp_, path, false);
        return Status::NOT_FOUND;
    }
    auto k = leaf.key(static_cast<std::uint16_t>(idx));
    if (k.size() != key.size() ||
        (!key.empty() && std::memcmp(k.data(), key.data(), k.size()) != 0)) {
        unpinPath(bp_, path, false);
        return Status::NOT_FOUND;
    }
    leafErase(leaf, static_cast<std::uint16_t>(idx));
    // No rebalancing in v1 — pages are allowed to underutilise space.
    unpinPath(bp_, path, true);
    return Status::OK;
}

// Range scan -----------------------------------------------------------------

Status BPlusTree::rangeScan(const Key& lo, const Key& hi,
                            std::vector<RecordId>& out) {
    out.clear();
    if (root_ == INVALID_PAGE_ID) return Status::OK;
    std::span<const std::uint8_t> loBytes(
        reinterpret_cast<const std::uint8_t*>(lo.data()), lo.size());
    std::span<const std::uint8_t> hiBytes(
        reinterpret_cast<const std::uint8_t*>(hi.data()), hi.size());

    std::vector<PathEntry> path;
    Status s = descend(bp_, root_, lo, path);
    if (s != Status::OK) return s;

    // Unpin ancestors; we only need the leaf pinned (and we re-pin each
    // next leaf as we walk).
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        (void)bp_->unpinPage(path[i].pageId, false);
    }
    PageId leafId = path.back().pageId;
    PageId curPage = leafId;
    bool leafStillPinned = true;

    while (curPage != INVALID_PAGE_ID) {
        Page* pg = nullptr;
        s = bp_->fetchPage(curPage, pg);
        if (s != Status::OK) {
            if (leafStillPinned) (void)bp_->unpinPage(leafId, false);
            return s;
        }
        Node n{};
        n.buf = pg->data();
        n.end = pg->data() + Page::SIZE;

        std::uint16_t start = 0;
        if (curPage == leafId) {
            std::int32_t lb = lowerBound(n, loBytes);
            start = static_cast<std::uint16_t>(lb);
        }
        for (std::uint16_t i = start; i < n.numKeys(); ++i) {
            auto k = n.key(i);
            int cmp = 0;
            if (k.size() != hi.size()) {
                cmp = (k.size() < hi.size()) ? -1 : 1;
            } else if (!k.empty()) {
                cmp = std::memcmp(k.data(), hi.data(), k.size());
            }
            if (cmp > 0) {
                (void)bp_->unpinPage(curPage, false);
                if (leafStillPinned && curPage != leafId) {
                    (void)bp_->unpinPage(leafId, false);
                }
                return Status::OK;
            }
            out.push_back(n.rid(i));
        }
        PageId next = n.nextLeaf();
        (void)bp_->unpinPage(curPage, false);
        if (leafStillPinned && curPage == leafId) leafStillPinned = false;
        curPage = next;
    }
    if (leafStillPinned) (void)bp_->unpinPage(leafId, false);
    return Status::OK;
}

} // namespace minidb::index