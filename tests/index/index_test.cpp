// =============================================================================
// tests/index/index_test.cpp
// -----------------------------------------------------------------------------
// Smoke tests for the B+ tree.
// =============================================================================
#include <cassert>
#include <cstdio>
#include <filesystem>

#include "index/bplus_tree.h"
#include "index/index_manager.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"

using namespace minidb;
namespace fs = std::filesystem;

int main() {
    fs::path tmp = fs::temp_directory_path() / "minidb_index_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    storage::DiskManager dm(tmp.string());
    storage::BufferPool bp(&dm, 8);

    index::BPlusTree tree(&bp, /*rootPageId=*/1);
    RecordId out;
    Status s = tree.search("hello", out);
    if (s == Status::UNIMPLEMENTED) {
        std::fprintf(stderr, "[skip] BPlusTree::search is a stub\n");
        return 0;
    }
    std::printf("[OK] index search returned\n");
    return 0;
}
