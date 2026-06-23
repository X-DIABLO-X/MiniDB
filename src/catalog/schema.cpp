// =============================================================================
// src/catalog/schema.cpp
// -----------------------------------------------------------------------------
// Schema helpers (offsets, row size, encode/decode).
//
// Wire format used by the catalog page:
//   [ u16  n_columns ]
//   for each column:
//     [ u16  name_len ]
//     [ name_len bytes : name ]
//     [ u8   type (Type enum) ]
//     [ u32  length ]
//     [ u8   nullable ? 1 : 0 ]
//     [ u8   isPrimaryKey ? 1 : 0 ]
// =============================================================================
#include "catalog/schema.h"

#include <cstring>
#include <span>
#include <stdexcept>

namespace minidb::catalog {

namespace {
constexpr std::size_t TYPE_SIZES[] = {
    /* INT     */ 4,
    /* FLOAT   */ 4,
    /* VARCHAR */ 0,   // length is taken from the Column declaration
    /* BOOL    */ 1,
};

template <typename T>
void appendPOD(std::vector<std::uint8_t>& out, const T& v) {
    const std::size_t off = out.size();
    out.resize(off + sizeof(T));
    std::memcpy(out.data() + off, &v, sizeof(T));
}

void appendStr(std::vector<std::uint8_t>& out, const std::string& s) {
    const std::uint16_t n = static_cast<std::uint16_t>(s.size());
    appendPOD<std::uint16_t>(out, n);
    if (n) {
        const std::size_t off = out.size();
        out.resize(off + n);
        std::memcpy(out.data() + off, s.data(), n);
    }
}

template <typename T>
bool readPOD(std::span<const std::uint8_t> in, std::size_t& off, T& out) {
    if (off + sizeof(T) > in.size()) return false;
    std::memcpy(&out, in.data() + off, sizeof(T));
    off += sizeof(T);
    return true;
}

bool readStr(std::span<const std::uint8_t> in, std::size_t& off, std::string& out) {
    std::uint16_t n = 0;
    if (!readPOD<std::uint16_t>(in, off, n)) return false;
    if (off + n > in.size()) return false;
    out.assign(reinterpret_cast<const char*>(in.data() + off), n);
    off += n;
    return true;
}
} // namespace

void Schema::addColumn(Column c) {
    cols_.push_back(std::move(c));
}

std::size_t Schema::numColumns() const { return cols_.size(); }
const Column& Schema::column(std::size_t i) const { return cols_.at(i); }

std::size_t Schema::columnOffset(const std::string& name) const {
    std::size_t off = 0;
    for (const auto& c : cols_) {
        if (c.name == name) return off;
        std::size_t sz = (c.type == Type::VARCHAR) ? c.length : TYPE_SIZES[static_cast<int>(c.type)];
        off += sz;
    }
    return static_cast<std::size_t>(-1);
}

std::size_t Schema::rowSize() const {
    std::size_t total = 0;
    for (const auto& c : cols_) {
        total += (c.type == Type::VARCHAR) ? c.length : TYPE_SIZES[static_cast<int>(c.type)];
    }
    return total;
}

// Encode the schema into a byte vector. Used by CatalogManager::flush.
void Schema::encode(std::vector<std::uint8_t>& out) const {
    const std::uint16_t n = static_cast<std::uint16_t>(cols_.size());
    appendPOD<std::uint16_t>(out, n);
    for (const auto& c : cols_) {
        appendStr(out, c.name);
        const std::uint8_t t = static_cast<std::uint8_t>(c.type);
        appendPOD<std::uint8_t>(out, t);
        const std::uint32_t len = c.length;
        appendPOD<std::uint32_t>(out, len);
        const std::uint8_t nullable = c.nullable ? 1 : 0;
        const std::uint8_t isPk     = c.isPrimaryKey ? 1 : 0;
        appendPOD<std::uint8_t>(out, nullable);
        appendPOD<std::uint8_t>(out, isPk);
    }
}

// Inverse of encode. Returns an empty schema on truncated input.
Schema Schema::decode(std::span<const std::uint8_t> in, std::size_t& off) {
    Schema s;
    std::uint16_t n = 0;
    if (!readPOD<std::uint16_t>(in, off, n)) return s;
    for (std::uint16_t i = 0; i < n; ++i) {
        Column c;
        if (!readStr(in, off, c.name)) return s;
        std::uint8_t t = 0;
        if (!readPOD<std::uint8_t>(in, off, t)) return s;
        c.type = static_cast<Type>(t);
        if (!readPOD<std::uint32_t>(in, off, c.length)) return s;
        std::uint8_t nullable = 0, isPk = 0;
        if (!readPOD<std::uint8_t>(in, off, nullable)) return s;
        if (!readPOD<std::uint8_t>(in, off, isPk)) return s;
        c.nullable = (nullable != 0);
        c.isPrimaryKey = (isPk != 0);
        s.addColumn(std::move(c));
    }
    return s;
}

} // namespace minidb::catalog