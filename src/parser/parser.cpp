// =============================================================================
// src/parser/parser.cpp
// -----------------------------------------------------------------------------
// Recursive-descent parser. Schema-agnostic — it only checks SQL syntax.
// Grammar follows the v1 dialect described in include/parser/README.md.
// =============================================================================
#include "parser/parser.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace minidb::parser {

// -----------------------------------------------------------------------------
// Construction / low-level helpers
// -----------------------------------------------------------------------------

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}
Parser::~Parser() = default;

// Look at the current token without consuming it. Caller must guarantee
// that pos_ is within range (tokenize always appends END_OF_INPUT).
const Token& Parser::peek() const { return tokens_[pos_]; }

// Consume and return the current token.
const Token& Parser::advance() { return tokens_[pos_++]; }

// If the current token matches k, advance and return true. Otherwise false.
bool Parser::match(TokenKind k) {
    if (peek().kind == k) { ++pos_; return true; }
    return false;
}

// If the current token matches k, advance and return true. Otherwise record
// an error message naming `what` was expected, and return false.
bool Parser::consume(TokenKind k, const char* what) {
    if (match(k)) return true;
    err_ = std::string("expected ") + what;
    return false;
}

// -----------------------------------------------------------------------------
// Entry points
// -----------------------------------------------------------------------------

// Parse a single statement. If parsing fails, lastError() explains why and
// the returned Stmt is the default-constructed shape.
Stmt Parser::parse() {
    err_.clear();
    auto s = parseStatement();
    if (!s) {
        Stmt empty;
        return empty;
    }
    Stmt out = std::move(*s);
    out.line = peek().loc.line;
    out.col  = peek().loc.col;
    return out;
}

// Parse a ;-separated script. Stops at the first parse error or at
// END_OF_INPUT.
std::vector<Stmt> Parser::parseScript() {
    std::vector<Stmt> out;
    while (peek().kind != TokenKind::END_OF_INPUT) {
        auto s = parseStatement();
        if (!s) break;
        out.push_back(std::move(*s));
        // Optional trailing semicolon between statements.
        match(TokenKind::SEMICOLON);
    }
    return out;
}

// -----------------------------------------------------------------------------
// Statement dispatch
// -----------------------------------------------------------------------------

// Dispatch to the right parseX based on the leading keyword.
std::unique_ptr<Stmt> Parser::parseStatement() {
    switch (peek().kind) {
        case TokenKind::KW_SELECT: {
            auto sel = parseSelect();
            if (!sel) return nullptr;
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::SELECT;
            s->select = std::move(sel);
            return s;
        }
        case TokenKind::KW_INSERT: {
            auto ins = parseInsert();
            if (!ins) return nullptr;
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::INSERT;
            s->insert = std::move(ins);
            return s;
        }
        case TokenKind::KW_DELETE: {
            auto d = parseDelete();
            if (!d) return nullptr;
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::DELETE;
            s->del = std::move(d);
            return s;
        }
        case TokenKind::KW_CREATE: {
            auto c = parseCreate();
            if (!c) return nullptr;
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::CREATE;
            s->create = std::move(c);
            return s;
        }
        case TokenKind::KW_DROP: {
            auto d = parseDrop();
            if (!d) return nullptr;
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::DROP;
            s->drop = std::move(d);
            return s;
        }
        case TokenKind::KW_BEGIN:
        case TokenKind::KW_COMMIT:
        case TokenKind::KW_ROLLBACK: {
            auto t = parseTxn();
            if (!t) return nullptr;
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::TXN;
            s->txn = std::move(t);
            return s;
        }
        default:
            err_ = "unsupported or unknown statement (expected SELECT, INSERT, "
                   "DELETE, CREATE, DROP, BEGIN, COMMIT, or ROLLBACK)";
            return nullptr;
    }
}

// -----------------------------------------------------------------------------
// SELECT
// -----------------------------------------------------------------------------

