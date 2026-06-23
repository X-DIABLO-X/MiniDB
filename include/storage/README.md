# `storage/` — pages, disk, buffer pool, heap files

The storage layer is the **foundation**. Every other module eventually calls
into it. Implement it first (M1 in the project brief).

## Files in this folder

| Header | Responsibility |
|---|---|
| `include/storage/page.h`         | A 4 KB byte buffer with an in-page slot directory. |
| `include/storage/disk_manager.h` | Maps `PageId` → file offset, owns `data/pages/`. |
| `include/storage/buffer_pool.h`  | In-memory page cache, LRU eviction, pin/unpin. |
| `include/storage/heap_file.h`    | Slotted pages, free-page list, hands out `RecordId`s. |

Source files mirror these in `src/storage/`.

## Public API (the contract other modules code against)

```cpp
// page.h
namespace minidb::storage {
    class Page {
        static constexpr uint32_t SIZE = 4096;
        PageId  getPageId() const;
        void    setPageId(PageId);
        uint8_t*       data();
        const uint8_t* data() const;
        uint16_t slotCount() const;
        bool    insertRecord(uint16_t slot, std::span<const uint8_t> bytes);
        std::span<const uint8_t> getRecord(uint16_t slot) const;
        bool    deleteRecord(uint16_t slot);
        uint16_t firstFreeSlot() const;
        void    reset();
        bool    isDirty() const;
        void    setDirty(bool);
    };
}

// disk_manager.h
namespace minidb::storage {
    class DiskManager {
        explicit DiskManager(const std::string& dbPath);
        ~DiskManager();
        Status readPage (PageId, Page& out);
        Status writePage(PageId, const Page&);
        PageId allocatePage();
        Status freePage (PageId);
        void   flush();
    };
}

// buffer_pool.h
namespace minidb::storage {
    class BufferPool {
        explicit BufferPool(DiskManager* disk, size_t numFrames = 64);
        ~BufferPool();
        Status fetchPage(PageId, Page*& outPage);   // returned page is pinned
        Status unpinPage(PageId, bool isDirty);
        Status flushPage(PageId);
        Status flushAll();
        size_t numFrames() const;
    };
}

// heap_file.h
namespace minidb::storage {
    class HeapFile {
        HeapFile(BufferPool*, const catalog::TableInfo*);
        ~HeapFile();
        Status insertRecord(std::span<const uint8_t>, RecordId& outRid);
        Status getRecord(RecordId, std::span<const uint8_t>& out) const;
        Status deleteRecord(RecordId);
        class Iterator { bool next(RecordId&, std::span<const uint8_t>&); void close(); };
        std::unique_ptr<Iterator> scan();
    };
}
```

## Page on-disk layout (4 KB)

```text
+----------------------- 0x000 -----------------------+
| pageId               | uint32                      |   4 bytes
+----------------------------------------------------+
| pageLSN              | uint64                      |   8 bytes   (for recovery redo)
+----------------------------------------------------+
| slotCount            | uint16                      |   2 bytes
+----------------------------------------------------+
| freeSpacePtr         | uint16                      |   2 bytes
+----------------------------------------------------+
| dirty                | uint8                       |   1 byte    (in-memory only; not flushed as part of dirty bit)
+----------------------------------------------------+
| padding              | 47 bytes                    |   (align)
+----------------------------------------------------+
| slot directory                                       |
|   for i in 0..slotCount-1:                          |
|     offset  | uint16                                |
|     length  | uint16                                |
+----------------------------------------------------+
| free space                                            |
+----------------------------------------------------+
| (records grow downward from end of page)             |
+----------------------------------------------------+
```

The `Page` API in `page.h` hides this layout — the only methods on `Page`
that other modules call are the ones listed in the public API above.

## Ownership and lifetime

- The `BufferPool` owns all `Page` memory. A pointer returned from
  `fetchPage` is **valid only until the matching `unpinPage`**.
- `DiskManager` owns the data file and is constructed once at startup.
- `HeapFile` is **not** owned by the storage layer; it is constructed by
  the executor (or by the catalog when opening a table).

## How other modules use the storage layer

| Module | Calls |
|---|---|
| `catalog/CatalogManager` | `DiskManager::readPage(0, …)` to load the catalog. |
| `index/IndexManager`     | `BufferPool::fetchPage` for B+ tree nodes. |
| `executor/SeqScan`       | `HeapFile::scan()` to walk all rows. |
| `executor/IndexScan`     | `HeapFile::getRecord(rid)` to materialise matched rows. |
| `executor/Insert/Delete` | `HeapFile::insertRecord` / `deleteRecord`. |
| `recovery/RecoveryManager` | `BufferPool::flushAll` and `DiskManager::flush` at the end of recovery. |

## Rules

- The `BufferPool` is the **only** object that holds `Page*` pointers.
  `DiskManager` and `HeapFile` borrow pages through it.
- `Page::data()` is intentionally `uint8_t*` — the higher layers interpret
  the bytes according to the table's schema (see `docs/catalog.md`).
- The storage layer **does not know about transactions**. Concurrency is
  the transaction layer's job; the storage layer just provides the
  pin/unpin protocol that lets the lock manager hold a page while a lock
  is held.
