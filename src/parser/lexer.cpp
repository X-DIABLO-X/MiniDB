// =============================================================================
// src/parser/lexer.cpp
// -----------------------------------------------------------------------------
// Hand-written lexer. See include/parser/README.md for the supported SQL.
// =============================================================================
#include "parser/lexer.h"

#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace minidb::parser {

namespace {
const std::unordered_map<std::string, TokenKind> kKeywordMap = {
    {"SELECT",   TokenKind::KW_SELECT},
    {"FROM",     TokenKind::KW_FROM},
    {"WHERE",    TokenKind::KW_WHERE},
    {"INSERT",   TokenKind::KW_INSERT},
    {"INTO",     TokenKind::KW_INTO},
    {"VALUES",   TokenKind::KW_VALUES},
    {"DELETE",   TokenKind::KW_DELETE},
    {"CREATE",   TokenKind::KW_CREATE},
    {"TABLE",    TokenKind::KW_TABLE},
    {"DROP",     TokenKind::KW_DROP},
    {"PRIMARY",  TokenKind::KW_PRIMARY},
    {"KEY",      TokenKind::KW_KEY},
    {"NOT",      TokenKind::KW_NOT},
    {"NULL",     TokenKind::KW_NULL},
    {"AND",      TokenKind::KW_AND},
    {"OR",       TokenKind::KW_OR},
    {"JOIN",     TokenKind::KW_JOIN},
    {"ON",       TokenKind::KW_ON},
    {"GROUP",    TokenKind::KW_GROUP},
    {"BY",       TokenKind::KW_BY},
    {"ORDER",    TokenKind::KW_ORDER},
    {"ASC",      TokenKind::KW_ASC},
    {"DESC",     TokenKind::KW_DESC},
    {"LIMIT",    TokenKind::KW_LIMIT},
    {"DISTINCT", TokenKind::KW_DISTINCT},
    {"BEGIN",    TokenKind::KW_BEGIN},
    {"COMMIT",   TokenKind::KW_COMMIT},
    {"ROLLBACK", TokenKind::KW_ROLLBACK},
    {"INT",      TokenKind::KW_INT},
    {"FLOAT",    TokenKind::KW_FLOAT},
    {"BOOL",     TokenKind::KW_BOOL},
    {"VARCHAR",  TokenKind::KW_VARCHAR},
    {"TRUE",     TokenKind::KW_TRUE},
    {"FALSE",    TokenKind::KW_FALSE},
    {"IF",       TokenKind::KW_IF},
    {"EXISTS",   TokenKind::KW_EXISTS},
};
} // namespace

Lexer::Lexer(std::string_view src) : src_(src) {}

void Lexer::skipWhitespace() {
    while (pos_ < src_.size()) {
        char c = src_[pos_];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++pos_; ++col_;
        } else if (c == '\n') {
            ++pos_; ++line_; col_ = 1;
        } else if (c == '-' && pos_ + 1 < src_.size() && src_[pos_+1] == '-') {
            // line comment
            while (pos_ < src_.size() && src_[pos_] != '\n') { ++pos_; }
        } else {
            break;
        }
    }
}

Token Lexer::readIdent() {
    Token t; t.loc = {line_, col_};
    while (pos_ < src_.size() &&
           (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_')) {
        t.text.push_back(src_[pos_]);
        ++pos_; ++col_;
    }
    // SQL is case-insensitive for keywords; lowercase for the keyword
    // lookup so the user can type "select" / "SELECT" / "Select"
    // interchangeably. Identifiers (table/column names) are preserved
    // in their original case because we look them up in the catalog
    // using case-sensitive match (v1: case-sensitive identifiers).
    std::string upper = t.text;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto it = kKeywordMap.find(upper);
    if (it != kKeywordMap.end()) {
        t.kind = it->second;
    } else {
        t.kind = TokenKind::IDENT;
    }
    return t;
}

Token Lexer::readNumber() {
    Token t; t.loc = {line_, col_};
    bool isFloat = false;
    while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
        t.text.push_back(src_[pos_]); ++pos_; ++col_;
    }
    if (pos_ < src_.size() && src_[pos_] == '.') {
        isFloat = true;
        t.text.push_back('.'); ++pos_; ++col_;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            t.text.push_back(src_[pos_]); ++pos_; ++col_;
        }
    }
    if (isFloat) {
        t.floatVal = std::stod(t.text);
        t.kind = TokenKind::FLOAT_LIT;
    } else {
        t.intVal = std::stoll(t.text);
        t.kind = TokenKind::INT_LIT;
    }
    return t;
}

