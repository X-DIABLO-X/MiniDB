# MiniDB Public Interfaces (THE contract)

> This file is the single source of truth for cross-module function calls.
> If a function is not listed here, other modules must not call it.
> If you change a signature here, announce it in chat **before** merging.

All identifiers live in the `minidb` namespace unless stated otherwise.

---

## 0. Common types (`include/common/`)

```cpp
namespace minidb {

using PageId       = uint32_t;     // physical page id (0 = invalid)
using TransactionId = uint64_t;    // monotonically increasing txn id
using RecordId     = uint64_t;     // (pageId << 32) | slotIdx
using FrameId      = int32_t;      // buffer-pool frame index (-1 = invalid)
using LSN          = uint64_t;     // log sequence number

using Key          = std::string;  // generic, comparable key (B+ tree key)

enum class Status : int {
    OK                  = 0,
    UNIMPLEMENTED       = 1,   // stub - safe to call, returns dummy data
    NOT_FOUND           = 2,
    DUPLICATE_KEY       = 3,
    FULL                = 4,
    IO_ERROR            = 5,
    INVALID_ARGUMENT    = 6,
    TYPE_MISMATCH       = 7,
    DEADLOCK            = 8,
    ABORTED             = 9,
    TXN_CONFLICT        = 10,  // MVCC write-write conflict
};

constexpr PageId       INVALID_PAGE_ID   = 0;
constexpr FrameId      INVALID_FRAME_ID  = -1;
constexpr TransactionId INVALID_TXN_ID   = 0;
constexpr RecordId     INVALID_RID       = 0;
constexpr LSN          INVALID_LSN       = 0;

// helpers (defined in include/common/record_id.h)
RecordId makeRid(PageId pageId, uint16_t slotIdx);
PageId    ridPage(RecordId rid);
uint16_t  ridSlot (RecordId rid);

} // namespace minidb
```

---

## 1. Storage (`include/storage/`)

### `Page` — `include/storage/page.h`
A 4 KB byte buffer with an in-header slot directory. Layout is described in
`include/storage/README.md`.

```cpp
class Page {
public:
    static constexpr uint32_t SIZE = 4096;

    Page();
    ~Page() = default;

    PageId  getPageId() const;
    void    setPageId(PageId id);
    uint8_t*       data();        // raw bytes (header + records)
    const uint8_t* data() const;

    // slot directory helpers (see storage/README.md for byte layout)
    uint16_t slotCount() const;
    bool     insertRecord(uint16_t slot, std::span<const uint8_t> bytes);
    std::span<const uint8_t> getRecord(uint16_t slot) const;
    bool     deleteRecord(uint16_t slot);
    uint16_t firstFreeSlot() const;

    void     reset();             // zero out, set pageId = INVALID_PAGE_ID
    bool     isDirty() const;
    void     setDirty(bool d);
};
```

### `DiskManager` — `include/storage/disk_manager.h`
Owns `data/pages/`. Page id 0 is reserved for the catalog bootstrap page.

```cpp
class DiskManager {
public:
    explicit DiskManager(const std::string& dbPath);
    ~DiskManager();

    Status readPage (PageId pageId, Page& out);
    Status writePage(PageId pageId, const Page& page);
    PageId allocatePage();
    Status freePage (PageId pageId);

    void   flush();               // fsync the data file
};
```

### `BufferPool` — `include/storage/buffer_pool.h`

```cpp
class BufferPool {
public:
    explicit BufferPool(DiskManager* disk, size_t numFrames = 64);
    ~BufferPool();

    // Returns a pinned frame. If the page is in cache, just pin it.
    // Otherwise fetch from disk, possibly evicting a victim.
    Status fetchPage(PageId pageId, Page*& outPage);

    // Decrement pin count. If pin count hits 0 the frame is evictable.
    Status unpinPage(PageId pageId, bool isDirty);

    Status flushPage (PageId pageId);
    Status flushAll  ();
    size_t numFrames () const;
};
```

> The `Page*` returned by `fetchPage` is owned by the BufferPool and is
> **only valid until the next `unpinPage`**. Hold the pin while you use it.

