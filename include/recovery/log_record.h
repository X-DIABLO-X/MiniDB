// =============================================================================
// include/recovery/log_record.h
// -----------------------------------------------------------------------------
// LogRecord + LogKind + (de)serialisation helpers.
// =============================================================================
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "common/types.h"

namespace minidb::recovery {

enum class LogKind : std::uint8_t {
    BEGIN      = 0,
    COMMIT     = 1,
    ABORT      = 2,
    INSERT     = 3,
    UPDATE     = 4,
    DELETE     = 5,
    CHECKPOINT = 6
};

struct LogRecord {
    LogKind                kind = LogKind::BEGIN;
    TransactionId          txnId = INVALID_TXN_ID;
    LSN                    prevLSN = INVALID_LSN;
    // payload
    RecordId               rid = INVALID_RID;
    std::vector<std::uint8_t> beforeImage;
    std::vector<std::uint8_t> afterImage;
    // CHECKPOINT only:
    std::vector<TransactionId> activeTxns;
};

// Serialise to / from a byte buffer. The wire format is documented in
// include/recovery/README.md.
std::vector<std::uint8_t> encode(const LogRecord& r);
bool                      decode(std::span<const std::uint8_t> bytes, LogRecord& out);

} // namespace minidb::recovery