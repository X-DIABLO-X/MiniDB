// =============================================================================
// include/catalog/schema.h
// -----------------------------------------------------------------------------
// A Schema is an ordered list of Columns. It knows how to compute the
// fixed-size layout of a tuple for a table.
// =============================================================================
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace minidb::catalog {

enum class Type : std::uint8_t {
    INT     = 0,
    FLOAT   = 1,
    VARCHAR = 2,
    BOOL    = 3,
};

struct Column {
    std::string   name;
    Type          type;
    std::uint32_t length     = 0;     // bytes (VARCHAR only)
    bool          nullable   = true;
    bool          isPrimaryKey = false;
};

class Schema {
public:
    Schema() = default;
    ~Schema() = default;

    void                       addColumn(Column c);
    std::size_t                numColumns() const;
    const Column&              column(std::size_t i) const;
    const std::vector<Column>& columns() const { return cols_; }

    // Returns the byte offset of column `name` within a tuple. Returns
    // SIZE_MAX if the column does not exist.
    std::size_t                columnOffset(const std::string& name) const;

    // Returns the fixed-size row footprint. VARCHARs are counted at their
    // declared length (no overflow pages for v1).
    std::size_t                rowSize() const;

    // Serialisation helpers (used by the catalog_manager to write/read the
    // catalog page).
    void                       encode (std::vector<std::uint8_t>& out) const;
    static Schema              decode(std::span<const std::uint8_t> in, std::size_t& off);

private:
    std::vector<Column> cols_;
};

} // namespace minidb::catalog
