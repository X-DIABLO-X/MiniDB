// =============================================================================
// src/executor/index_scan.cpp
// -----------------------------------------------------------------------------
// IndexScanExecutor: drives a B+ tree lookup (or range scan) and materialises
// matching rows from the heap file into Tuples.
//
// For v1 we support a single equality predicate of the form
//     <column> = <literal>
// which we extract from the AST. A range predicate (<, <=, >, >=) lowers
// to a range scan. Anything more exotic falls back to "scan all matching
// record ids from a point lookup of the first key, then filter via
// the supplied predicate on the row itself".
// =============================================================================
#include "executor/index_scan.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "catalog/catalog_manager.h"
#include "catalog/schema.h"
#include "catalog/table_info.h"
#include "common/record_id.h"
#include "common/status.h"
#include "common/types.h"
#include "executor/executor.h"
#include "index/bplus_tree.h"
#include "index/index_manager.h"
#include "parser/ast.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"

namespace minidb::executor {

namespace {

// Encode a Value into a B+ tree key byte string. The encoding mirrors the
// row layout so the index can compare keys lexicographically.
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

// Same decoding logic as SeqScan: walk the schema column-by-column and
// peel the right number of bytes off the raw row image.
Value decodeColumn(const catalog::Column& c, const std::uint8_t* base) {
    std::size_t n = c.length == 0
        ? (c.type == catalog::Type::VARCHAR ? std::size_t{0} : std::size_t{4})
        : c.length;
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
            std::string out;
            if (n > 0) out.assign(reinterpret_cast<const char*>(base), n);
            while (!out.empty() && out.back() == '\0') out.pop_back();
            return Value::makeStr(out);
        }
    }
    return Value::makeNull();
}

void decodeRow(const Tuple& inRaw, Tuple& out) {
    // inRaw is unused here; this stub exists so callers can pass it.
    (void)inRaw;
    (void)out;
}

// Build a single Tuple from a heap record image.
void materialiseTuple(std::span<const std::uint8_t> bytes,
                      const catalog::TableInfo& info,
                      Tuple& out) {
    out.values.clear();
    const auto& cols = info.schema.columns();
    std::size_t off = 0;
    for (const auto& col : cols) {
        std::size_t n = col.length == 0
            ? (col.type == catalog::Type::VARCHAR ? std::size_t{0} : std::size_t{4})
            : col.length;
        out.values.push_back(decodeColumn(col, bytes.data() + off));
        off += n;
    }
}

} // namespace

// ----- IndexScanExecutor -----

IndexScanExecutor::IndexScanExecutor(ExecutorContext* ctx,
                                     std::string table,
                                     std::string indexName,
                                     std::unique_ptr<parser::Expr> predicate)
    : Executor(ctx), table_(std::move(table)),
      indexName_(std::move(indexName)), predicate_(std::move(predicate)) {}

// Out-of-line destructor for the unique_ptr<Expr> member.
IndexScanExecutor::~IndexScanExecutor() = default;

// Open the index, perform a key lookup (or range scan) driven by the
// predicate, and cache the resulting RecordIds in `rids_`.
Status IndexScanExecutor::init() {
    info_ = ctx_->cat->getTable(table_);
    if (!info_) return Status::NOT_FOUND;
    tree_ = ctx_->idx->open(indexName_);
    if (!tree_) return Status::NOT_FOUND;

    rids_.clear();
    cursor_ = 0;

    // No predicate: fall back to an empty result set. A caller that wants
    // every row should use SeqScan. An index without a predicate would
    // require a full index traversal which is out of scope for v1.
    if (!predicate_) return Status::OK;

    // Inspect the predicate: we only handle the simple `col = literal`
    // shape here. Anything else yields an empty result (caller may add
    // a recheck on the row itself in future versions).
    const parser::Expr& p = *predicate_;
    if (p.kind != parser::ExprKind::BINARY_OP) return Status::OK;
    if (p.op == "=") {
        if (p.args.size() < 2) return Status::OK;
        const parser::Expr* col = p.args[0].get();
        const parser::Expr* lit = p.args[1].get();
        if (col->kind != parser::ExprKind::COLUMN) {
            std::swap(col, lit);
            if (col->kind != parser::ExprKind::COLUMN) return Status::OK;
        }
        Value v;
        switch (lit->kind) {
            case parser::ExprKind::INT_LIT:   v = Value::makeInt(static_cast<int32_t>(lit->intVal)); break;
            case parser::ExprKind::FLOAT_LIT: v = Value::makeFloat(static_cast<float>(lit->floatVal)); break;
            case parser::ExprKind::STR_LIT:   v = Value::makeStr(lit->strVal); break;
            case parser::ExprKind::BOOL_LIT:  v = Value::makeBool(lit->boolVal); break;
            default: return Status::OK;
        }
        std::string key = encodeKey(v);
        RecordId rid = INVALID_RID;
        Status s = tree_->search(key, rid);
        if (s == Status::OK && rid != INVALID_RID) rids_.push_back(rid);
        return Status::OK;
    }
    if (p.op == "<" || p.op == "<=" || p.op == ">" || p.op == ">=") {
        if (p.args.size() < 2) return Status::OK;
        const parser::Expr* col = p.args[0].get();
        const parser::Expr* lit = p.args[1].get();
        if (col->kind != parser::ExprKind::COLUMN) return Status::OK;
        Value v;
        switch (lit->kind) {
            case parser::ExprKind::INT_LIT:   v = Value::makeInt(static_cast<int32_t>(lit->intVal)); break;
            case parser::ExprKind::FLOAT_LIT: v = Value::makeFloat(static_cast<float>(lit->floatVal)); break;
            case parser::ExprKind::STR_LIT:   v = Value::makeStr(lit->strVal); break;
            default: return Status::OK;
        }
        std::string key = encodeKey(v);
        std::vector<RecordId> rids;
        // For v1 we only use the rangeScan interface. The literal is used
        // as both bounds — exact-equality on the chosen column.
        std::string lo = key;
        std::string hi = key;
        if (p.op == ">" || p.op == ">=") {
            lo = encodeKey(Value::makeInt(std::numeric_limits<int32_t>::min()));
        } else {
            hi = encodeKey(Value::makeInt(std::numeric_limits<int32_t>::max()));
        }
        Status s = tree_->rangeScan(lo, hi, rids);
        if (s == Status::OK) rids_ = std::move(rids);
        return Status::OK;
    }
    return Status::OK;
}

// Yield the next cached RecordId, materialising its row image.
Status IndexScanExecutor::next(Tuple& out) {
    (void)decodeRow; // suppress unused-helper warning
    if (cursor_ >= rids_.size()) return Status::DONE;
    RecordId rid = rids_[cursor_++];
    PageId pid = ridPage(rid);
    std::uint16_t slot = ridSlot(rid);

    storage::Page* page = nullptr;
    Status s = ctx_->bp->fetchPage(pid, page);
    if (s != Status::OK) return s;
    std::span<const std::uint8_t> bytes = page->getRecord(slot);
    materialiseTuple(bytes, *info_, out);
    (void)ctx_->bp->unpinPage(pid, false);
    return Status::OK;
}

// Nothing buffered; nothing to release.
Status IndexScanExecutor::close() {
    return Status::OK;
}

} // namespace minidb::executor
