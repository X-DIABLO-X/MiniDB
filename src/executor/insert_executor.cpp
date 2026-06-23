// =============================================================================
// src/executor/insert_executor.cpp
// -----------------------------------------------------------------------------
// InsertExecutor: serialises a row from the parsed InsertStmt according to
// the table's schema, calls HeapFile::insertRecord, and updates every
// index that covers one of the table's columns with the resulting
// RecordId. One call to next() inserts exactly one row; subsequent calls
// insert the next row from the parsed rows[] list until all are done.
// =============================================================================
#include "executor/insert_executor.h"

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
#include "storage/heap_file.h"

namespace minidb::executor {

namespace {

// Column footprint inside a row image (4 bytes for fixed types, declared
// length for VARCHAR, 0 means the row image is empty for that column).
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

// Evaluate a literal expression to a Value.
Value evalLiteral(const parser::Expr& e) {
    switch (e.kind) {
        case parser::ExprKind::INT_LIT:   return Value::makeInt(static_cast<int32_t>(e.intVal));
        case parser::ExprKind::FLOAT_LIT: return Value::makeFloat(static_cast<float>(e.floatVal));
        case parser::ExprKind::STR_LIT:   return Value::makeStr(e.strVal);
        case parser::ExprKind::BOOL_LIT:  return Value::makeBool(e.boolVal);
        case parser::ExprKind::NULL_LIT:  return Value::makeNull();
        default:                          return Value::makeNull();
    }
}

// Serialize a Value into a fixed-size field image, left-padded / right-
// truncated as needed so the row always fits the schema footprint.
void encodeValue(const Value& v, const catalog::Column& c,
                 std::vector<std::uint8_t>& out) {
    std::size_t n = colBytes(c);
    std::size_t start = out.size();
    out.resize(start + n, 0);
    switch (c.type) {
        case catalog::Type::INT: {
            int32_t x = (v.tag == Value::Tag::INT) ? v.i
                       : (v.tag == Value::Tag::FLOAT) ? static_cast<int32_t>(v.f) : 0;
            std::memcpy(out.data() + start, &x, sizeof(x));
            break;
        }
        case catalog::Type::FLOAT: {
            float x = (v.tag == Value::Tag::FLOAT) ? v.f
                     : (v.tag == Value::Tag::INT) ? static_cast<float>(v.i) : 0.0f;
            std::memcpy(out.data() + start, &x, sizeof(x));
            break;
        }
        case catalog::Type::BOOL: {
            int32_t x = (v.tag == Value::Tag::BOOL && v.b) ? 1 : 0;
            std::memcpy(out.data() + start, &x, sizeof(x));
            break;
        }
        case catalog::Type::VARCHAR: {
            const std::string& s = (v.tag == Value::Tag::STRING) ? v.s : std::string();
            std::size_t copy = s.size() < n ? s.size() : n;
            if (copy > 0) std::memcpy(out.data() + start, s.data(), copy);
            // Remaining bytes are already zero.
            break;
        }
    }
}

// Encode a B+ tree key from a Value (mirrors index_scan.cpp's encoding).
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

} // namespace

// ----- InsertExecutor -----

InsertExecutor::InsertExecutor(ExecutorContext* ctx,
                               std::unique_ptr<parser::InsertStmt> stmt)
    : Executor(ctx), stmt_(std::move(stmt)) {}

// Out-of-line destructor for the unique_ptr members.
InsertExecutor::~InsertExecutor() = default;

// Open the heap file for the target table.
Status InsertExecutor::init() {
    info_ = ctx_->cat->getTable(stmt_->table);
    if (!info_) return Status::NOT_FOUND;
    file_ = std::make_unique<storage::HeapFile>(ctx_->bp, info_);
    rowIdx_ = 0;
    return Status::OK;
}

// Insert the next parsed row. Returns DONE when all rows have been
// inserted, OK otherwise (one row per successful call).
Status InsertExecutor::next(Tuple& out) {
    (void)out;
    if (!info_ || !file_) return Status::DONE;
    if (!stmt_ || rowIdx_ >= stmt_->rows.size()) return Status::DONE;

    const auto& row = stmt_->rows[rowIdx_];
    const auto& cols = info_->schema.columns();

    // Build a (column name -> source expression) map. When the user
    // supplied a column list, we only fill those slots; otherwise we
    // assume the row values are in schema order.
    std::vector<std::unique_ptr<parser::Expr>> byName(cols.size());
    if (stmt_->columns.empty()) {
        for (std::size_t i = 0; i < cols.size() && i < row.size(); ++i) {
            byName[i] = (i < row.size()) ? std::move(const_cast<std::unique_ptr<parser::Expr>&>(row[i]))
                                         : nullptr;
        }
    } else {
        for (std::size_t ci = 0; ci < cols.size(); ++ci) {
            for (std::size_t k = 0; k < stmt_->columns.size(); ++k) {
                if (stmt_->columns[k] == cols[ci].name && k < row.size()) {
                    byName[ci] = std::move(const_cast<std::unique_ptr<parser::Expr>&>(row[k]));
                    break;
                }
            }
        }
    }

    // Serialise the row.
    std::vector<std::uint8_t> bytes;
    for (std::size_t ci = 0; ci < cols.size(); ++ci) {
        if (!byName[ci]) {
            // NULL: just leave the field zeroed.
            std::size_t n = colBytes(cols[ci]);
            bytes.resize(bytes.size() + n, 0);
            continue;
        }
        Value v = evalLiteral(*byName[ci]);
        encodeValue(v, cols[ci], bytes);
    }

    // Insert into the heap file.
    RecordId rid = INVALID_RID;
    Status s = file_->insertRecord(
        std::span<const std::uint8_t>(bytes.data(), bytes.size()), rid);
    if (s != Status::OK) return s;
    if (rid == INVALID_RID) return Status::IO_ERROR;

    // Update every index that covers one of the columns. We rebuild the
    // list of indexes from the index manager (any name that mentions this
    // table gets updated — fine for v1; the catalog could grow a
    // (table,column) -> name mapping later).
    auto indexNames = ctx_->idx->list();
    for (const auto& name : indexNames) {
        index::BPlusTree* tree = ctx_->idx->open(name);
        if (!tree) continue;
        // We use the first column of the table as the key for any index
        // that points to this table. v1 simplification: the column-to-
        // index mapping isn't threaded through here, so we fall back to
        // the first column's value.
        if (cols.empty()) continue;
        Value keyVal;
        std::size_t base = 0;
        for (std::size_t ci = 0; ci + 1 < cols.size(); ++ci) {
            base += colBytes(cols[ci]);
        }
        // Re-derive the first value from the serialised image.
        const catalog::Column& c0 = cols[0];
        std::size_t n0 = colBytes(c0);
        switch (c0.type) {
            case catalog::Type::INT: {
                int32_t x = 0;
                if (n0 >= sizeof(int32_t)) std::memcpy(&x, bytes.data() + base, sizeof(x));
                keyVal = Value::makeInt(x);
                break;
            }
            case catalog::Type::FLOAT: {
                float x = 0.0f;
                if (n0 >= sizeof(float)) std::memcpy(&x, bytes.data() + base, sizeof(x));
                keyVal = Value::makeFloat(x);
                break;
            }
            case catalog::Type::BOOL: {
                int32_t x = 0;
                if (n0 >= sizeof(int32_t)) std::memcpy(&x, bytes.data() + base, sizeof(x));
                keyVal = Value::makeBool(x != 0);
                break;
            }
            case catalog::Type::VARCHAR: {
                std::string s2;
                if (n0 > 0) s2.assign(reinterpret_cast<const char*>(bytes.data() + base), n0);
                while (!s2.empty() && s2.back() == '\0') s2.pop_back();
                keyVal = Value::makeStr(s2);
                break;
            }
        }
        std::string key = encodeKey(keyVal);
        (void)tree->insert(key, rid);
    }

    ++rowIdx_;
    return Status::OK;
}

// Nothing buffered; the heap file owns its own resources.
Status InsertExecutor::close() {
    return Status::OK;
}

} // namespace minidb::executor
