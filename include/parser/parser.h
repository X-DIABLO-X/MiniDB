// =============================================================================
// include/parser/parser.h
// -----------------------------------------------------------------------------
// Recursive-descent parser. Single entry point is Parser::parse() for a
// single statement, or Parser::parseScript() for a ;-separated script.
// =============================================================================
#pragma once

#include <string>
#include <vector>

#include "parser/ast.h"
#include "parser/lexer.h"

namespace minidb::parser {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    ~Parser();

    Parser(const Parser&)            = delete;
    Parser& operator=(const Parser&) = delete;

    Stmt               parse();          // one statement
    std::vector<Stmt>  parseScript();    // ;-separated

    const std::string& lastError() const { return err_; }

private:
    std::vector<Token> tokens_;
    std::size_t        pos_ = 0;
    std::string        err_;

    const Token& peek() const;
    const Token& advance();
    bool         match(TokenKind k);
    bool         consume(TokenKind k, const char* what);

    // Grammar
    std::unique_ptr<Stmt>    parseStatement();
    std::unique_ptr<SelectStmt> parseSelect();
    std::unique_ptr<InsertStmt> parseInsert();
    std::unique_ptr<DeleteStmt> parseDelete();
    std::unique_ptr<CreateStmt> parseCreate();
    std::unique_ptr<DropStmt>   parseDrop();
    std::unique_ptr<TxnStmt>    parseTxn();

    // Expressions (precedence climbing)
    std::unique_ptr<Expr> parseExpr();          // OR
    std::unique_ptr<Expr> parseAnd();
    std::unique_ptr<Expr> parseNot();
    std::unique_ptr<Expr> parseCompare();
    std::unique_ptr<Expr> parseAddSub();
    std::unique_ptr<Expr> parseMulDiv();
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parsePrimary();

    // Helpers
    catalog::Type parseTypeKeyword();
};

} // namespace minidb::parser