// =============================================================================
// src/catalog/catalog_manager.cpp
// -----------------------------------------------------------------------------
// Persists table metadata on page 0 of the data file.
//
// Wire layout of page 0:
//   [ 6  bytes : "MNDB\0\0"            ] @ 0
//   [ 2  bytes : u16 version           ] @ 6
//   [ 2  bytes : u16 numTables         ] @ 8
//   for each table:
//     [ u16  name_len + name bytes ]
//     [ Schema::encode output        ]
//     [ u32  firstPageId             ]
//     [ u16  primaryIndexName_len + name bytes ]
// =============================================================================
#include "catalog/catalog_manager.h"

#include "catalog/schema.h"
#include "storage/page.h"

#include <cstring>
#include <stdexcept>

namespace minidb::catalog {

namespace {

constexpr PageId CATALOG_PAGE_ID = 0;
constexpr std::array<char, 6> MAGIC = {'M','N','D','B', 0, 0};

template <typename T>
void append(std::vector<std::uint8_t>& out, const T& v) {
    const std::size_t off = out.size();
    out.resize(off + sizeof(T));
    std::memcpy(out.data() + off, &v, sizeof(T));
}

void appendStr(std::vector<std::uint8_t>& out, const std::string& s) {
    const std::uint16_t n = static_cast<std::uint16_t>(s.size());
    append<std::uint16_t>(out, n);
    if (n) {
        const std::size_t off = out.size();
        out.resize(off + n);
        std::memcpy(out.data() + off, s.data(), n);
    }
}

template <typename T>
bool readAt(const std::uint8_t* base, std::size_t& off, T& v) {
    if (off + sizeof(T) > storage::Page::SIZE) return false;
    std::memcpy(&v, base + off, sizeof(T));
    off += sizeof(T);
    return true;
}

bool readStr(const std::uint8_t* base, std::size_t& off, std::string& out) {
    std::uint16_t n = 0;
    if (!readAt<std::uint16_t>(base, off, n)) return false;
    if (off + n > storage::Page::SIZE) return false;
    out.assign(reinterpret_cast<const char*>(base + off), n);
    off += n;
    return true;
}

} // namespace

CatalogManager::CatalogManager(storage::DiskManager* dm) : dm_(dm) {}

CatalogManager::~CatalogManager() {
    (void)flush();
}

Status CatalogManager::load() {
    std::lock_guard<std::mutex> lk(mu_);
    storage::Page p;
    Status s = dm_->readPage(CATALOG_PAGE_ID, p);
    if (s != Status::OK) return s;

    tables_.clear();
    card_.clear();

    if (std::memcmp(p.data(), MAGIC.data(), MAGIC.size()) != 0) {
        // Fresh DB. Stay empty and let the first createTable populate.
        return Status::OK;
    }
    const std::uint8_t* base = p.data();
    std::size_t off = MAGIC.size();
    std::uint16_t version = 0;
    if (!readAt<std::uint16_t>(base, off, version)) return Status::OK;
    (void)version;
    std::uint16_t n = 0;
    if (!readAt<std::uint16_t>(base, off, n)) return Status::OK;
    for (std::uint16_t i = 0; i < n; ++i) {
        TableInfo info;
        if (!readStr(base, off, info.name)) break;
        info.schema = Schema::decode(std::span<const std::uint8_t>(base, storage::Page::SIZE), off);
        if (!readAt<std::uint32_t>(base, off, reinterpret_cast<std::uint32_t&>(info.firstPageId))) break;
        if (!readStr(base, off, info.primaryIndexName)) break;
        tables_[info.name] = std::move(info);
    }
    return Status::OK;
}

Status CatalogManager::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    return flushLocked();
}

// flushLocked() — flush the catalog to disk WITHOUT acquiring mu_.
// Callers MUST already hold mu_. (flushLocked exists so that callers
// already inside a mu_-critical section can persist without re-locking
// and deadlocking on a non-recursive std::mutex.)
Status CatalogManager::flushLocked() {
    storage::Page p;
    p.reset();
    p.setPageId(CATALOG_PAGE_ID);

    std::vector<std::uint8_t> buf;
    append<std::array<char, 6>>(buf, MAGIC);
    const std::uint16_t version = 1;
    append<std::uint16_t>(buf, version);
    const std::uint16_t n = static_cast<std::uint16_t>(tables_.size());
    append<std::uint16_t>(buf, n);
    for (const auto& [name, info] : tables_) {
        (void)name;
        appendStr(buf, info.name);
        info.schema.encode(buf);
        const std::uint32_t fpid = info.firstPageId;
        append<std::uint32_t>(buf, fpid);
        appendStr(buf, info.primaryIndexName);
    }
    if (buf.size() > storage::Page::SIZE) return Status::FULL;
    std::memcpy(p.data(), buf.data(), buf.size());
    return dm_->writePage(CATALOG_PAGE_ID, p);
}

Status CatalogManager::createTable(const TableInfo& info) {
    std::lock_guard<std::mutex> lk(mu_);
    if (tables_.count(info.name)) return Status::DUPLICATE_KEY;
    TableInfo copy = info;
    // Eagerly allocate the first page of the table's heap file. We do this
    // here (and not in HeapFile) so that HeapFile can hold a const
    // TableInfo* — the firstPageId is part of the persisted schema.
    if (copy.firstPageId == 0) {
        copy.firstPageId = dm_->allocatePage();
    }
    tables_.emplace(copy.name, std::move(copy));
    return flushLocked();
}

Status CatalogManager::dropTable(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tables_.find(name);
    if (it == tables_.end()) return Status::NOT_FOUND;
    tables_.erase(it);
    card_.erase(name);
    return flushLocked();
}

const TableInfo* CatalogManager::getTable(const std::string& n) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tables_.find(n);
    return it == tables_.end() ? nullptr : &it->second;
}

std::vector<std::string> CatalogManager::listTables() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(tables_.size());
    for (const auto& [name, _] : tables_) out.push_back(name);
    return out;
}

void CatalogManager::setCardinality(const std::string& table, std::uint64_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    card_[table] = n;
}

std::uint64_t CatalogManager::cardinality(const std::string& table) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = card_.find(table);
    return it == card_.end() ? 0 : it->second;
}

} // namespace minidb::catalog