// SELECT [DISTINCT] <cols|*> FROM <t> [JOIN <t2> ON <expr>] [WHERE <expr>]
//   [GROUP BY <exprs>] [ORDER BY <exprs> [ASC|DESC]] [LIMIT n]
std::unique_ptr<SelectStmt> Parser::parseSelect() {
    auto out = std::make_unique<SelectStmt>();

    if (!consume(TokenKind::KW_SELECT, "SELECT")) return nullptr;

    // Optional DISTINCT
    out->distinct = match(TokenKind::KW_DISTINCT);

    // Projection list. "*" is represented as an empty vector.
    if (match(TokenKind::STAR)) {
        // wildcard — leave projection empty
    } else {
        for (;;) {
            auto e = parseExpr();
            if (!e) return nullptr;
            out->projection.push_back(std::move(e));
            if (!match(TokenKind::COMMA)) break;
        }
    }

    if (!consume(TokenKind::KW_FROM, "FROM")) return nullptr;
    if (peek().kind != TokenKind::IDENT) {
        err_ = "expected table name after FROM";
        return nullptr;
    }
    out->fromTable = advance().text;

    // Optional JOIN <t2> ON <expr>
    if (match(TokenKind::KW_JOIN)) {
        if (peek().kind != TokenKind::IDENT) {
            err_ = "expected table name after JOIN";
            return nullptr;
        }
        out->joinTable = advance().text;
        if (!consume(TokenKind::KW_ON, "ON")) return nullptr;
        out->joinOn = parseExpr();
        if (!out->joinOn) return nullptr;
    }

    // Optional WHERE
    if (match(TokenKind::KW_WHERE)) {
        out->where = parseExpr();
        if (!out->where) return nullptr;
    }

    // Optional GROUP BY
    if (match(TokenKind::KW_GROUP)) {
        if (!consume(TokenKind::KW_BY, "BY")) return nullptr;
        for (;;) {
            auto e = parseExpr();
            if (!e) return nullptr;
            out->groupBy.push_back(std::move(e));
            if (!match(TokenKind::COMMA)) break;
        }
    }

    // Optional ORDER BY ... [ASC|DESC]
    if (match(TokenKind::KW_ORDER)) {
        if (!consume(TokenKind::KW_BY, "BY")) return nullptr;
        for (;;) {
            auto e = parseExpr();
            if (!e) return nullptr;
            out->orderBy.push_back(std::move(e));
            if (!match(TokenKind::COMMA)) break;
        }
        // Default direction is ASC; consume DESC if present.
        if (match(TokenKind::KW_DESC)) {
            out->orderDesc = true;
        } else {
            match(TokenKind::KW_ASC); // optional, defaults to ASC
        }
    }

    // Optional LIMIT n
    if (match(TokenKind::KW_LIMIT)) {
        if (peek().kind != TokenKind::INT_LIT) {
            err_ = "expected integer after LIMIT";
            return nullptr;
        }
        out->limit = static_cast<int>(advance().intVal);
    }

    return out;
}

// -----------------------------------------------------------------------------
// INSERT
// -----------------------------------------------------------------------------

// INSERT INTO <t> [(cols...)] VALUES (row), (row), ...
std::unique_ptr<InsertStmt> Parser::parseInsert() {
    auto out = std::make_unique<InsertStmt>();

    if (!consume(TokenKind::KW_INSERT, "INSERT")) return nullptr;
    if (!consume(TokenKind::KW_INTO,    "INTO"))  return nullptr;
    if (peek().kind != TokenKind::IDENT) {
        err_ = "expected table name after INSERT INTO";
        return nullptr;
    }
    out->table = advance().text;

    // Optional column list
    if (match(TokenKind::LPAREN)) {
        for (;;) {
            if (peek().kind != TokenKind::IDENT) {
                err_ = "expected column name";
                return nullptr;
            }
            out->columns.push_back(advance().text);
            if (!match(TokenKind::COMMA)) break;
        }
        if (!consume(TokenKind::RPAREN, "')'")) return nullptr;
    }

    if (!consume(TokenKind::KW_VALUES, "VALUES")) return nullptr;

    // One or more value rows
    for (;;) {
        if (!consume(TokenKind::LPAREN, "'('")) return nullptr;
        std::vector<std::unique_ptr<Expr>> row;
        for (;;) {
            auto e = parseExpr();
            if (!e) return nullptr;
            row.push_back(std::move(e));
            if (!match(TokenKind::COMMA)) break;
        }
        if (!consume(TokenKind::RPAREN, "')'")) return nullptr;
        out->rows.push_back(std::move(row));
        if (!match(TokenKind::COMMA)) break;
    }

    return out;
}

