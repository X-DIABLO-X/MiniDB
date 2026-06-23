// =============================================================================
// src/parser/ast.cpp
// -----------------------------------------------------------------------------
// Pretty-printer for AST nodes. Used in error messages and tests.
// =============================================================================
#include "parser/ast.h"

#include <sstream>

namespace minidb::parser {

// Render an expression back to SQL-like text. Handles every ExprKind
// defined in ast.h, including nested binary/unary/function expressions.
std::string toString(const Expr& e) {
    std::ostringstream os;
    switch (e.kind) {
        case ExprKind::COLUMN:
            os << e.text;
            break;
        case ExprKind::INT_LIT:
            os << e.intVal;
            break;
        case ExprKind::FLOAT_LIT:
            os << e.floatVal;
            break;
        case ExprKind::STR_LIT:
            os << "'" << e.strVal << "'";
            break;
        case ExprKind::BOOL_LIT:
            os << (e.boolVal ? "TRUE" : "FALSE");
            break;
        case ExprKind::NULL_LIT:
            os << "NULL";
            break;
        case ExprKind::BINARY_OP:
            // Need at least two operands; if args is short, just show what we have.
            if (e.args.size() >= 2) {
                os << "(" << toString(*e.args[0]) << " " << e.op
                   << " " << toString(*e.args[1]) << ")";
            } else {
                os << e.op;
            }
            break;
        case ExprKind::UNARY_OP:
            if (!e.args.empty()) {
                os << "(" << e.op << " " << toString(*e.args[0]) << ")";
            } else {
                os << e.op;
            }
            break;
        case ExprKind::FUNCTION_CALL:
            os << e.text << "(";
            for (std::size_t i = 0; i < e.args.size(); ++i) {
                if (i) os << ", ";
                if (e.args[i]) {
                    os << toString(*e.args[i]);
                } else {
                    os << "*";
                }
            }
            os << ")";
            break;
    }
    return os.str();
}

// Render a statement briefly. Each variant pulls the relevant fields out of
// the tag-discriminated union.
std::string toString(const Stmt& s) {
    std::ostringstream os;
    switch (s.kind) {
        case StmtKind::SELECT:
            if (s.select) {
                os << "SELECT";
                if (s.select->distinct) os << " DISTINCT";
                os << " ";
                if (s.select->projection.empty()) {
                    os << "*";
                } else {
                    for (std::size_t i = 0; i < s.select->projection.size(); ++i) {
                        if (i) os << ", ";
                        if (s.select->projection[i]) {
                            os << toString(*s.select->projection[i]);
                        }
                    }
                }
                os << " FROM " << s.select->fromTable;
                if (!s.select->joinTable.empty()) {
                    os << " JOIN " << s.select->joinTable << " ON ";
                    if (s.select->joinOn) os << toString(*s.select->joinOn);
                }
                if (s.select->where) os << " WHERE " << toString(*s.select->where);
                if (!s.select->groupBy.empty()) {
                    os << " GROUP BY ";
                    for (std::size_t i = 0; i < s.select->groupBy.size(); ++i) {
                        if (i) os << ", ";
                        if (s.select->groupBy[i]) os << toString(*s.select->groupBy[i]);
                    }
                }
                if (!s.select->orderBy.empty()) {
                    os << " ORDER BY ";
                    for (std::size_t i = 0; i < s.select->orderBy.size(); ++i) {
                        if (i) os << ", ";
                        if (s.select->orderBy[i]) os << toString(*s.select->orderBy[i]);
                    }
                    os << (s.select->orderDesc ? " DESC" : " ASC");
                }
                if (s.select->limit >= 0) os << " LIMIT " << s.select->limit;
            } else {
                os << "SELECT <null>";
            }
            break;
        case StmtKind::INSERT:
            if (s.insert) {
                os << "INSERT INTO " << s.insert->table;
                if (!s.insert->columns.empty()) {
                    os << " (";
                    for (std::size_t i = 0; i < s.insert->columns.size(); ++i) {
                        if (i) os << ", ";
                        os << s.insert->columns[i];
                    }
                    os << ")";
                }
                os << " VALUES (...)";
            } else {
                os << "INSERT <null>";
            }
            break;
        case StmtKind::DELETE:
            if (s.del) {
                os << "DELETE FROM " << s.del->table;
                if (s.del->where) os << " WHERE " << toString(*s.del->where);
            } else {
                os << "DELETE <null>";
            }
            break;
        case StmtKind::CREATE:
            if (s.create) {
                os << "CREATE TABLE";
                if (s.create->ifNotExists) os << " IF NOT EXISTS";
                os << " " << s.create->table << " (";
                for (std::size_t i = 0; i < s.create->columns.size(); ++i) {
                    if (i) os << ", ";
                    const auto& c = s.create->columns[i];
                    os << c.name << " ";
                    switch (c.type) {
                        case catalog::Type::INT:     os << "INT"; break;
                        case catalog::Type::FLOAT:   os << "FLOAT"; break;
                        case catalog::Type::VARCHAR: os << "VARCHAR(" << c.length << ")"; break;
                        case catalog::Type::BOOL:    os << "BOOL"; break;
                    }
                    if (!c.nullable) os << " NOT NULL";
                    if (c.isPrimaryKey) os << " PRIMARY KEY";
                }
                os << ")";
            } else {
                os << "CREATE <null>";
            }
            break;
        case StmtKind::DROP:
            if (s.drop) {
                os << "DROP TABLE";
                if (s.drop->ifExists) os << " IF EXISTS";
                os << " " << s.drop->table;
            } else {
                os << "DROP <null>";
            }
            break;
        case StmtKind::TXN:
            if (s.txn) {
                switch (s.txn->op) {
                    case TxnStmt::Op::BEGIN:    os << "BEGIN"; break;
                    case TxnStmt::Op::COMMIT:   os << "COMMIT"; break;
                    case TxnStmt::Op::ROLLBACK: os << "ROLLBACK"; break;
                }
            } else {
                os << "TXN <null>";
            }
            break;
    }
    return os.str();
}

} // namespace minidb::parser
