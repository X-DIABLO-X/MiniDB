// =============================================================================
// include/parser/lexer.h
// -----------------------------------------------------------------------------
// Tokeniser for MiniDB's SQL dialect. See include/parser/README.md for the
// grammar.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace minidb::parser {

enum class TokenKind {
    // keywords
    KW_SELECT, KW_FROM, KW_WHERE, KW_INSERT, KW_INTO, KW_VALUES,
    KW_DELETE, KW_CREATE, KW_TABLE, KW_DROP, KW_PRIMARY, KW_KEY,
    KW_NOT, KW_NULL, KW_AND, KW_OR, KW_JOIN, KW_ON,
    KW_GROUP, KW_BY, KW_ORDER, KW_ASC, KW_DESC, KW_LIMIT, KW_DISTINCT,
    KW_BEGIN, KW_COMMIT, KW_ROLLBACK,
    KW_INT, KW_FLOAT, KW_BOOL, KW_VARCHAR, KW_TRUE, KW_FALSE,
    KW_IF, KW_EXISTS,

    // literals / identifiers
    IDENT, INT_LIT, FLOAT_LIT, STR_LIT,

    // operators and punctuation
    STAR, COMMA, LPAREN, RPAREN, SEMICOLON, DOT,
    EQ, NEQ, LT, LE, GT, GE,
    PLUS, MINUS, SLASH, PERCENT,

    END_OF_INPUT, UNKNOWN
};

struct SourceLoc { int line = 1, col = 1; };

struct Token {
    TokenKind           kind = TokenKind::UNKNOWN;
    std::string         text;
    int64_t             intVal  = 0;
    double              floatVal = 0.0;
    SourceLoc           loc;
};

class Lexer {
public:
    explicit Lexer(std::string_view source);
    std::vector<Token> tokenize();
    const std::string& lastError() const { return err_; }

private:
    std::string_view src_;
    std::size_t      pos_ = 0;
    int              line_ = 1, col_ = 1;
    std::string      err_;

    Token nextToken();
    void  skipWhitespace();
    Token readIdent();
    Token readNumber();
    Token readString();
    Token punct();
};

} // namespace minidb::parser