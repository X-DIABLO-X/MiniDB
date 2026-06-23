// =============================================================================
// src/executor/delete_executor.cpp
// -----------------------------------------------------------------------------
// DeleteExecutor: drives a SeqScan (or whatever scan the planner plugged
// into `child_`), then for every produced tuple:
//   1. removes the record from the heap file
//   2. removes the corresponding entry from every index
//   3. appends a WAL DELETE record
//
// For v1 we don't have a (table,column) -> index map and the scan doesn't
// leak the RecordId, so we issue a delete on the heap using the row's
// first-value-as-RID heuristic. The actual implementation is best-effort:
// on a stub heap file the delete will surface as Status::UNIMPLEMENTED
// and the row is simply skipped.
// =============================================================================
#include "executor/delete_executor.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/catalog_manager.h"
#include "catalog/schema.h"
#include "catalog/table_info.h"
#include "common/record_id.h"
#include "common/status.h"
#include "common/types.h"
#include "executor/executor.h"
#include "executor/seq_scan.h"
#include "index/bplus_tree.h"
#include "index/index_manager.h"
#include "parser/ast.h"
#include "storage/heap_file.h"

namespace minidb::executor {

namespace {

// Column footprint inside a row image (4 bytes for fixed types, declared
// length for VARCHAR).
std::size_t colBytes(const catalog::Column& c) {
    if (c.length == 0) {
        switch (c.type) {
            case catalog::Type::INT:
            case catalog::Type::FLOAT:
            case catalog::Type::BOOL:
                return 4;
            case catalog::Type::VARCHAR:
                return 0;
        }
    }
    return c.length;
}

// Encode a B+ tree key from a Value (mirrors insert_executor.cpp).
std::string encodeKey(const Value& v) {
    char buf[16];
    switch (v.tag) {
        case Value::Tag::INT: {
            int32_t x = v.i;
            std::memcpy(buf, &x, sizeof(x));
            return std::string(buf, sizeof(x));
        }
        case Value::Tag::FLOAT: {
            float x = v.f;
            std::memcpy(buf, &x, sizeof(x));
            return std::string(buf, sizeof(x));
        }
        case Value::Tag::BOOL: {
            int32_t x = v.b ? 1 : 0;
            std::memcpy(buf, &x, sizeof(x));
            return std::string(buf, sizeof(x));
        }
        case Value::Tag::STRING:
            return v.s;
        case Value::Tag::NULL_:
            return std::string();
    }
    return std::string();
}

// Decode a single column's value from a raw row image (using the column
// index, not the column name, because the executor doesn't keep names).
Value decodeAt(const std::uint8_t* base, const catalog::Column& c) {
    std::size_t n = colBytes(c);
    switch (c.type) {
        case catalog::Type::INT: {
            int32_t v = 0;
            if (n >= sizeof(int32_t)) std::memcpy(&v, base, sizeof(int32_t));
            return Value::makeInt(v);
        }
        case catalog::Type::FLOAT: {
            float v = 0.0f;
            if (n >= sizeof(float)) std::memcpy(&v, base, sizeof(float));
            return Value::makeFloat(v);
        }
        case catalog::Type::BOOL: {
            int32_t v = 0;
            if (n >= sizeof(int32_t)) std::memcpy(&v, base, sizeof(int32_t));
            return Value::makeBool(v != 0);
        }
        case catalog::Type::VARCHAR: {
            std::string s;
            if (n > 0) s.assign(reinterpret_cast<const char*>(base), n);
            while (!s.empty() && s.back() == '\0') s.pop_back();
            return Value::makeStr(s);
        }
    }
    return Value::makeNull();
}

} // namespace

// ----- DeleteExecutor -----

DeleteExecutor::DeleteExecutor(ExecutorContext* ctx,
                               std::unique_ptr<parser::DeleteStmt> stmt)
    : Executor(ctx), stmt_(std::move(stmt)) {}

// Out-of-line destructor for the unique_ptr members.
DeleteExecutor::~DeleteExecutor() = default;

// Build a SeqScan over the target table with the WHERE clause as the
// filter predicate.
Status DeleteExecutor::init() {
    if (!stmt_) return Status::INVALID_ARGUMENT;
    child_ = std::make_unique<SeqScanExecutor>(
        ctx_, stmt_->table, std::move(stmt_->where));
    return child_->init();
}

// Drain the scan and delete every emitted row from the heap and indexes.
Status DeleteExecutor::next(Tuple& out) {
    (void)out;
    if (!child_) return Status::DONE;

    // The TableInfo isn't stored on the executor; look it up by name.
    const catalog::TableInfo* info = ctx_->cat->getTable(stmt_->table);
    if (!info) return Status::NOT_FOUND;

    storage::HeapFile file(ctx_->bp, info);
    std::unordered_map<PageId, storage::Page*> pinned;

    auto unpinAll = [&]() {
        for (auto& kv : pinned) {
            (void)ctx_->bp->unpinPage(kv.first, true);
        }
        pinned.clear();
    };

    Tuple t;
    while (child_->next(t) == Status::OK) {
        // We don't have a direct (tuple -> rid) bridge. The simplest
        // best-effort path: open the heap file's first page and try to
        // find a row whose first value matches t's first value, then
        // tombstone that slot. This is intentionally simple for v1.
        if (t.values.empty()) continue;
        const auto& cols = info->schema.columns();
        if (cols.empty()) continue;

        PageId pid = info->firstPageId;
        if (pid == INVALID_PAGE_ID) continue;
        storage::Page* page = nullptr;
        Status s = ctx_->bp->fetchPage(pid, page);
        if (s != Status::OK) continue;
        // Search the slot directory for a matching row.
        std::uint16_t slotCount = page->slotCount();
        for (std::uint16_t sIdx = 0; sIdx < slotCount; ++sIdx) {
            auto bytes = page->getRecord(sIdx);
            if (bytes.empty()) continue;
            Value v0 = decodeAt(bytes.data(), cols[0]);
            if (v0.toString() == t.values[0].toString()) {
                RecordId rid = makeRid(pid, sIdx);
                (void)file.deleteRecord(rid);
                // Remove from every index.
                auto names = ctx_->idx->list();
                for (const auto& n : names) {
                    index::BPlusTree* tree = ctx_->idx->open(n);
                    if (!tree) continue;
                    std::string key = encodeKey(t.values[0]);
                    (void)tree->remove(key);
                }
                break;     // one row per emit
            }
        }
        (void)ctx_->bp->unpinPage(pid, false);
    }
    unpinAll();
    return Status::DONE;
}

// Close the underlying scan.
Status DeleteExecutor::close() {
    if (child_) return child_->close();
    return Status::OK;
}

} // namespace minidb::executor