### `HeapFile` — `include/storage/heap_file.h`

```cpp
class HeapFile {
public:
    HeapFile(BufferPool* bp, const catalog::TableInfo* info);
    ~HeapFile();

    // Insert a serialised record. Returns its RecordId.
    Status insertRecord(std::span<const uint8_t> record, RecordId& outRid);

    // Fetch by RecordId. Buffer must outlive the returned span.
    Status getRecord(RecordId rid, std::span<const uint8_t>& out) const;

    Status deleteRecord(RecordId rid);

    // Iterator for SeqScan executor. See executor/README.md.
    class Iterator {
    public:
        bool  next(RecordId& rid, std::span<const uint8_t>& bytes);
        void  close();
    };
    std::unique_ptr<Iterator> scan();
};
```

---

## 2. Catalog (`include/catalog/`)

```cpp
namespace minidb::catalog {

enum class Type { INT, FLOAT, VARCHAR, BOOL };

struct Column {
    std::string name;
    Type        type;
    uint32_t    length;        // bytes; meaningful for VARCHAR
    bool        nullable;
    bool        isPrimaryKey;
};

class Schema {
public:
    void        addColumn(Column c);
    const Column& column(size_t i) const;
    size_t      numColumns() const;
    size_t      columnOffset(const std::string& name) const;
    size_t      rowSize() const;       // fixed part, VARCHARs spill to overflow page (out of scope)
};

struct TableInfo {
    std::string    name;
    Schema         schema;
    PageId         firstPageId;        // head of heap file's free list
    std::string    primaryIndexName;
};

class CatalogManager {
public:
    explicit CatalogManager(DiskManager* disk);
    ~CatalogManager();

    Status createTable(const TableInfo& info);
    Status dropTable  (const std::string& name);
    const TableInfo* getTable(const std::string& name) const;
    std::vector<std::string> listTables() const;

    // persistence helpers
    Status load();                      // called once at startup
    Status flush();                     // called on graceful shutdown
};

} // namespace minidb::catalog
```

---

## 3. Index (`include/index/`)

```cpp
namespace minidb::index {

class BPlusTree {
public:
    BPlusTree(BufferPool* bp, PageId rootPageId);
    ~BPlusTree();

    Status search        (const Key& key, RecordId& outRid);
    Status insert        (const Key& key, RecordId rid);
    Status remove        (const Key& key);
    Status rangeScan     (const Key& lo, const Key& hi,
                          std::vector<RecordId>& out);
    PageId rootPageId() const;
};

class IndexManager {
public:
    explicit IndexManager(BufferPool* bp, catalog::CatalogManager* cat);
    ~IndexManager();

    Status createIndex (const std::string& table,
                        const std::string& column,
                        const std::string& indexName);
    Status dropIndex   (const std::string& indexName);

    // Returns nullptr if the index does not exist.
    BPlusTree* open     (const std::string& indexName);
    std::vector<std::string> list() const;
};

} // namespace minidb::index
```

---

## 4. Parser (`include/parser/`)

```cpp
namespace minidb::parser {

enum class StmtKind { SELECT, INSERT, DELETE, CREATE, DROP, TXN };

struct Stmt { /* tag-discriminated union of statement shapes; see ast.h */ };

class Lexer {
public:
    explicit Lexer(std::string_view source);
    std::vector<Token> tokenize();
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    Stmt parse();                      // returns one top-level statement
    std::vector<Stmt> parseScript();   // ; separated
};

} // namespace minidb::parser
```

The actual AST node types are defined in `include/parser/ast.h`. The
executor consumes them through the planner — never directly.

---

## 5. Planner (`include/planner/`)

```cpp
namespace minidb::planner {

class LogicalPlan { /* opaque relational-algebra tree */ };
class PhysicalPlan { /* opaque operator tree */ };

class Optimizer {
public:
    Optimizer(catalog::CatalogManager* cat,
              index::IndexManager*     idx,
              transaction::TransactionManager* txn);

    std::unique_ptr<PhysicalPlan> optimize(const parser::Stmt& s);
};

} // namespace minidb::planner
```

