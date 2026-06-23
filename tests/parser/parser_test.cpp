// =============================================================================
// tests/parser/parser_test.cpp
// -----------------------------------------------------------------------------
// Round-trip the lexer + parser for a small set of canonical queries.
// =============================================================================
#include <cassert>
#include <cstdio>

#include "parser/lexer.h"
#include "parser/parser.h"

int main() {
    const char* kSamples[] = {
        "SELECT * FROM t;",
        "SELECT id, name FROM users WHERE id = 10;",
        "INSERT INTO t (a, b) VALUES (1, 'x');",
        "DELETE FROM t WHERE id = 1;",
        "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(20));",
        "DROP TABLE t;",
        "BEGIN;",
        "COMMIT;",
    };

    for (const char* sql : kSamples) {
        auto toks = minidb::parser::Lexer(sql).tokenize();
        minidb::parser::Parser p(toks);
        minidb::parser::Stmt s = p.parse();
        if (!p.lastError().empty()) {
            std::fprintf(stderr, "[fail] %s -> %s\n", sql, p.lastError().c_str());
            return 1;
        }
        std::printf("[ok]   %s\n", sql);
    }
    return 0;
}
