// =============================================================================
// src/index/index_manager.cpp
// -----------------------------------------------------------------------------
// Owns the (indexName → BPlusTree) map. Persists (name, table, column,
// rootPageId) tuples to a dedicated metadata page so that an IndexManager
// can be reopened in a later session.
//
// On-disk layout of the metadata page (fixed 4 KB):
//
//   +0   magic[6]      = {'I','M','D','B',0,0}
//   +6   uint16 version
//   +8   uint16 count
//   +12  entries:   for each entry
//         uint16 nameLen, name bytes,
//         uint16 tableLen, table bytes,
//         uint16 columnLen, column bytes,
//         uint32 rootPageId
//
// Page 1 is reserved for the metadata; index roots start at page 2.
// =============================================================================
#include "index/index_manager.h"

#include <algorithm>
#include <cstring>
#include <mutex>
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

constexpr PageId       META_PAGE_ID   = 1;
constexpr PageId       FIRST_ROOT_PID = 2;
constexpr std::array<char, 6> MAGIC = {'I','M','D','B', 0, 0};

// Minimal raw read/write helpers (host endian).
inline void writeU16(std::uint8_t*& p, std::uint16_t v) {
    std::memcpy(p, &v, sizeof(v));
    p += sizeof(v);
}
inline void writeU32(std::uint8_t*& p, std::uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
    p += sizeof(v);
}
inline void writeStr(std::uint8_t*& p, const std::string& s) {
    std::uint16_t n = static_cast<std::uint16_t>(s.size());
    writeU16(p, n);
    if (n) std::memcpy(p, s.data(), n);
    p += n;
}
inline std::uint16_t readU16(const std::uint8_t*& p) {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    p += sizeof(v);
    return v;
}
inline std::uint32_t readU32(const std::uint8_t*& p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    p += sizeof(v);
    return v;
}
inline std::string readStr(const std::uint8_t*& p) {
    std::uint16_t n = readU16(p);
    std::string out;
    if (n) {
        out.assign(reinterpret_cast<const char*>(p), n);
        p += n;
    }
    return out;
}

// Allocates a never-used page id by scanning forward from `start`. A truly
// free page reads as zeros (numKeys==0, isLeaf==0, parent==0). Returns
// INVALID_PAGE_ID and outPage=nullptr if the underlying BufferPool cannot
// serve us (e.g. it is still a stub).
PageId allocateNodePage(BufferPool* bp, Page*& outPage, PageId start) {
    constexpr PageId MAX_TRY = 1u << 20;
    outPage = nullptr;
    for (PageId tryPid = start; tryPid < MAX_TRY; ++tryPid) {
        if (tryPid == META_PAGE_ID) continue;
        Page* pg = nullptr;
        Status s = bp->fetchPage(tryPid, pg);
        if (s != Status::OK) return INVALID_PAGE_ID;
        Node probe{};
        probe.buf = pg->data();
        probe.end = pg->data() + Page::SIZE;
        if (probe.numKeys() == 0 && !probe.isLeaf() &&
            probe.parent() == INVALID_PAGE_ID) {
            outPage = pg;
            return tryPid;
        }
        (void)bp->unpinPage(tryPid, false);
    }
    return INVALID_PAGE_ID;
}

} // namespace

// =============================================================================
// IndexManager
// =============================================================================

IndexManager::IndexManager(BufferPool* bp, catalog::CatalogManager* cat)
    : bp_(bp), cat_(cat) {
    std::lock_guard<std::mutex> lk(mu_);

    // Try to load existing metadata. A stub BufferPool returns IO_ERROR or
    // UNIMPLEMENTED; both are treated as "fresh DB".
    Page* pg = nullptr;
    Status s = bp_->fetchPage(META_PAGE_ID, pg);
    if (s != Status::OK) return;
    const std::uint8_t* buf = pg->data();
    if (std::memcmp(buf, MAGIC.data(), MAGIC.size()) != 0) {
        (void)bp_->unpinPage(META_PAGE_ID, false);
        return;
    }
    const std::uint8_t* p = buf + MAGIC.size();
    std::uint16_t version = readU16(p); (void)version;
    std::uint16_t count = readU16(p);
    for (std::uint16_t i = 0; i < count; ++i) {
        Entry e;
        std::string name = readStr(p);
        e.table = readStr(p);
        e.column = readStr(p);
        e.rootPageId = readU32(p);
        if (!name.empty()) {
            indexes_.emplace(std::move(name), std::move(e));
        }
    }
    (void)bp_->unpinPage(META_PAGE_ID, false);
}