Token Lexer::readString() {
    Token t; t.loc = {line_, col_};
    char quote = src_[pos_];
    ++pos_; ++col_;          // consume opening quote
    while (pos_ < src_.size() && src_[pos_] != quote) {
        if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
            char esc = src_[pos_+1];
            char real = (esc == 'n') ? '\n' : (esc == 't') ? '\t' :
                        (esc == 'r') ? '\r' : (esc == '\\') ? '\\' :
                        (esc == '\'') ? '\'' : esc;
            t.text.push_back(real);
            pos_ += 2; col_ += 2;
        } else {
            t.text.push_back(src_[pos_]);
            ++pos_; ++col_;
        }
    }
    if (pos_ < src_.size()) { ++pos_; ++col_; }   // closing quote
    t.kind = TokenKind::STR_LIT;
    return t;
}

Token Lexer::punct() {
    Token t; t.loc = {line_, col_};
    char c = src_[pos_];
    char n = (pos_ + 1 < src_.size()) ? src_[pos_ + 1] : '\0';
    auto take = [&](TokenKind k, std::size_t adv = 1) {
        t.kind = k; t.text.push_back(c);
        if (adv == 2) t.text.push_back(n);
        pos_ += adv; col_ += static_cast<int>(adv);
    };
    switch (c) {
        case '*': take(TokenKind::STAR); break;
        case ',': take(TokenKind::COMMA); break;
        case '(': take(TokenKind::LPAREN); break;
        case ')': take(TokenKind::RPAREN); break;
        case ';': take(TokenKind::SEMICOLON); break;
        case '.': take(TokenKind::DOT); break;
        case '+': take(TokenKind::PLUS); break;
        case '-': take(TokenKind::MINUS); break;
        case '/': take(TokenKind::SLASH); break;
        case '%': take(TokenKind::PERCENT); break;
        case '=': take(TokenKind::EQ, 1); break;
        case '<':
            if (n == '=') take(TokenKind::LE, 2);
            else if (n == '>') take(TokenKind::NEQ, 2);
            else take(TokenKind::LT, 1);
            break;
        case '>':
            if (n == '=') take(TokenKind::GE, 2);
            else take(TokenKind::GT, 1);
            break;
        case '!':
            if (n == '=') take(TokenKind::NEQ, 2);
            else { t.kind = TokenKind::UNKNOWN; t.text = "!"; ++pos_; }
            break;
        default:
            t.kind = TokenKind::UNKNOWN;
            t.text.push_back(c);
            ++pos_; ++col_;
    }
    return t;
}

Token Lexer::nextToken() {
    skipWhitespace();
    if (pos_ >= src_.size()) {
        Token t; t.kind = TokenKind::END_OF_INPUT; t.loc = {line_, col_}; return t;
    }
    char c = src_[pos_];
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return readIdent();
    if (std::isdigit(static_cast<unsigned char>(c)))              return readNumber();
    if (c == '\'' || c == '"')                                    return readString();
    return punct();
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    while (true) {
        Token t = nextToken();
        if (t.kind == TokenKind::END_OF_INPUT) {
            out.push_back(t);
            break;
        }
        out.push_back(t);
    }
    return out;
}

} // namespace minidb::parser