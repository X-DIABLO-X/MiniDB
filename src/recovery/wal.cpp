// =============================================================================
// src/recovery/wal.cpp
// -----------------------------------------------------------------------------
// Append-only Write-Ahead Log with fsync on flush.
//
// On-disk frame (per task spec):
//   [ magic    : u32 ]   = kWalMagic ('W' 'A' 'L' '0' = 0x304C4157 LE)
//   [ len      : u32 ]   = payload length in bytes
//   [ payload  : u8 * len ]
//
// `append()` writes one frame, advances `nextLSN_`, and returns the LSN
// assigned to that frame. The payload itself is the result of
// `recovery::encode(LogRecord)`.
//
// `read(from, visit)` walks the log forward from the first frame and
// invokes `visit` for every record with LSN >= `from`. Decoding errors
// stop the walk (a torn tail is normal at the very end after a crash).
//
// `flush()` performs an `fsync`-equivalent on the underlying file
// (fflush + fsync via `_commit` on Windows / `fsync` on POSIX).
// =============================================================================
#include "recovery/wal.h"

#include <array>
#include <filesystem>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <io.h>
  #define MINIDB_FSYNCRESULT int
#else
  #include <unistd.h>
  #define MINIDB_FSYNCRESULT int
#endif

// Truncate a FILE* to a given absolute size. Returns 0 on success.
static int truncateFile(std::FILE* fp, long size) {
    if (!fp || size < 0) return -1;
    std::fflush(fp);
#if defined(_WIN32)
    return _chsize_s(fileno(fp), size) == 0 ? 0 : -1;
#else
    return ftruncate(fileno(fp), static_cast<off_t>(size)) == 0 ? 0 : -1;
#endif
}

namespace minidb::recovery {

namespace {

// 'W' 'A' 'L' '0' stored little-endian -> 0x304C4157.
constexpr std::uint32_t kWalMagic = 0x304C4157u;

// fsync the given FILE*. Returns 0 on success, -1 on failure.
int fsyncFile(std::FILE* fp) {
    if (!fp) return -1;
    std::fflush(fp);
    const int fd = fileno(fp);
    if (fd < 0) return -1;
#if defined(_WIN32)
    return ::_commit(fd) == 0 ? 0 : -1;
#else
    return ::fsync(fd) == 0 ? 0 : -1;
#endif
}

} // namespace

// -----------------------------------------------------------------------------
// Open the WAL file in append+read mode and recover `nextLSN_` by counting
// valid frames from the start.
// -----------------------------------------------------------------------------
WAL::WAL(const std::string& path) : path_(path) {
    // Ensure the parent directory exists so fopen("a+b") can create
    // the file. On a fresh DB the directory may not be present yet.
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);

    // Use "a+b" so we can both append and read. The file is created if it
    // does not exist; fopen failure here is non-fatal — recovery must
    // still succeed on a fresh DB.
    std::FILE* fp = std::fopen(path_.c_str(), "a+b");
    if (!fp) {
        // Fall back to read-only: maybe the directory is not writable, but
        // we still want to be able to read an existing log.
        fp = std::fopen(path_.c_str(), "rb");
    }
    fp_ = fp;

    if (!fp_) {
        // No log yet — leave nextLSN_ at 1 and let later operations
        // recreate the file as needed.
        nextLSN_ = 1;
        return;
    }

    // Walk the file to determine how many complete frames are present.
    nextLSN_ = 1;
    while (true) {
        const long here = std::ftell(fp_);
        if (here < 0) break;

        std::array<std::uint8_t, 8> header{};
        const std::size_t got = std::fread(header.data(), 1, header.size(), fp_);
        if (got == 0) {
            break; // clean EOF
        }
        if (got != header.size()) {
            // Torn header — truncate the partial frame so future appends
            // start on a fresh frame boundary.
            truncateFile(fp_, here);
            break;
        }

        std::uint32_t magic = 0;
        std::uint32_t len   = 0;
        std::memcpy(&magic, header.data() + 0, sizeof(magic));
        std::memcpy(&len,   header.data() + 4, sizeof(len));

        if (magic != kWalMagic) {
            // Bad magic — assume the rest of the file is garbage; truncate
            // here so we can keep appending.
            truncateFile(fp_, here);
            break;
        }

        // Sanity-cap to avoid spinning on a corrupt length.
        constexpr std::uint32_t MAX_FRAME_PAYLOAD = 64u * 1024u * 1024u;
        if (len > MAX_FRAME_PAYLOAD) {
            truncateFile(fp_, here);
            break;
        }

        // Try to skip the payload. If we cannot read `len` bytes, the
        // tail is torn — truncate and stop.
        if (len > 0) {
            if (std::fseek(fp_, static_cast<long>(len), SEEK_CUR) != 0) {
                truncateFile(fp_, here);
                break;
            }
        }

        ++nextLSN_;
    }

