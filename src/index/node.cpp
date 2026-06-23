// =============================================================================
// src/index/node.cpp
// -----------------------------------------------------------------------------
// Node view over a Page buffer. See include/index/README.md for the layout.
//
// Data-area layout (after the fixed 16-byte header):
//
//   Leaf page:
//       [ keyLen0:uint16 | key0 | rid0:uint64 | keyLen1:uint16 | key1 | rid1:uint64 ... ]
//
//   Internal page:
//       [ child0:uint32 | keyLen0:uint16 | key0 | child1:uint32 | keyLen1:uint16 | key1 ... ]
//
// We keep the data area packed — no per-entry length prefix for the value,
// so reading entry i requires walking entries 0..i-1 to find the offset of i.
// Insertions are handled by BPlusTree, which always rewrites the affected
// range of the data area after a split has freed enough space.
// =============================================================================
#include "index/node.h"

#include <cstring>
#include <stdexcept>

namespace minidb::index {

namespace {
constexpr std::size_t OFF_IS_LEAF     = 0;
constexpr std::size_t OFF_NUM_KEYS    = 1;
constexpr std::size_t OFF_PARENT      = 4;
constexpr std::size_t OFF_PREV_LEAF   = 8;
constexpr std::size_t OFF_NEXT_LEAF   = 12;
constexpr std::size_t OFF_DATA_BASE   = 16;

// Returns the byte offset of the *start* of entry `i` within the data area.
// For a leaf, entry i begins at the keyLen of key i.
// For an internal node, entry i begins at child_i (i.e. before keyLen_i).
inline std::size_t entryOffset(bool isLeaf, std::uint16_t i) {
    if (isLeaf) {
        // Each leaf entry = 2 (keyLen) + keyLen + 8 (rid)
        return OFF_DATA_BASE + static_cast<std::size_t>(i) * 2u;
    }
    // For internal, entry i = child_i(4) + keyLen_i(2) + key_i
    return OFF_DATA_BASE + static_cast<std::size_t>(i) * 6u;
}

inline std::uint16_t readU16(const std::uint8_t* p) {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline std::uint32_t readU32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline std::uint64_t readU64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline void writeU16(std::uint8_t* p, std::uint16_t v) {
    std::memcpy(p, &v, sizeof(v));
}

inline void writeU32(std::uint8_t* p, std::uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

inline void writeU64(std::uint8_t* p, std::uint64_t v) {
    std::memcpy(p, &v, sizeof(v));
}
} // namespace

// -- header ------------------------------------------------------------------

bool         Node::isLeaf() const { return buf[OFF_IS_LEAF] != 0; }
void         Node::setLeaf(bool v) { buf[OFF_IS_LEAF] = v ? 1 : 0; }

std::uint16_t Node::numKeys() const {
    return readU16(buf + OFF_NUM_KEYS);
}
void Node::setNumKeys(std::uint16_t n) {
    writeU16(buf + OFF_NUM_KEYS, n);
}

PageId Node::parent() const {
    return readU32(buf + OFF_PARENT);
}
void Node::setParent(PageId p) {
    writeU32(buf + OFF_PARENT, p);
}

PageId Node::prevLeaf() const {
    return readU32(buf + OFF_PREV_LEAF);
}
PageId Node::nextLeaf() const {
    return readU32(buf + OFF_NEXT_LEAF);
}
void Node::setPrevLeaf(PageId p) { writeU32(buf + OFF_PREV_LEAF, p); }
void Node::setNextLeaf(PageId p) { writeU32(buf + OFF_NEXT_LEAF, p); }

// -- leaf entry: (keyLen:uint16, keyBytes, rid:uint64) -----------------------

namespace {
// Compute the byte length of leaf entry i when its key is `kLen` bytes long.
inline std::size_t leafEntrySize(std::size_t kLen) {
    return 2u + kLen + 8u;
}
// Compute the byte length of internal entry i when its key is `kLen` bytes.
inline std::size_t internalEntrySize(std::size_t kLen) {
    // child:uint32 + keyLen:uint16 + key bytes
    return 4u + 2u + kLen;
}
} // namespace

// Returns a span covering exactly the key bytes of entry `i`. The caller must
// ensure that `i < numKeys()` and that the node is a leaf; the span is valid
// only as long as the underlying page buffer is alive (i.e. while the page
// is pinned through the BufferPool).
std::span<const std::uint8_t> Node::key(std::uint16_t i) const {
    if (i >= numKeys()) return {};
    const std::uint8_t* p = buf + entryOffset(isLeaf(), i);
    std::uint16_t kLen;
    if (isLeaf()) {
        kLen = readU16(p);
        return { p + 2, kLen };
    }
    // internal: skip child0(4) once, then for each entry (keyLen:2 + key)
    p += 4;                                 // child0
    for (std::uint16_t k = 0; k < i; ++k) {
        kLen = readU16(p);
        p += 2 + kLen + 4;                  // skip keyLen, key, next child
    }
    kLen = readU16(p);
    return { p + 2, kLen };
}

std::span<std::uint8_t> Node::key(std::uint16_t i) {
    if (i >= numKeys()) return {};
    std::uint8_t* p = buf + entryOffset(isLeaf(), i);
    std::uint16_t kLen;
    if (isLeaf()) {
        kLen = readU16(p);
        return { p + 2, kLen };
    }
    p += 4;
    for (std::uint16_t k = 0; k < i; ++k) {
        kLen = readU16(p);
        p += 2 + kLen + 4;
    }
    kLen = readU16(p);
    return { p + 2, kLen };
}

RecordId Node::rid(std::uint16_t i) const {
    if (!isLeaf() || i >= numKeys()) return INVALID_RID;
    std::span<const std::uint8_t> k = key(i);
    const std::uint8_t* p = k.data() + k.size();
    return readU64(p);
}

void Node::setRid(std::uint16_t i, RecordId r) {
    if (!isLeaf() || i >= numKeys()) return;
    std::span<std::uint8_t> k = key(i);
    writeU64(k.data() + k.size(), r);
}

// -- internal entry: (child_i:uint32, key_i_len:uint16, key_i) ----------------

PageId Node::child(std::uint16_t i) const {
    if (isLeaf()) return INVALID_PAGE_ID;
    // child0 is stored first (4 bytes at OFF_DATA_BASE), then for i>=1 the
    // child is stored *after* key_{i-1}.
    if (i == 0) {
        return readU32(buf + OFF_DATA_BASE);
    }
    if (i > numKeys()) return INVALID_PAGE_ID;
    // walk to end of key_{i-1}
    const std::uint8_t* p = buf + OFF_DATA_BASE + 4;  // skip child0
    for (std::uint16_t k = 0; k + 1 < i; ++k) {
        std::uint16_t kLen = readU16(p);
        p += 2 + kLen + 4;
    }
    std::uint16_t prevLen = readU16(p);
    p += 2 + prevLen;
    return readU32(p);
}

void Node::setChild(std::uint16_t i, PageId pg) {
    if (isLeaf()) return;
    if (i == 0) {
        writeU32(buf + OFF_DATA_BASE, pg);
        return;
    }
    if (i > numKeys()) return;
    std::uint8_t* p = buf + OFF_DATA_BASE + 4;
    for (std::uint16_t k = 0; k + 1 < i; ++k) {
        std::uint16_t kLen = readU16(p);
        p += 2 + kLen + 4;
    }
    std::uint16_t prevLen = readU16(p);
    p += 2 + prevLen;
    writeU32(p, pg);
}

// -- key (de)serialisation ---------------------------------------------------

// INT — 4 bytes, big-endian so INT keys sort the same as their integer order.
std::vector<std::uint8_t> encodeIntKey(int32_t v) {
    std::vector<std::uint8_t> out(4);
    out[0] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(v) >> 24) & 0xFFu);
    out[1] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(v) >> 16) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(v) >>  8) & 0xFFu);
    out[3] = static_cast<std::uint8_t>( static_cast<std::uint32_t>(v)        & 0xFFu);
    return out;
}