// -----------------------------------------------------------------------------
// DELETE
// -----------------------------------------------------------------------------

// DELETE FROM <t> [WHERE <expr>]
std::unique_ptr<DeleteStmt> Parser::parseDelete() {
    auto out = std::make_unique<DeleteStmt>();

    if (!consume(TokenKind::KW_DELETE, "DELETE")) return nullptr;
    if (!consume(TokenKind::KW_FROM,   "FROM"))   return nullptr;
    if (peek().kind != TokenKind::IDENT) {
        err_ = "expected table name after DELETE FROM";
        return nullptr;
    }
    out->table = advance().text;

    if (match(TokenKind::KW_WHERE)) {
        out->where = parseExpr();
        if (!out->where) return nullptr;
    }

    return out;
}

// -----------------------------------------------------------------------------
// CREATE
// -----------------------------------------------------------------------------

// CREATE TABLE [IF NOT EXISTS] <t> (col TYPE [NOT NULL] [PRIMARY KEY], ...)
std::unique_ptr<CreateStmt> Parser::parseCreate() {
    auto out = std::make_unique<CreateStmt>();

    if (!consume(TokenKind::KW_CREATE, "CREATE")) return nullptr;
    if (!consume(TokenKind::KW_TABLE,  "TABLE"))  return nullptr;

    // Optional IF NOT EXISTS
    if (match(TokenKind::KW_IF)) {
        if (!consume(TokenKind::KW_NOT, "NOT")) return nullptr;
        if (!consume(TokenKind::KW_EXISTS, "EXISTS")) return nullptr;
        out->ifNotExists = true;
    }

    if (peek().kind != TokenKind::IDENT) {
        err_ = "expected table name after CREATE TABLE";
        return nullptr;
    }
    out->table = advance().text;

    if (!consume(TokenKind::LPAREN, "'('")) return nullptr;

    for (;;) {
        catalog::Column col;
        if (peek().kind != TokenKind::IDENT) {
            err_ = "expected column name";
            return nullptr;
        }
        col.name = advance().text;

        col.type = parseTypeKeyword();
        if (peek().kind == TokenKind::KW_INT     ||
            peek().kind == TokenKind::KW_FLOAT   ||
            peek().kind == TokenKind::KW_BOOL    ||
            peek().kind == TokenKind::KW_VARCHAR) {
            advance();
        } else {
            err_ = "expected column type (INT, FLOAT, BOOL, VARCHAR)";
            return nullptr;
        }

        // VARCHAR needs a length: VARCHAR(n)
        if (col.type == catalog::Type::VARCHAR) {
            if (!consume(TokenKind::LPAREN, "'(' after VARCHAR")) return nullptr;
            if (peek().kind != TokenKind::INT_LIT) {
                err_ = "expected integer length after VARCHAR(";
                return nullptr;
            }
            col.length = static_cast<std::uint32_t>(advance().intVal);
            if (!consume(TokenKind::RPAREN, "')'")) return nullptr;
        }

        // Column constraints. NOT NULL and PRIMARY KEY can appear in either
        // order; PRIMARY KEY implies NOT NULL.
        bool sawNotNull    = false;
        bool sawPrimaryKey = false;
        while (peek().kind == TokenKind::KW_NOT || peek().kind == TokenKind::KW_PRIMARY) {
            if (match(TokenKind::KW_NOT)) {
                if (!consume(TokenKind::KW_NULL, "NULL")) return nullptr;
                sawNotNull = true;
                col.nullable = false;
            } else if (match(TokenKind::KW_PRIMARY)) {
                if (!consume(TokenKind::KW_KEY, "KEY")) return nullptr;
                sawPrimaryKey = true;
                col.isPrimaryKey = true;
                col.nullable = false;
            }
        }
        (void)sawNotNull;
        (void)sawPrimaryKey;

        out->columns.push_back(std::move(col));
        if (!match(TokenKind::COMMA)) break;
    }

    if (!consume(TokenKind::RPAREN, "')'")) return nullptr;
    return out;
}