The exact operator node types are defined in
`include/planner/{logical,physical}_plan.h` — the executor depends on these.

---

## 6. Executor (`include/executor/`)

All executors follow the Volcano pattern:

```cpp
class SomeExecutor {
public:
    Status init();
    Status next(Tuple& out);   // returns OK / DONE / error
    Status close();
};
```

```cpp
namespace minidb::executor {

class SeqScanExecutor      { /* ... */ };
class IndexScanExecutor    { /* ... */ };
class FilterExecutor       { /* ... */ };
class ProjectExecutor      { /* ... */ };
class NestedLoopJoinExecutor { /* ... */ };
class HashJoinExecutor     { /* ... */ };
class AggregateExecutor    { /* ... */ };
class SortExecutor         { /* ... */ };
class LimitExecutor        { /* ... */ };
class InsertExecutor       { /* ... */ };
class DeleteExecutor       { /* ... */ };

// Top-level façade:
class QueryEngine {
public:
    QueryEngine(BufferPool* bp,
                catalog::CatalogManager* cat,
                index::IndexManager* idx,
                transaction::TransactionManager* txn,
                recovery::RecoveryManager* rec);

    std::vector<Tuple> execute(const std::string& sql);
    Status             executeUpdate(const std::string& sql);
};

} // namespace minidb::executor
```

---

## 7. Transaction (`include/transaction/`)

```cpp
namespace minidb::transaction {

enum class TxnState { ACTIVE, COMMITTED, ABORTED };
enum class LockMode { SHARED, EXCLUSIVE };

class LockManager {
public:
    Status acquireShared    (TransactionId txn, RecordId rid);
    Status acquireExclusive (TransactionId txn, RecordId rid);
    void   releaseAll       (TransactionId txn);     // 2PL: at commit/abort
    bool   hasCycle();                              // for tests/benchmarks
};

class Transaction {
public:
    TransactionId id() const;
    TxnState      state() const;
    // MVCC snapshot:
    TransactionId snapshotHigh() const;            // max active txn at begin
};

class TransactionManager {
public:
    TransactionId begin();
    Status commit  (TransactionId txn);
    Status abort   (TransactionId txn);

    // For MVCC executor:
    bool isVisible(TransactionId rowCreator,
                   TransactionId rowDeleter,
                   const Transaction& reader);

    // For 2PL baseline:
    LockManager& lockManager();
};

} // namespace minidb::transaction
```

---

## 8. Recovery (`include/recovery/`)

```cpp
namespace minidb::recovery {

enum class LogKind { BEGIN, COMMIT, ABORT, INSERT, UPDATE, DELETE, CHECKPOINT };

struct LogRecord {
    LogKind        kind;
    TransactionId  txnId;
    LSN            prevLSN;
    // payload depends on `kind` (see include/recovery/log_record.h)
};

class WAL {
public:
    explicit WAL(const std::string& path);
    ~WAL();

    LSN   append (const LogRecord& r);     // returns assigned LSN
    Status flush ();                       // fsync
    Status read  (LSN from, std::function<bool(const LogRecord&)> visit);
    LSN   currentLSN() const;
};

class RecoveryManager {
public:
    RecoveryManager(WAL* wal,
                    BufferPool* bp,
                    catalog::CatalogManager* cat,
                    index::IndexManager* idx,
                    transaction::TransactionManager* txn);

    Status runAtStartup();     // ARIES: analysis + redo + undo
};

} // namespace minidb::recovery
```

---

## Integration rule (the only rule that matters)

> **Call sites in code must use exactly the names in this file.**
> "Exactly" means: same function name, same parameter list, same return type,
> same namespace. If you find yourself wanting a different one, you must
> (a) add it here, (b) get it approved, (c) merge the docs change **before**
> the code change.

Anti-pattern (banned):

```cpp
auto* root = bplus.rootNode;        // reaching into private state
root->children[0] = ...;
```

Allowed:

```cpp
RecordId rid;
bplus.search(key, rid);
```