int32_t decodeIntKey(std::span<const std::uint8_t> k) {
    if (k.size() < 4) return 0;
    std::uint32_t v = (static_cast<std::uint32_t>(k[0]) << 24) |
                      (static_cast<std::uint32_t>(k[1]) << 16) |
                      (static_cast<std::uint32_t>(k[2]) <<  8) |
                      (static_cast<std::uint32_t>(k[3]));
    return static_cast<int32_t>(v);
}

// FLOAT — re-interpret the IEEE-754 bit pattern so that byte-wise comparison
// matches the numeric order. Positive floats have sign bit 0; the unsigned
// bit pattern is monotonic for positives, but for negatives it is reversed
// (more negative = larger unsigned). We flip all bits for negatives and
// flip just the sign bit for non-negatives (a standard trick).
std::vector<std::uint8_t> encodeFloatKey(float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    std::uint32_t mask = (bits & 0x80000000u) ? 0xFFFFFFFFu : 0x80000000u;
    bits ^= mask;
    std::vector<std::uint8_t> out(4);
    out[0] = static_cast<std::uint8_t>((bits >> 24) & 0xFFu);
    out[1] = static_cast<std::uint8_t>((bits >> 16) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((bits >>  8) & 0xFFu);
    out[3] = static_cast<std::uint8_t>( bits        & 0xFFu);
    return out;
}

float decodeFloatKey(std::span<const std::uint8_t> k) {
    if (k.size() < 4) return 0.0f;
    std::uint32_t bits = (static_cast<std::uint32_t>(k[0]) << 24) |
                         (static_cast<std::uint32_t>(k[1]) << 16) |
                         (static_cast<std::uint32_t>(k[2]) <<  8) |
                         (static_cast<std::uint32_t>(k[3]));
    std::uint32_t mask = (bits & 0x80000000u) ? 0xFFFFFFFFu : 0x80000000u;
    bits ^= mask;
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// VARCHAR — the raw bytes themselves. Caller is responsible for the column's
// declared length; the B+ tree treats the Key as opaque.
std::vector<std::uint8_t> encodeStrKey(std::string_view s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string decodeStrKey(std::span<const std::uint8_t> k) {
    return std::string(reinterpret_cast<const char*>(k.data()), k.size());
}

} // namespace minidb::index
