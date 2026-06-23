// =============================================================================
// include/parser/ast.h
// -----------------------------------------------------------------------------
// AST node types produced by Parser::parse. The planner consumes these
// values; nothing else in the system sees raw SQL.
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog/schema.h"

namespace minidb::parser {

enum class StmtKind {
    SELECT, INSERT, DELETE, CREATE, DROP, TXN
};

// ----- Expressions -----
enum class ExprKind {
    COLUMN,            // "id"  -> qualified? column_ref
    INT_LIT, FLOAT_LIT, STR_LIT, BOOL_LIT, NULL_LIT,
    BINARY_OP,         // +, -, *, /, =, !=, <, <=, >, >=, AND, OR
    UNARY_OP,          // NOT, IS NULL, IS NOT NULL
    FUNCTION_CALL,     // COUNT(*), SUM(x), MIN, MAX, AVG
};

struct Expr {
    ExprKind kind;
    std::string text;                        // for COLUMN/function name
    std::string op;                          // for BINARY_OP / UNARY_OP
    std::vector<std::unique_ptr<Expr>> args; // for BINARY_OP, FUNCTION_CALL
    int64_t  intVal  = 0;
    double   floatVal = 0.0;
    bool     boolVal = false;
    std::string strVal;
    int  line = 0, col = 0;                  // source location
};

// ----- Statements -----
struct SelectStmt {
    bool        distinct = false;
    std::vector<std::unique_ptr<Expr>> projection;   // empty = "*"
    std::string fromTable;
    std::string joinTable;                            // empty = no join
    std::unique_ptr<Expr> joinOn;                     // nullptr = no join
    std::unique_ptr<Expr> where;
    std::vector<std::unique_ptr<Expr>> groupBy;
    std::vector<std::unique_ptr<Expr>> orderBy;
    bool orderDesc = false;
    int  limit = -1;
};

struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;                 // empty = all
    std::vector<std::vector<std::unique_ptr<Expr>>> rows;
};

struct DeleteStmt {
    std::string table;
    std::unique_ptr<Expr> where;
};

struct CreateStmt {
    std::string table;
    std::vector<catalog::Column> columns;
    bool ifNotExists = false;
};

struct DropStmt {
    std::string table;
    bool ifExists = false;
};

struct TxnStmt {
    enum class Op { BEGIN, COMMIT, ROLLBACK };
    Op op;
};

struct Stmt {
    StmtKind kind = StmtKind::SELECT;
    std::unique_ptr<SelectStmt> select;
    std::unique_ptr<InsertStmt> insert;
    std::unique_ptr<DeleteStmt> del;
    std::unique_ptr<CreateStmt> create;
    std::unique_ptr<DropStmt>   drop;
    std::unique_ptr<TxnStmt>    txn;

    int line = 0, col = 0;                            // source location
};

// Pretty-printer for error messages and tests.
std::string toString(const Expr& e);
std::string toString(const Stmt& s);

} // namespace minidb::parser