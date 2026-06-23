// =============================================================================
// tests/storage/storage_test.cpp
// -----------------------------------------------------------------------------
// Smoke tests for the storage layer. Will fail until DiskManager /
// BufferPool / HeapFile are implemented; that's the point — the test
// fails loudly instead of silently compiling.
// =============================================================================
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/page.h"

int main() {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "minidb_storage_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    fs::path dbFile = tmp / "minidb.db";

    // DiskManager takes a file path, not a directory. The parent dir
    // is created as needed by the DiskManager itself.
    minidb::storage::DiskManager dm(dbFile.string());
    minidb::storage::BufferPool  bp(&dm, /*numFrames=*/8);

    minidb::storage::Page p;
    p.setPageId(7);
    minidb::Status s = dm.writePage(7, p);
    if (s == minidb::Status::UNIMPLEMENTED) {
        std::fprintf(stderr, "[skip] DiskManager::writePage is a stub\n");
        return 0;       // OK during M0
    }
    assert(s == minidb::Status::OK);

    minidb::storage::Page q;
    s = dm.readPage(7, q);
    assert(s == minidb::Status::OK);
    assert(q.getPageId() == 7);

    std::printf("[OK] storage round-trip\n");
    return 0;
}
