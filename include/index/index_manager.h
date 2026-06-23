// =============================================================================
// include/index/index_manager.h
// -----------------------------------------------------------------------------
// IndexManager owns a (name → BPlusTree*) map. It does not own the B+
// tree's pages — those go through the BufferPool.
// =============================================================================
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/catalog_manager.h"
#include "common/status.h"
#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"

namespace minidb::index {

class IndexManager {
public:
    IndexManager(storage::BufferPool* bp, catalog::CatalogManager* cat);
    ~IndexManager();

    IndexManager(const IndexManager&)            = delete;
    IndexManager& operator=(const IndexManager&) = delete;

    Status createIndex(const std::string& table,
                       const std::string& column,
                       const std::string& indexName);
    Status dropIndex  (const std::string& indexName);

    // Returns nullptr if no such index.
    BPlusTree*           open(const std::string& indexName);
    std::vector<std::string> list() const;

private:
    struct Entry {
        std::string        table;
        std::string        column;
        PageId             rootPageId;
        std::unique_ptr<BPlusTree> tree;
    };

    storage::BufferPool*      bp_;
    catalog::CatalogManager*  cat_;
    mutable std::mutex        mu_;
    std::unordered_map<std::string, Entry> indexes_;
};

} // namespace minidb::index