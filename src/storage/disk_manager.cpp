// =============================================================================
// src/storage/disk_manager.cpp
// -----------------------------------------------------------------------------
// Owns the data file (default `data/pages/minidb.db`). The file is opened in
// read/write binary mode; reads and writes are page-aligned (4 KB) and
// protected by a single mutex. Page id 0 is reserved for the catalog
// bootstrap page and is always present.
//
// Page allocation: `nextPageId_` is the highest id+1 ever issued. `freeList_`
// holds previously freed ids, popped LIFO.
// =============================================================================
#include "storage/disk_manager.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
  #include <io.h>
#else
  #include <unistd.h>
#endif

namespace minidb::storage {

namespace {

// Translate the last OS error into an empty Status; the caller will fall
// back to IO_ERROR. We only need errno-style diagnostics in DEBUG builds.
int fsyncFd(int fd) {
#if defined(_WIN32)
    return ::_commit(fd) == 0 ? 0 : -1;
#else
    return ::fsync(fd) == 0 ? 0 : -1;
#endif
}

} // namespace

DiskManager::DiskManager(const std::string& dbPath) : path_(dbPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    // Open read/write binary. If the file does not exist, create it.
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_.is_open()) {
        file_.clear();
        file_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    }
    if (!file_.is_open()) {
        // Fallback: create-only then reopen. Some platforms refuse r/w on
        // a missing file even with trunc.
        std::FILE* fp = std::fopen(path_.c_str(), "wb");
        if (fp) std::fclose(fp);
        file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    }
    if (!file_.is_open()) {
        // Last resort: leave file_ closed; reads/writes will fail.
        nextPageId_ = 1;
        return;
    }
    // Discover current file size to seed nextPageId_.
    file_.seekg(0, std::ios::end);
    std::streamoff sz = file_.tellg();
    if (sz < 0) sz = 0;
    const PageId n = static_cast<PageId>(sz / Page::SIZE);
    nextPageId_ = (n == 0) ? 1 : n;
}

DiskManager::~DiskManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

Status DiskManager::readPage(PageId pageId, Page& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!file_.is_open()) return Status::IO_ERROR;
    out.setPageId(pageId);
    out.setDirty(false);

    file_.clear();
    file_.seekg(static_cast<std::streamoff>(pageId) * Page::SIZE, std::ios::beg);
    if (file_.fail()) {
        // Page beyond EOF: treat as zero page (fresh DB). Out is already
        // reset by the caller? No — callers reuse the Page object. We
        // must zero the buffer before returning.
        out.reset();
        out.setPageId(pageId);
        return Status::OK;
    }
    file_.read(reinterpret_cast<char*>(out.data()), Page::SIZE);
    const std::streamsize got = file_.gcount();
    if (got < static_cast<std::streamsize>(Page::SIZE)) {
        // Short read: zero the tail.
        std::memset(out.data() + got, 0, Page::SIZE - static_cast<std::size_t>(got));
    }
    return Status::OK;
}

Status DiskManager::writePage(PageId pageId, const Page& page) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!file_.is_open()) return Status::IO_ERROR;

    file_.clear();
    file_.seekp(static_cast<std::streamoff>(pageId) * Page::SIZE, std::ios::beg);
    if (file_.fail()) return Status::IO_ERROR;
    file_.write(reinterpret_cast<const char*>(page.data()), Page::SIZE);
    if (file_.fail()) return Status::IO_ERROR;
    file_.flush();
    return Status::OK;
}

PageId DiskManager::allocatePage() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!freeList_.empty()) {
        const PageId id = freeList_.back();
        freeList_.pop_back();
        return id;
    }
    const PageId id = nextPageId_++;
    if (file_.is_open()) {
        // Extend the file by one page so subsequent writes have room.
        file_.clear();
        file_.seekp(0, std::ios::end);
        std::array<std::uint8_t, Page::SIZE> zeros{};
        file_.write(reinterpret_cast<const char*>(zeros.data()), Page::SIZE);
        file_.flush();
    }
    return id;
}

Status DiskManager::freePage(PageId pageId) {
    std::lock_guard<std::mutex> lk(mu_);
    if (pageId == 0) return Status::INVALID_ARGUMENT; // catalog page is permanent
    freeList_.push_back(pageId);
    return Status::OK;
}

void DiskManager::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!file_.is_open()) return;
    file_.flush();
#if defined(_WIN32)
    // Best-effort: fstream does not expose the underlying fd. Try the
    // platform-specific sync via a side-channel FILE* opened on the same path.
    if (std::FILE* fp = std::fopen(path_.c_str(), "rb+")) {
        std::fflush(fp);
        int fd = fileno(fp);
        if (fd >= 0) (void)fsyncFd(fd);
        std::fclose(fp);
    }
#else
    // POSIX: fstream internally uses an fd we can pull via the native handle.
    // We can flush via a side channel.
    if (std::FILE* fp = std::fopen(path_.c_str(), "rb+")) {
        std::fflush(fp);
        int fd = fileno(fp);
        if (fd >= 0) (void)fsyncFd(fd);
        std::fclose(fp);
    }
#endif
}

} // namespace minidb::storage