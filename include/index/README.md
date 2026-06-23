# `index/` — B+ tree and index manager

A B+ tree indexes a `(Key → RecordId)` map for one column of one table.
The leaf level is a doubly-linked list of pages so range scans are O(log N)
seek + linear scan.

## Files in this folder

| Header | Responsibility |
|---|---|
| `include/index/node.h`          | On-disk node layout (internal vs leaf). |
| `include/index/bplus_tree.h`    | `BPlusTree` (search/insert/remove/rangeScan). |
| `include/index/index_manager.h` | `IndexManager` — opens/creates/drops B+ trees. |

## Public API (contract)

```cpp
namespace minidb::index {

class BPlusTree {
public:
    BPlusTree(storage::BufferPool* bp, PageId rootPageId);
    ~BPlusTree();

    Status search(const Key& key, RecordId& outRid);
    Status insert(const Key& key, RecordId rid);
    Status remove(const Key& key);
    Status rangeScan(const Key& lo, const Key& hi, std::vector<RecordId>& out);
    PageId rootPageId() const;
};

class IndexManager {
public:
    IndexManager(storage::BufferPool* bp, catalog::CatalogManager* cat);
    ~IndexManager();

    Status createIndex(const std::string& table,
                       const std::string& column,
                       const std::string& indexName);
    Status dropIndex  (const std::string& indexName);

    // Returns nullptr if no such index.
    BPlusTree* open    (const std::string& indexName);
    std::vector<std::string> list() const;
};

}
```

## Node layout (4 KB page, see `node.h`)

```text
+----------------------- 0x000 -----------------------+
| isLeaf               | uint8                        |
+-----------------------------------------------------+
| numKeys              | uint16                       |
+-----------------------------------------------------+
| parentPageId         | uint32   (0 = no parent)     |
+-----------------------------------------------------+
| nextLeaf / child0    | uint32                       |   <-- varies by isLeaf
|   if internal: child0, key0, child1, key1, ..., childN
|   if leaf    : key0, rid0, key1, rid1, ..., keyN-1, ridN-1
+-----------------------------------------------------+
```

Leaf pages also store `prevLeaf` / `nextLeaf` in the page header to allow
range scans to traverse without re-descending from the root.

## How other modules use the index

| Module | Calls |
|---|---|
| `executor/IndexScan` | `IndexManager::open(name)` → `BPlusTree::search` or `rangeScan`. |
| `executor/Insert/Delete` | `BPlusTree::insert` / `remove` to keep the index in sync. |
| `recovery/RecoveryManager` | `IndexManager::list()` to redo index updates on crash recovery. |

## Rules

- B+ trees are clustered on the primary key by default (table order = PK
  order) and unclustered on secondary indexes.
- Insert/delete pin pages through the BufferPool; the B+ tree never
  touches the DiskManager directly.
- The on-disk key is whatever the `IndexManager::createIndex` writer
  encoded. For an `INT` column it's 4 bytes big-endian; for `VARCHAR` it's
  length-prefixed bytes; for `FLOAT` it's the IEEE-754 bit pattern. The
  `BPlusTree` itself treats all keys as opaque `Key` blobs.
- Tree height is bounded by `ceil(log_F(N))` where `F` is the fanout. For
  v1 we use `F ≈ 100` (4 KB page, ~40 bytes per entry).