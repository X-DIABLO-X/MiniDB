// =============================================================================
// include/storage/disk_manager.h
// -----------------------------------------------------------------------------
// Owns the on-disk data file (default: data/pages/minidb.db) and the page
// free-list. Page id 0 is reserved for the catalog bootstrap page.
//
// Other modules should never open or write the data file directly — they go
// through this class.
// =============================================================================
#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "common/status.h"
#include "common/types.h"
#include "storage/page.h"

namespace minidb::storage {

class DiskManager {
public:
    // Opens (or creates) `<dbPath>/minidb.db` and loads the free list.
    explicit DiskManager(const std::string& dbPath);
    ~DiskManager();

    DiskManager(const DiskManager&)            = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Reads page `pageId` into `out`. Caller owns `out` and pre-allocates it.
    Status readPage(PageId pageId, Page& out);

    // Writes `page` to disk at offset `pageId * Page::SIZE`.
    Status writePage(PageId pageId, const Page& page);

    // Returns a never-used page id. Updates the in-memory free list and
    // extends the file if necessary.
    PageId allocatePage();

    // Marks the page as free for reuse. The bytes are zeroed on next read.
    Status freePage(PageId pageId);

    // fsync the data file.
    void flush();

    // Path of the data file (for tests / cleanup).
    const std::string& path() const { return path_; }

private:
    std::string       path_;
    std::fstream      file_;
    std::mutex        mu_;
    std::vector<PageId> freeList_;   // pages that were freed and can be reused
    PageId            nextPageId_ = 1;   // page 0 is reserved for the catalog
};

} // namespace minidb::storage