// -----------------------------------------------------------------------------
// DROP
// -----------------------------------------------------------------------------

// DROP TABLE [IF EXISTS] <t>
std::unique_ptr<DropStmt> Parser::parseDrop() {
    auto out = std::make_unique<DropStmt>();

    if (!consume(TokenKind::KW_DROP,  "DROP"))  return nullptr;
    if (!consume(TokenKind::KW_TABLE, "TABLE")) return nullptr;

    if (match(TokenKind::KW_IF)) {
        if (!consume(TokenKind::KW_EXISTS, "EXISTS")) return nullptr;
        out->ifExists = true;
    }

    if (peek().kind != TokenKind::IDENT) {
        err_ = "expected table name after DROP TABLE";
        return nullptr;
    }
    out->table = advance().text;
    return out;
}

// -----------------------------------------------------------------------------
// Transaction control
// -----------------------------------------------------------------------------

// BEGIN | COMMIT | ROLLBACK
std::unique_ptr<TxnStmt> Parser::parseTxn() {
    auto out = std::make_unique<TxnStmt>();
    switch (peek().kind) {
        case TokenKind::KW_BEGIN:    out->op = TxnStmt::Op::BEGIN;    advance(); break;
        case TokenKind::KW_COMMIT:   out->op = TxnStmt::Op::COMMIT;   advance(); break;
        case TokenKind::KW_ROLLBACK: out->op = TxnStmt::Op::ROLLBACK; advance(); break;
        default:
            err_ = "expected BEGIN, COMMIT, or ROLLBACK";
            return nullptr;
    }
    return out;
}

// -----------------------------------------------------------------------------
// Expression parsing — precedence climbing
//
//   parseExpr      -> parseAnd ( OR parseAnd )*
//   parseAnd       -> parseNot ( AND parseNot )*
//   parseNot       -> NOT parseNot | parseCompare
//   parseCompare   -> parseAddSub ( (=|!=|<|<=|>|>=) parseAddSub )?
//   parseAddSub    -> parseMulDiv ( (+|-) parseMulDiv )*
//   parseMulDiv    -> parseUnary   ( (*|/|%) parseUnary )*
//   parseUnary     -> -parseUnary | parsePrimary
//   parsePrimary   -> literal | column_ref | function_call | '(' expr ')'
// -----------------------------------------------------------------------------

// OR level
std::unique_ptr<Expr> Parser::parseExpr() {
    auto left = parseAnd();
    if (!left) return nullptr;
    while (peek().kind == TokenKind::KW_OR) {
        advance();
        auto right = parseAnd();
        if (!right) return nullptr;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::BINARY_OP;
        e->op = "OR";
        e->args.push_back(std::move(left));
        e->args.push_back(std::move(right));
        left = std::move(e);
    }
    return left;
}

// AND level
std::unique_ptr<Expr> Parser::parseAnd() {
    auto left = parseNot();
    if (!left) return nullptr;
    while (peek().kind == TokenKind::KW_AND) {
        advance();
        auto right = parseNot();
        if (!right) return nullptr;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::BINARY_OP;
        e->op = "AND";
        e->args.push_back(std::move(left));
        e->args.push_back(std::move(right));
        left = std::move(e);
    }
    return left;
}

// NOT level — also handles IS NULL / IS NOT NULL
std::unique_ptr<Expr> Parser::parseNot() {
    if (match(TokenKind::KW_NOT)) {
        auto inner = parseNot();
        if (!inner) return nullptr;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::UNARY_OP;
        e->op = "NOT";
        e->args.push_back(std::move(inner));
        return e;
    }
    return parseCompare();
}

