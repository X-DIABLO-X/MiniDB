# MiniDB Catalog (table metadata)

The catalog is the only piece of state that *every* module needs to read at
runtime. It is loaded once at startup by `CatalogManager::load()` from
`data/metadata/catalog.dat`.

This document is the contract between **storage**, **index**, **executor**,
and **planner** for "where do I find the schema of table X?".

---

## 1. On-disk layout

`data/metadata/catalog.dat` is a single fixed-size page (page 0 — reserved
for the catalog) containing:

```text
+-----------------------------------------------+
| MAGIC ("MNDB")                  | 6 bytes     |
+---------------------------------+-------------+
| VERSION                        | uint16      |
+---------------------------------+-------------+
| table_count                    | uint16      |
+---------------------------------+-------------+
| for each table:                                |
|   name_len   | uint16                         |
|   name       | name_len bytes (UTF-8, NUL pad) |
|   column_count | uint16                       |
|   for each column:                            |
|     name_len | uint16                          |
|     name     | name_len bytes                  |
|     type     | uint8 (see Type enum)           |
|     length   | uint32                          |
|     nullable | uint8                           |
|     isPK     | uint8                           |
|   firstPageId | uint32                         |
|   pkIndexName_len | uint16                     |
|   pkIndexName | pkIndexName_len bytes          |
+-----------------------------------------------+
| CHECKSUM (crc32 of bytes above) | uint32      |
+-----------------------------------------------+
```

If the file does not exist, the catalog starts empty and is materialised on
the first `CREATE TABLE`.

---

## 2. In-memory representation

```cpp
namespace minidb::catalog {

enum class Type : uint8_t { INT, FLOAT, VARCHAR, BOOL };

struct Column {
    std::string name;
    Type        type;
    uint32_t    length;     // bytes
    bool        nullable;
    bool        isPrimaryKey;
};

class Schema {
    std::vector<Column> cols_;
public:
    void             addColumn(Column c);
    size_t           numColumns() const;
    const Column&    column(size_t i) const;
    size_t           columnOffset(const std::string& name) const;
    size_t           rowSize() const;            // fixed part only
    const std::vector<Column>& columns() const;
};

struct TableInfo {
    std::string name;
    Schema      schema;
    PageId      firstPageId;        // 0 = unallocated
    std::string primaryIndexName;   // "" = none yet
};

class CatalogManager {
public:
    explicit CatalogManager(storage::DiskManager* dm);
    Status load();
    Status flush();

    Status createTable(const TableInfo& info);
    Status dropTable  (const std::string& name);
    const TableInfo*  getTable(const std::string& name) const;
    std::vector<std::string> listTables() const;
};

} // namespace minidb::catalog
```

---

## 3. Record layout in a heap page

A row on a heap page is the **fixed-size** concatenation of the column
values, in schema order. `VARCHAR(n)` columns are stored inline (capped at
`n` bytes) for the v1 demo; overflow pages are out of scope.

```text
| INT col0 | FLOAT col1 | VARCHAR(20) col2 (20 bytes, NUL-padded) | BOOL col3 |
| 4 bytes  | 4 bytes    | 20 bytes                                 | 1 byte   |
```

The on-page tuple does **not** carry the schema. The executor reads the
schema from `TableInfo` and slices the tuple accordingly.

---

## 4. How other modules use the catalog

| Module | Call site | Why |
|---|---|---|
| `storage/HeapFile` | `CatalogManager::getTable(name)` | needs `Schema::rowSize()` to size records. |
| `index/IndexManager` | `CatalogManager::getTable(name)` | needs column type to choose a comparator. |
| `executor/*` | `CatalogManager::getTable(name)` | needs `Schema` to materialise a `Tuple`. |
| `planner/Optimizer` | `CatalogManager::getTable(name)` | needs `Schema::rowSize()` for cost estimation. |
| `parser/Parser` | *(does not use the catalog)* | parsing is schema-agnostic; validation happens in the planner. |

> **Bootstrap.** Page 0 of the data file is reserved for the catalog. The
> first thing `CatalogManager::load()` does is read page 0; if the magic
> doesn't match it starts a fresh catalog.

---

## 5. Concurrency

The catalog is **read-mostly at runtime** — `CREATE TABLE` / `DROP TABLE`
take an exclusive table-level lock via `LockManager` (2PL baseline) or
append a `CATALOG` log record (MVCC path). All other modules cache the
`TableInfo*` they got at plan time; they re-fetch on miss.

---

## 6. What the planner needs that isn't in `TableInfo`

The planner needs **cardinality estimates** to pick join orders. We keep
those in a tiny sidecar file `data/metadata/stats.dat` (one uint64 per
table). The catalog loads them in `load()` and exposes them as:

```cpp
class CatalogManager {
    // ...
    void   setCardinality(const std::string& table, uint64_t n);
    uint64_t cardinality(const std::string& table) const;   // 0 = unknown
};
```

Updated on `COMMIT` by a background updater (out of scope to make
incremental — a full `ANALYZE` is fine for the demo).
