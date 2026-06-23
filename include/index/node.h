// =============================================================================
// include/index/node.h
// -----------------------------------------------------------------------------
// Layout of a B+ tree node on a single page. See include/index/README.md.
//
// The Node class is a *view* over a Page buffer — it does not own storage.
// It is what BPlusTree uses to read/write nodes without touching the
// BufferPool directly.
// =============================================================================
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "common/types.h"
#include "storage/page.h"

namespace minidb::index {

struct Node {
    std::uint8_t*       buf;     // points into a Page buffer (must outlive Node)
    const std::uint8_t* end;     // buf + Page::SIZE

    bool         isLeaf() const;
    void         setLeaf(bool v);
    std::uint16_t numKeys() const;
    void         setNumKeys(std::uint16_t n);

    PageId       parent() const;
    void         setParent(PageId p);

    // Leaf page header
    PageId       prevLeaf() const;
    void         setPrevLeaf(PageId p);
    PageId       nextLeaf() const;
    void         setNextLeaf(PageId p);

    // Key/value accessors. Caller must check isLeaf() and stay within numKeys.
    std::span<const std::uint8_t> key(std::uint16_t i) const;
    std::span<std::uint8_t>       key(std::uint16_t i);
    RecordId                      rid(std::uint16_t i) const;
    void                          setRid(std::uint16_t i, RecordId r);

    PageId                        child(std::uint16_t i) const;
    void                          setChild(std::uint16_t i, PageId p);
};

// Helpers for the BPlusTree to (de)serialise keys.
std::vector<std::uint8_t> encodeIntKey(int32_t v);
std::vector<std::uint8_t> encodeFloatKey(float v);
std::vector<std::uint8_t> encodeStrKey(std::string_view s);
int32_t  decodeIntKey  (std::span<const std::uint8_t> k);
float    decodeFloatKey(std::span<const std::uint8_t> k);
std::string decodeStrKey(std::span<const std::uint8_t> k);

} // namespace minidb::index