// Comparison level: =, !=, <, <=, >, >=, IS NULL, IS NOT NULL
std::unique_ptr<Expr> Parser::parseCompare() {
    auto left = parseAddSub();
    if (!left) return nullptr;

    // Handle "IS [NOT] NULL" as a unary op on `left`.
    if (peek().kind == TokenKind::KW_NULL && false) {
        // We don't match here — fall through to the IS branch below.
    }

    // Peek for a comparison operator.
    TokenKind k = peek().kind;
    std::string op;
    bool isCompare = false;
    switch (k) {
        case TokenKind::EQ: op = "=";  isCompare = true; break;
        case TokenKind::NEQ: op = "!="; isCompare = true; break;
        case TokenKind::LT:  op = "<";  isCompare = true; break;
        case TokenKind::LE:  op = "<="; isCompare = true; break;
        case TokenKind::GT:  op = ">";  isCompare = true; break;
        case TokenKind::GE:  op = ">="; isCompare = true; break;
        default: break;
    }

    // IS [NOT] NULL
    if (!isCompare && k == TokenKind::KW_NULL) {
        // "left IS NULL" / "left IS NOT NULL"
        // Distinguish by peeking ahead.
        if (pos_ + 1 < tokens_.size() && tokens_[pos_ + 1].kind == TokenKind::KW_NOT) {
            advance(); // NULL
            advance(); // NOT
            if (!consume(TokenKind::KW_NULL, "NULL")) return nullptr;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::UNARY_OP;
            e->op = "IS NOT NULL";
            e->args.push_back(std::move(left));
            return e;
        }
        advance(); // NULL
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::UNARY_OP;
        e->op = "IS NULL";
        e->args.push_back(std::move(left));
        return e;
    }

    if (isCompare) {
        advance();
        auto right = parseAddSub();
        if (!right) return nullptr;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::BINARY_OP;
        e->op = op;
        e->args.push_back(std::move(left));
        e->args.push_back(std::move(right));
        return e;
    }

    return left;
}

// Addition / subtraction
std::unique_ptr<Expr> Parser::parseAddSub() {
    auto left = parseMulDiv();
    if (!left) return nullptr;
    while (peek().kind == TokenKind::PLUS || peek().kind == TokenKind::MINUS) {
        std::string op = (peek().kind == TokenKind::PLUS) ? "+" : "-";
        advance();
        auto right = parseMulDiv();
        if (!right) return nullptr;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::BINARY_OP;
        e->op = op;
        e->args.push_back(std::move(left));
        e->args.push_back(std::move(right));
        left = std::move(e);
    }
    return left;
}

// Multiplication / division / modulo
std::unique_ptr<Expr> Parser::parseMulDiv() {
    auto left = parseUnary();
    if (!left) return nullptr;
    while (peek().kind == TokenKind::STAR  ||
           peek().kind == TokenKind::SLASH ||
           peek().kind == TokenKind::PERCENT) {
        std::string op;
        switch (peek().kind) {
            case TokenKind::STAR:    op = "*"; break;
            case TokenKind::SLASH:   op = "/"; break;
            case TokenKind::PERCENT: op = "%"; break;
            default: break;
        }
        advance();
        auto right = parseUnary();
        if (!right) return nullptr;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::BINARY_OP;
        e->op = op;
        e->args.push_back(std::move(left));
        e->args.push_back(std::move(right));
        left = std::move(e);
    }
    return left;
}

// Unary minus / plus
std::unique_ptr<Expr> Parser::parseUnary() {
    if (peek().kind == TokenKind::MINUS) {
        advance();
        auto inner = parseUnary();
        if (!inner) return nullptr;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::UNARY_OP;
        e->op = "-";
        e->args.push_back(std::move(inner));
        return e;
    }
    if (peek().kind == TokenKind::PLUS) {
        advance();
        return parseUnary();
    }
    return parsePrimary();
}

