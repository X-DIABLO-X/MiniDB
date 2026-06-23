// =============================================================================
// include/recovery/wal.h
// -----------------------------------------------------------------------------
// Append-only Write-Ahead Log with fsync on flush.
// =============================================================================
#pragma once

#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

#include "common/status.h"
#include "common/types.h"
#include "recovery/log_record.h"

namespace minidb::recovery {

class WAL {
public:
    explicit WAL(const std::string& path);
    ~WAL();

    WAL(const WAL&)            = delete;
    WAL& operator=(const WAL&) = delete;

    LSN    append(const LogRecord& r);   // assigns LSN, returns it
    Status flush();                      // fsync

    // Read from LSN `from` to end of log, calling `visit` for every record.
    // Stops if `visit` returns false.
    Status read(LSN from,
                std::function<bool(const LogRecord&)> visit);

    LSN    currentLSN() const { return nextLSN_; }

    const std::string& path() const { return path_; }

private:
    std::string   path_;
    FILE*         fp_      = nullptr;
    LSN           nextLSN_ = 1;
    std::mutex    mu_;
};

} // namespace minidb::recovery