    // Seek to end so the next append writes after the last valid frame.
    std::fseek(fp_, 0, SEEK_END);
}

WAL::~WAL() {
    if (fp_) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

// -----------------------------------------------------------------------------
// Append a single record frame to the WAL.
// -----------------------------------------------------------------------------
LSN WAL::append(const LogRecord& r) {
    std::lock_guard<std::mutex> lk(mu_);

    if (!fp_) {
        // Lazy (re)open. If we still cannot open, we fail the append by
        // returning INVALID_LSN — the caller is expected to surface this.
        fp_ = std::fopen(path_.c_str(), "ab");
        if (!fp_) return INVALID_LSN;
        std::fseek(fp_, 0, SEEK_END);
    }

    const std::vector<std::uint8_t> payload = encode(r);
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());

    std::array<std::uint8_t, 8> header{};
    std::memcpy(header.data() + 0, &kWalMagic, sizeof(kWalMagic));
    std::memcpy(header.data() + 4, &len,       sizeof(len));

    const std::size_t hw = std::fwrite(header.data(), 1, header.size(), fp_);
    if (hw != header.size()) return INVALID_LSN;
    if (len > 0) {
        const std::size_t pw = std::fwrite(payload.data(), 1, payload.size(), fp_);
        if (pw != payload.size()) return INVALID_LSN;
    }

    const LSN assigned = nextLSN_++;
    return assigned;
}

// -----------------------------------------------------------------------------
// Force the WAL file to durable storage.
// -----------------------------------------------------------------------------
Status WAL::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fp_) return Status::OK; // nothing to flush
    if (fsyncFile(fp_) != 0) {
        return Status::IO_ERROR;
    }
    return Status::OK;
}

// -----------------------------------------------------------------------------
// Walk the log forward and invoke `visit` for every valid record with
// LSN >= `from`. Stops early if `visit` returns false.
// -----------------------------------------------------------------------------
Status WAL::read(LSN from, std::function<bool(const LogRecord&)> visit) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fp_) return Status::OK;
    if (!visit) return Status::OK;

    // We have to read from the start because the on-disk file is a
    // sequence of variable-length frames; only by walking them can we
    // match an LSN. Open a parallel handle so we do not disturb the
    // append position.
    std::FILE* rfp = std::fopen(path_.c_str(), "rb");
    if (!rfp) return Status::OK; // no log file -> nothing to read

    LSN lsn = 1;
    while (true) {
        std::array<std::uint8_t, 8> header{};
        const std::size_t got = std::fread(header.data(), 1, header.size(), rfp);
        if (got == 0) break; // clean EOF
        if (got != header.size()) break; // torn header at tail

        std::uint32_t magic = 0;
        std::uint32_t len   = 0;
        std::memcpy(&magic, header.data() + 0, sizeof(magic));
        std::memcpy(&len,   header.data() + 4, sizeof(len));

        if (magic != kWalMagic) break; // corruption stops the walk

        std::vector<std::uint8_t> payload(len);
        if (len > 0) {
            const std::size_t pg = std::fread(payload.data(), 1, payload.size(), rfp);
            if (pg != payload.size()) break; // torn payload at tail
        }

        if (lsn >= from) {
            LogRecord rec;
            if (decode(payload, rec)) {
                if (!visit(rec)) {
                    std::fclose(rfp);
                    return Status::OK;
                }
            }
        }
        ++lsn;
    }

    std::fclose(rfp);
    return Status::OK;
}

} // namespace minidb::recovery
