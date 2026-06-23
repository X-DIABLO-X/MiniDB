// =============================================================================
// src/executor/seq_scan.cpp
// -----------------------------------------------------------------------------
// SeqScanExecutor: walks the heap file row by row, applies a predicate
// (if any), and materialises a Tuple per surviving row.
//
// Row encoding follows the Schema's column layout: for every column we
// reserve `length` bytes (4 for INT/FLOAT/BOOL, declared for VARCHAR),
// producing a fixed-size row image that fits in a single slot.
// =============================================================================
#include "executor/seq_scan.h"

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
#include "parser/ast.h"
#include "storage/heap_file.h"

namespace minidb::executor {

namespace {

// Return the byte size of one column's slot inside a row image.
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

// Evaluate a literal expression to a Value (used by predicates).
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

// Resolve a value by column name within the current row. Returns NULL
// when the column is missing.
Value resolveColumn(const Tuple& t, const catalog::Schema& schema, const std::string& name) {
    for (std::size_t i = 0; i < schema.numColumns(); ++i) {
        if (schema.column(i).name == name && i < t.values.size()) {
            return t.values[i];
        }
    }
    return Value::makeNull();
}

// Truthiness used by WHERE. NULL is "false" (SQL three-valued logic v1
// simplification: only the FALSE case rejects a row).
bool isTrue(const Value& v) {
    switch (v.tag) {
        case Value::Tag::NULL_:  return false;
        case Value::Tag::BOOL:   return v.b;
        case Value::Tag::INT:    return v.i != 0;
        case Value::Tag::FLOAT:  return v.f != 0.0f;
        case Value::Tag::STRING: return !v.s.empty();
    }
    return false;
}

// Three-way compare helper, NULL sorts as "less than everything" for
// ordering, but for equality we treat NULL == NULL as false (v1).
int compareValues(const Value& a, const Value& b) {
    if (a.tag == Value::Tag::NULL_ || b.tag == Value::Tag::NULL_) return 0;
    if (a.tag == Value::Tag::INT && b.tag == Value::Tag::INT) {
        return (a.i < b.i) ? -1 : (a.i > b.i ? 1 : 0);
    }
    if (a.tag == Value::Tag::FLOAT && b.tag == Value::Tag::FLOAT) {
        return (a.f < b.f) ? -1 : (a.f > b.f ? 1 : 0);
    }
    if (a.tag == Value::Tag::INT && b.tag == Value::Tag::FLOAT) {
        float av = static_cast<float>(a.i);
        return (av < b.f) ? -1 : (av > b.f ? 1 : 0);
    }
    if (a.tag == Value::Tag::FLOAT && b.tag == Value::Tag::INT) {
        float bv = static_cast<float>(b.i);
        return (a.f < bv) ? -1 : (a.f > bv ? 1 : 0);
    }
    if (a.tag == Value::Tag::STRING && b.tag == Value::Tag::STRING) {
        if (a.s == b.s) return 0;
        return a.s < b.s ? -1 : 1;
    }
    if (a.tag == Value::Tag::BOOL && b.tag == Value::Tag::BOOL) {
        return (a.b == b.b) ? 0 : (a.b ? 1 : -1);
    }
    return 0;
}

// Evaluate a predicate over a single-row tuple. Supported operators:
// comparisons (=, !=, <, <=, >, >=), AND, OR, NOT, IS NULL, IS NOT NULL.
// Literals and COLUMN refs. Unsupported shapes degrade to "true" so
// non-filtered plans still work.
bool evalPredicate(const parser::Expr& e, const Tuple& t, const catalog::Schema& schema) {
    switch (e.kind) {
        case parser::ExprKind::BOOL_LIT:  return e.boolVal;
        case parser::ExprKind::NULL_LIT:  return false;
        case parser::ExprKind::INT_LIT:   return e.intVal != 0;
        case parser::ExprKind::FLOAT_LIT: return e.floatVal != 0.0;
        case parser::ExprKind::STR_LIT:   return !e.strVal.empty();

        case parser::ExprKind::COLUMN: {
            Value v = resolveColumn(t, schema, e.text);
            return isTrue(v);
        }

        case parser::ExprKind::UNARY_OP: {
            if (e.op == "IS NULL" || e.op == "IS NOT NULL") {
                Value v = e.args.empty() ? Value::makeNull()
                                         : resolveColumn(t, schema, e.args[0]->text);
                bool isNull = (v.tag == Value::Tag::NULL_);
                return e.op == "IS NULL" ? isNull : !isNull;
            }
            if (e.op == "NOT") {
                if (!e.args.empty()) {
                    return !evalPredicate(*e.args[0], t, schema);
                }
                return false;
            }
            return true;
        }

        case parser::ExprKind::BINARY_OP: {
            if (e.op == "AND") {
                if (e.args.size() < 2) return true;
                return evalPredicate(*e.args[0], t, schema) &&
                       evalPredicate(*e.args[1], t, schema);
            }
            if (e.op == "OR") {
                if (e.args.size() < 2) return true;
                return evalPredicate(*e.args[0], t, schema) ||
                       evalPredicate(*e.args[1], t, schema);
            }
            if (e.args.size() < 2) return true;
            Value lv = (e.args[0]->kind == parser::ExprKind::COLUMN)
                ? resolveColumn(t, schema, e.args[0]->text)
                : evalLiteral(*e.args[0]);
            Value rv = (e.args[1]->kind == parser::ExprKind::COLUMN)
                ? resolveColumn(t, schema, e.args[1]->text)
                : evalLiteral(*e.args[1]);
            int cmp = compareValues(lv, rv);
            if (e.op == "=")  return cmp == 0;
            if (e.op == "!=") return cmp != 0;
            if (e.op == "<")  return cmp <  0;
            if (e.op == "<=") return cmp <= 0;
            if (e.op == ">")  return cmp >  0;
            if (e.op == ">=") return cmp >= 0;
            return true;
        }

        default: return true;
    }
}

// Decode the raw bytes for a single column into a Value.
Value decodeColumn(const catalog::Column& c, const std::uint8_t* base) {
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
            std::string out;
            if (n > 0) out.assign(reinterpret_cast<const char*>(base), n);
            // Trim trailing NULs so VARCHAR columns don't display padding.
            while (!out.empty() && out.back() == '\0') out.pop_back();
            return Value::makeStr(out);
        }
    }
    return Value::makeNull();
}

} // namespace