// Literal, column reference, function call, or parenthesised expression.
// A column reference may be qualified as `table.column`; we keep the dot
// inside `text` and let the planner split it.
std::unique_ptr<Expr> Parser::parsePrimary() {
    const Token& t = peek();
    SourceLoc loc = t.loc;

    // Parenthesised expression
    if (t.kind == TokenKind::LPAREN) {
        advance();
        auto inner = parseExpr();
        if (!inner) return nullptr;
        if (!consume(TokenKind::RPAREN, "')'")) return nullptr;
        return inner;
    }

    // Literals
    if (t.kind == TokenKind::INT_LIT) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::INT_LIT;
        e->intVal = t.intVal;
        e->line = loc.line;
        e->col = loc.col;
        return e;
    }
    if (t.kind == TokenKind::FLOAT_LIT) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::FLOAT_LIT;
        e->floatVal = t.floatVal;
        e->line = loc.line;
        e->col = loc.col;
        return e;
    }
    if (t.kind == TokenKind::STR_LIT) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::STR_LIT;
        e->strVal = t.text;
        e->line = loc.line;
        e->col = loc.col;
        return e;
    }
    if (t.kind == TokenKind::KW_TRUE) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::BOOL_LIT;
        e->boolVal = true;
        e->line = loc.line;
        e->col = loc.col;
        return e;
    }
    if (t.kind == TokenKind::KW_FALSE) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::BOOL_LIT;
        e->boolVal = false;
        e->line = loc.line;
        e->col = loc.col;
        return e;
    }
    if (t.kind == TokenKind::KW_NULL) {
        advance();
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::NULL_LIT;
        e->line = loc.line;
        e->col = loc.col;
        return e;
    }

    // Identifier: either a column reference, qualified column, or a
    // function call when followed by '('.
    if (t.kind == TokenKind::IDENT) {
        std::string name = t.text;
        advance();

        // Function call: NAME '(' [args] ')'
        if (peek().kind == TokenKind::LPAREN) {
            advance();
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::FUNCTION_CALL;
            e->text = name;
            e->line = loc.line;
            e->col  = loc.col;
            // Empty arg list is allowed (e.g. NOW()).
            if (peek().kind != TokenKind::RPAREN) {
                for (;;) {
                    // COUNT(*) — accept STAR as a pseudo-arg.
                    if (peek().kind == TokenKind::STAR) {
                        advance();
                        auto star = std::make_unique<Expr>();
                        star->kind = ExprKind::COLUMN;
                        star->text = "*";
                        e->args.push_back(std::move(star));
                    } else {
                        auto a = parseExpr();
                        if (!a) return nullptr;
                        e->args.push_back(std::move(a));
                    }
                    if (!match(TokenKind::COMMA)) break;
                }
            }
            if (!consume(TokenKind::RPAREN, "')'")) return nullptr;
            return e;
        }

        // Optional qualifier: name DOT name (or name DOT STAR)
        if (match(TokenKind::DOT)) {
            if (peek().kind == TokenKind::STAR) {
                advance();
                name += ".*";
            } else if (peek().kind == TokenKind::IDENT) {
                name += "." + advance().text;
            } else {
                err_ = "expected identifier or '*' after '.'";
                return nullptr;
            }
        }

        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::COLUMN;
        e->text = std::move(name);
        e->line = loc.line;
        e->col  = loc.col;
        return e;
    }

    // Bare STAR is only legal as a SELECT projection; here it's a syntax error.
    err_ = "unexpected token in expression: '" + t.text + "'";
    return nullptr;
}

// -----------------------------------------------------------------------------
// Type keyword helper
// -----------------------------------------------------------------------------

// Map a type keyword token to the catalog::Type enum. The caller is
// expected to have already advanced past the keyword; this helper exists
// because we sometimes want to peek-then-advance.
catalog::Type Parser::parseTypeKeyword() {
    switch (peek().kind) {
        case TokenKind::KW_INT:     return catalog::Type::INT;
        case TokenKind::KW_FLOAT:   return catalog::Type::FLOAT;
        case TokenKind::KW_VARCHAR: return catalog::Type::VARCHAR;
        case TokenKind::KW_BOOL:    return catalog::Type::BOOL;
        default:                    return catalog::Type::INT;
    }
}

} // namespace minidb::parser