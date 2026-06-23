# `parser/` — lexer, parser, AST

Turns SQL text into a typed AST (`parser::Stmt`). The parser is the **only**
module that reads SQL strings; everything downstream works on `Stmt` values.

## Files in this folder

| Header | Responsibility |
|---|---|
| `include/parser/ast.h`   | All AST node types (`Stmt`, `SelectStmt`, `InsertStmt`, …). |
| `include/parser/lexer.h` | Tokeniser. |
| `include/parser/parser.h`| Recursive-descent parser. |

## Public API (contract)

```cpp
namespace minidb::parser {

enum class StmtKind { SELECT, INSERT, DELETE, CREATE, DROP, TXN };

struct Stmt {
    StmtKind kind;
    // tag-discriminated union:
    std::unique_ptr<SelectStmt> select;
    std::unique_ptr<InsertStmt> insert;
    std::unique_ptr<DeleteStmt> del;
    std::unique_ptr<CreateStmt> create;
    std::unique_ptr<DropStmt>   drop;
    std::unique_ptr<TxnStmt>    txn;
};

enum class TokenKind { /* KW_*, IDENT, INT_LIT, FLOAT_LIT, STR_LIT, OP, ... */ };
struct Token { TokenKind kind; std::string text; int64_t intVal; double floatVal; SourceLoc loc; };

class Lexer {
public:
    explicit Lexer(std::string_view source);
    std::vector<Token> tokenize();
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    Stmt               parse();                  // one statement
    std::vector<Stmt>  parseScript();            // ; separated
};

}
```

## AST shapes (kept short — full definitions in `ast.h`)

```cpp
struct SelectStmt {
    bool        distinct = false;
    std::vector<std::string> projection;          // empty = "*"
    std::string fromTable;
    std::unique_ptr<Expr> where;                 // see Expr in ast.h
    std::vector<std::pair<std::string, std::string>> join; // (left.col, right.col) for ON
    std::string joinTable;
    std::vector<std::string> groupBy;
    std::vector<std::string> orderBy;
    bool orderDesc = false;
    int  limit = -1;                              // -1 = no limit
};

struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;             // empty = all columns
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

struct DropStmt   { std::string table; bool ifExists = false; };
struct TxnStmt    { enum class Op { BEGIN, COMMIT, ROLLBACK }; Op op; };
```

## Supported SQL (v1)

- `SELECT [DISTINCT] <cols|*> FROM <t> [JOIN <t2> ON a.x = b.y] [WHERE ...] [GROUP BY ...] [ORDER BY ... [ASC|DESC]] [LIMIT n]`
- `INSERT INTO <t> [(cols...)] VALUES (row), (row), ...`
- `DELETE FROM <t> [WHERE ...]`
- `CREATE TABLE [IF NOT EXISTS] <t> (col TYPE [NOT NULL] [PRIMARY KEY], ...)`
- `DROP TABLE [IF EXISTS] <t>`
- `BEGIN;` / `COMMIT;` / `ROLLBACK;`

## How other modules use the parser

| Module | Calls |
|---|---|
| `cli/main.cpp` | `Lexer::tokenize` + `Parser::parse` per user line. |
| `planner/Planner` | consumes a `parser::Stmt` (read-only). |

## Rules

- The parser is **schema-agnostic** — it does not look up tables or
  columns. Validation (does the table exist? does the column exist?) is
  the planner's job.
- The lexer is whitespace-insensitive but **case-insensitive** only for
  keywords. Identifiers preserve case.
- The parser returns errors via `Parser::lastError`; the CLI surfaces
  them to the user. Throwing from the parser is reserved for
  unrecoverable cases (OOM, etc.).
