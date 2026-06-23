# `catalog/` — table metadata

The catalog is the *only* module that knows about table names and column
types. Everything else (executor, index, planner) goes through it.

## Files in this folder

| Header | Responsibility |
|---|---|
| `include/catalog/schema.h`         | `Type` enum, `Column`, `Schema` (list of columns). |
| `include/catalog/table_info.h`     | `TableInfo` (schema + heap head page id + primary index). |
| `include/catalog/catalog_manager.h`| `CatalogManager` — create/drop/open tables; persist to page 0. |

The on-disk format is documented in `docs/catalog.md`.

## Public API (contract)

```cpp
namespace minidb::catalog {

    enum class Type : std::uint8_t { INT, FLOAT, VARCHAR, BOOL };

    struct Column {
        std::string name;
        Type        type;
        std::uint32_t length;     // bytes, meaningful for VARCHAR
        bool        nullable;
        bool        isPrimaryKey;
    };

    class Schema {
    public:
        void                 addColumn(Column c);
        std::size_t          numColumns() const;
        const Column&        column(std::size_t i) const;
        std::size_t          columnOffset(const std::string& name) const;
        std::size_t          rowSize() const;
        const std::vector<Column>& columns() const;
    };

    struct TableInfo {
        std::string name;
        Schema      schema;
        PageId      firstPageId;
        std::string primaryIndexName;
    };

    class CatalogManager {
    public:
        explicit CatalogManager(storage::DiskManager* dm);
        ~CatalogManager();

        Status load();
        Status flush();

        Status createTable(const TableInfo& info);
        Status dropTable  (const std::string& name);
        const TableInfo*  getTable(const std::string& name) const;
        std::vector<std::string> listTables() const;

        void     setCardinality(const std::string& table, std::uint64_t n);
        std::uint64_t cardinality(const std::string& table) const;   // 0 = unknown
    };
}
```

## How other modules use the catalog

| Module | Calls |
|---|---|
| `storage/HeapFile` | `getTable(name)` → `info.schema.rowSize()` to size records. |
| `index/IndexManager` | `getTable(name)` → `info.schema.columnOffset(col)` for key extraction. |
| `executor/*` | `getTable(name)` → `info.schema.columns()` for tuple materialisation. |
| `planner/Optimizer` | `getTable(name)` and `cardinality(name)` for cost estimation. |

## Storage of the catalog itself

The catalog lives on **page 0** of the data file. Page 0 is reserved
unconditionally at DB creation. See `docs/catalog.md` for the byte layout.

## Rules

- The catalog manager **does not** hold long-lived references to
  `TableInfo` that other modules could dereference after `dropTable`. After
  `dropTable`, every existing `const TableInfo*` is invalid.
- `getTable` returns `nullptr` if the table does not exist. Callers MUST
  check.
- Schema is **append-only at the column level** for v1. `ALTER TABLE
  ADD/DROP COLUMN` is out of scope (see `README.md` §13).