IndexManager::~IndexManager() = default;

Status IndexManager::createIndex(const std::string& table,
                                 const std::string& column,
                                 const std::string& indexName) {
    if (!bp_) return Status::INVALID_ARGUMENT;
    if (indexName.empty() || table.empty() || column.empty()) {
        return Status::INVALID_ARGUMENT;
    }

    // Validate the column exists. The catalog may be a stub in tests, so
    // we tolerate the missing method and only fail when it actually says
    // "no such table".
    if (cat_) {
        const auto* info = cat_->getTable(table);
        if (info == nullptr) return Status::NOT_FOUND;
        bool found = false;
        for (const auto& c : info->schema.columns()) {
            if (c.name == column) { found = true; break; }
        }
        if (!found) return Status::NOT_FOUND;
    }

    std::lock_guard<std::mutex> lk(mu_);
    if (indexes_.count(indexName)) return Status::DUPLICATE_KEY;

    Page* rootPage = nullptr;
    PageId newRoot = allocateNodePage(bp_, rootPage, FIRST_ROOT_PID);
    if (newRoot == INVALID_PAGE_ID || rootPage == nullptr) {
        return Status::IO_ERROR;
    }
    Node rootNode{};
    rootNode.buf = rootPage->data();
    rootNode.end = rootPage->data() + Page::SIZE;
    rootNode.setLeaf(true);
    rootNode.setNumKeys(0);
    rootNode.setParent(INVALID_PAGE_ID);
    rootNode.setPrevLeaf(INVALID_PAGE_ID);
    rootNode.setNextLeaf(INVALID_PAGE_ID);
    (void)bp_->unpinPage(newRoot, true);

    Entry e;
    e.table = table;
    e.column = column;
    e.rootPageId = newRoot;
    e.tree = std::make_unique<BPlusTree>(bp_, newRoot);

    indexes_.emplace(indexName, std::move(e));

    // Persist. Failures roll back the in-memory insert.
    Page* pg = nullptr;
    Status ps = bp_->fetchPage(META_PAGE_ID, pg);
    if (ps != Status::OK) {
        indexes_.erase(indexName);
        return ps;
    }
    std::uint8_t* buf = pg->data();
    std::memset(buf, 0, Page::SIZE);
    std::uint8_t* wp = buf;
    std::memcpy(wp, MAGIC.data(), MAGIC.size()); wp += MAGIC.size();
    std::uint16_t version = 1; writeU16(wp, version);
    std::uint16_t count = static_cast<std::uint16_t>(indexes_.size());
    writeU16(wp, count);
    for (const auto& [name, ent] : indexes_) {
        writeStr(wp, name);
        writeStr(wp, ent.table);
        writeStr(wp, ent.column);
        writeU32(wp, ent.rootPageId);
    }
    (void)bp_->unpinPage(META_PAGE_ID, true);

    return Status::OK;
}

Status IndexManager::dropIndex(const std::string& indexName) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = indexes_.find(indexName);
    if (it == indexes_.end()) return Status::NOT_FOUND;
    indexes_.erase(it);

    // Rewrite the metadata page without the erased entry. If persistence
    // fails, surface the error but keep the in-memory removal.
    Page* pg = nullptr;
    Status ps = bp_->fetchPage(META_PAGE_ID, pg);
    if (ps != Status::OK) return ps;
    std::uint8_t* buf = pg->data();
    std::memset(buf, 0, Page::SIZE);
    std::uint8_t* wp = buf;
    std::memcpy(wp, MAGIC.data(), MAGIC.size()); wp += MAGIC.size();
    std::uint16_t version = 1; writeU16(wp, version);
    std::uint16_t count = static_cast<std::uint16_t>(indexes_.size());
    writeU16(wp, count);
    for (const auto& [name, ent] : indexes_) {
        writeStr(wp, name);
        writeStr(wp, ent.table);
        writeStr(wp, ent.column);
        writeU32(wp, ent.rootPageId);
    }
    (void)bp_->unpinPage(META_PAGE_ID, true);
    return Status::OK;
}

BPlusTree* IndexManager::open(const std::string& indexName) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = indexes_.find(indexName);
    if (it == indexes_.end()) return nullptr;
    if (!it->second.tree) {
        it->second.tree = std::make_unique<BPlusTree>(bp_, it->second.rootPageId);
    }
    return it->second.tree.get();
}

std::vector<std::string> IndexManager::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(indexes_.size());
    for (const auto& [name, e] : indexes_) {
        (void)e;
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace minidb::index