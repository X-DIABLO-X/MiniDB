// =============================================================================
// src/recovery/log_record.cpp
// -----------------------------------------------------------------------------
// LogRecord (de)serialisation.
//
// Wire format (host endian, written with memcpy):
//   [ kind : u8 ]
//   [ txnId : u8 if kind==CHECKPOINT ? u32 : u64 ]    (we always use u64)
//   [ prevLSN : u64 ]
//   --- kind-dependent payload ---
//   BEGIN / COMMIT / ABORT:
//       (no extra payload)
//   INSERT:
//       [ rid : u64 ]
//       [ rowLen : u32 ][ rowBytes : rowLen ]
//   UPDATE:
//       [ rid : u64 ]
//       [ beforeLen : u32 ][ beforeBytes : beforeLen ]
//       [ afterLen  : u32 ][ afterBytes  : afterLen  ]
//   DELETE:
//       [ rid : u64 ]
//       [ rowLen : u32 ][ rowBytes : rowLen ]
//   CHECKPOINT:
//       [ activeCount : u32 ][ txnId : u64 ] * activeCount
//
// The LogRecord has separate `beforeImage` and `afterImage` fields. We map
// `afterImage` to "rowBytes" for INSERT/DELETE, and use both fields for
// UPDATE. For DELETE, `beforeImage` is also accepted and used as the
// captured row (so that a CLR for a DELETE can carry the row).
// =============================================================================
#include "recovery/log_record.h"

#include <cstring>

namespace minidb::recovery {

namespace {

// Append a trivially-copyable value to the buffer using memcpy.
template <typename T>
void appendPOD(std::vector<std::uint8_t>& buf, const T& v) {
    const std::size_t off = buf.size();
    buf.resize(off + sizeof(T));
    std::memcpy(buf.data() + off, &v, sizeof(T));
}

// Append a length-prefixed byte span.
void appendBytes(std::vector<std::uint8_t>& buf,
                 std::span<const std::uint8_t> bytes) {
    const std::uint32_t len = static_cast<std::uint32_t>(bytes.size());
    appendPOD<std::uint32_t>(buf, len);
    const std::size_t off = buf.size();
    buf.resize(off + len);
    if (len > 0) {
        std::memcpy(buf.data() + off, bytes.data(), len);
    }
}

// Read a trivially-copyable value from a span, advancing `off`. Returns
// false on out-of-range.
template <typename T>
bool readPOD(std::span<const std::uint8_t> bytes, std::size_t& off, T& out) {
    if (off + sizeof(T) > bytes.size()) return false;
    std::memcpy(&out, bytes.data() + off, sizeof(T));
    off += sizeof(T);
    return true;
}

// Read a length-prefixed byte vector, advancing `off`. Returns false on
// out-of-range. `cap` is an upper bound used to detect obviously corrupt
// lengths; we use 8 MiB as a generous cap.
bool readBytes(std::span<const std::uint8_t> bytes,
               std::size_t& off,
               std::vector<std::uint8_t>& out) {
    constexpr std::uint32_t MAX_LEN = 8u * 1024u * 1024u;
    std::uint32_t len = 0;
    if (!readPOD<std::uint32_t>(bytes, off, len)) return false;
    if (len > MAX_LEN) return false;
    if (off + len > bytes.size()) return false;
    out.assign(bytes.begin() + off, bytes.begin() + off + len);
    off += len;
    return true;
}

} // namespace

// -----------------------------------------------------------------------------
// Encode a LogRecord to a flat byte buffer (host endian).
// -----------------------------------------------------------------------------
std::vector<std::uint8_t> encode(const LogRecord& r) {
    std::vector<std::uint8_t> buf;
    buf.reserve(64 + r.beforeImage.size() + r.afterImage.size());

    // Common header: kind + txnId + prevLSN.
    const std::uint8_t kind8 = static_cast<std::uint8_t>(r.kind);
    appendPOD<std::uint8_t>(buf, kind8);
    appendPOD<std::uint64_t>(buf, r.txnId);
    appendPOD<std::uint64_t>(buf, r.prevLSN);

    switch (r.kind) {
        case LogKind::BEGIN:
        case LogKind::COMMIT:
        case LogKind::ABORT:
            // No payload.
            break;

        case LogKind::INSERT: {
            appendPOD<std::uint64_t>(buf, r.rid);
            appendBytes(buf, r.afterImage);
            break;
        }

        case LogKind::UPDATE: {
            appendPOD<std::uint64_t>(buf, r.rid);
            appendBytes(buf, r.beforeImage);
            appendBytes(buf, r.afterImage);
            break;
        }

        case LogKind::DELETE: {
            appendPOD<std::uint64_t>(buf, r.rid);
            // For DELETE we accept either afterImage (raw row) or
            // beforeImage (the captured row used by a CLR). Prefer
            // afterImage for consistency with the README.
            std::span<const std::uint8_t> row = r.afterImage;
            if (row.empty() && !r.beforeImage.empty()) {
                row = std::span<const std::uint8_t>(r.beforeImage.data(),
                                                    r.beforeImage.size());
            }
            appendBytes(buf, row);
            break;
        }

        case LogKind::CHECKPOINT: {
            const std::uint32_t n =
                static_cast<std::uint32_t>(r.activeTxns.size());
            appendPOD<std::uint32_t>(buf, n);
            for (TransactionId t : r.activeTxns) {
                appendPOD<std::uint64_t>(buf, t);
            }
            break;
        }
    }

    return buf;
}

// -----------------------------------------------------------------------------
// Decode a byte buffer produced by `encode` back into a LogRecord.
// Returns false on truncated / malformed input.
// -----------------------------------------------------------------------------
bool decode(std::span<const std::uint8_t> bytes, LogRecord& out) {
    out = LogRecord{};
    std::size_t off = 0;

    std::uint8_t kind8 = 0;
    if (!readPOD<std::uint8_t>(bytes, off, kind8)) return false;
    if (kind8 > static_cast<std::uint8_t>(LogKind::CHECKPOINT)) return false;
    out.kind = static_cast<LogKind>(kind8);

    if (!readPOD<std::uint64_t>(bytes, off, out.txnId))  return false;
    if (!readPOD<std::uint64_t>(bytes, off, out.prevLSN)) return false;

    switch (out.kind) {
        case LogKind::BEGIN:
        case LogKind::COMMIT:
        case LogKind::ABORT:
            return true;

        case LogKind::INSERT: {
            if (!readPOD<std::uint64_t>(bytes, off, out.rid)) return false;
            if (!readBytes(bytes, off, out.afterImage)) return false;
            return true;
        }

        case LogKind::UPDATE: {
            if (!readPOD<std::uint64_t>(bytes, off, out.rid)) return false;
            if (!readBytes(bytes, off, out.beforeImage)) return false;
            if (!readBytes(bytes, off, out.afterImage))  return false;
            return true;
        }

        case LogKind::DELETE: {
            if (!readPOD<std::uint64_t>(bytes, off, out.rid)) return false;
            if (!readBytes(bytes, off, out.afterImage)) return false;
            return true;
        }

        case LogKind::CHECKPOINT: {
            std::uint32_t n = 0;
            if (!readPOD<std::uint32_t>(bytes, off, n)) return false;
            out.activeTxns.clear();
            out.activeTxns.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                TransactionId t = INVALID_TXN_ID;
                if (!readPOD<std::uint64_t>(bytes, off, t)) return false;
                out.activeTxns.push_back(t);
            }
            return true;
        }
    }
    return false;
}

} // namespace minidb::recovery