// ----- SeqScanExecutor -----

SeqScanExecutor::SeqScanExecutor(ExecutorContext* ctx,
                                 std::string table,
                                 std::unique_ptr<parser::Expr> predicate)
    : Executor(ctx), table_(std::move(table)),
      predicate_(std::move(predicate)) {}

// Default the destructor explicitly so the unique_ptr members get the
// generated destructor in this translation unit.
SeqScanExecutor::~SeqScanExecutor() = default;

// Open the table, build a HeapFile, and start a forward scan iterator.
Status SeqScanExecutor::init() {
    info_ = ctx_->cat->getTable(table_);
    if (!info_) return Status::NOT_FOUND;
    file_ = std::make_unique<storage::HeapFile>(ctx_->bp, info_);
    it_   = file_->scan();
    return Status::OK;
}

// Pull the next surviving (predicate-passing) row, decoding it into out.
Status SeqScanExecutor::next(Tuple& out) {
    if (!it_) return Status::DONE;
    RecordId rid;
    std::span<const std::uint8_t> bytes;
    while (it_->next(rid, bytes)) {
        out.values.clear();
        const auto& cols = info_->schema.columns();
        std::size_t off = 0;
        for (const auto& col : cols) {
            std::size_t n = colBytes(col);
            Value v = decodeColumn(col, bytes.data() + off);
            out.values.push_back(v);
            off += n;
        }
        if (!predicate_) return Status::OK;
        if (evalPredicate(*predicate_, out, info_->schema)) return Status::OK;
        // Predicate failed — keep scanning.
    }
    return Status::DONE;
}

// Close the underlying iterator.
Status SeqScanExecutor::close() {
    if (it_) it_->close();
    return Status::OK;
}

} // namespace minidb::executor
