// =============================================================================
// include/catalog/catalog_manager.h
// -----------------------------------------------------------------------------
// Loads / persists table metadata on page 0 of the data file. See
// docs/catalog.md for the on-disk layout.
// =============================================================================
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/schema.h"
#include "catalog/table_info.h"
#include "common/status.h"
#include "common/types.h"
#include "storage/disk_manager.h"

namespace minidb::catalog {

class CatalogManager {
public:
    explicit CatalogManager(storage::DiskManager* dm);
    ~CatalogManager();

    CatalogManager(const CatalogManager&)            = delete;
    CatalogManager& operator=(const CatalogManager&) = delete;

    // Reads page 0 and rebuilds the in-memory table map. If page 0 is
    // empty, starts fresh.
    Status load();

    // Serialises the in-memory map back to page 0 and fsyncs.
    Status flush();

    // flushLocked() — same as flush() but does NOT acquire mu_. Callers
    // must already hold the mutex. Used by createTable / dropTable which
    // are already inside a mu_-critical section.
    Status flushLocked();

    Status                  createTable(const TableInfo& info);
    Status                  dropTable  (const std::string& name);
    const TableInfo*        getTable   (const std::string& name) const;
    std::vector<std::string> listTables() const;

    void        setCardinality(const std::string& table, std::uint64_t n);
    std::uint64_t cardinality(const std::string& table) const;   // 0 = unknown

private:
    storage::DiskManager*                              dm_;
    mutable std::mutex                                 mu_;
    std::unordered_map<std::string, TableInfo>         tables_;
    std::unordered_map<std::string, std::uint64_t>    card_;
};

} // namespace minidb::catalog
