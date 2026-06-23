// =============================================================================
// src/cli/main.cpp
// -----------------------------------------------------------------------------
// MiniDB CLI. Two modes:
//   - default: read SQL from stdin, one statement per line
//   - file mode: minidb <script.sql>
//
// Startup sequence (see include/storage/README.md and docs/architecture.md):
//   1. Open DiskManager at data/
//   2. Open BufferPool
//   3. Load CatalogManager
//   4. Open WAL, construct RecoveryManager, runAtStartup()
//   5. Construct QueryEngine
//   6. REPL: read line, hand to QueryEngine
// =============================================================================
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "executor/query_engine.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "catalog/catalog_manager.h"
#include "index/index_manager.h"
#include "transaction/transaction_manager.h"
#include "recovery/wal.h"
#include "recovery/recovery_manager.h"

using namespace minidb;

static void printBanner() {
    std::cout <<
        "MiniDB v0.1.0  (Advanced DBMS capstone)\n"
        "Type SQL and end with ';' or '\\q' to quit.\n"
        "\n";
}

// Run a single SQL statement. The QueryEngine exposes two entry points:
//   execute()        -> SELECT, returns rows
//   executeUpdate()  -> INSERT/DELETE/CREATE/DROP/TXN, returns Status
// We peek at the first keyword to route to the right one.
static Status runStatement(executor::QueryEngine& qe,
                           const std::string& sql,
                           std::vector<executor::Tuple>& outRows) {
    // Find the first non-whitespace, upper-case it, and check the verb.
    std::size_t i = sql.find_first_not_of(" \t\r\n");
    std::string head;
    if (i != std::string::npos) {
        for (std::size_t j = i; j < sql.size() && std::isalpha(static_cast<unsigned char>(sql[j])); ++j) {
            head += static_cast<char>(std::toupper(static_cast<unsigned char>(sql[j])));
        }
    }
    if (head == "SELECT" || head == "WITH") {
        outRows = qe.execute(sql);
        return Status::OK;
    }
    outRows.clear();
    return qe.executeUpdate(sql);
}

// isSelectLike() — returns true when `sql` looks like a SELECT/WITH query.
// Used only for user-facing feedback so we can distinguish "SELECT
// returned 0 rows" from "INSERT/etc succeeded".
static bool isSelectLike(const std::string& sql) {
    std::size_t i = sql.find_first_not_of(" \t\r\n");
    if (i == std::string::npos) return false;
    std::string head;
    for (std::size_t j = i; j < sql.size() && std::isalpha(static_cast<unsigned char>(sql[j])); ++j) {
        head += static_cast<char>(std::toupper(static_cast<unsigned char>(sql[j])));
    }
    return head == "SELECT" || head == "WITH";
}

// tryDotCommand() — handle SQLite-style dot-commands (".tables",
// ".quit", ".help"). Returns true if `line` matched a dot-command
// (and the caller should NOT treat it as SQL). The helper is given
// access to the catalog so .tables can list real tables.
static bool tryDotCommand(const std::string& line,
                          catalog::CatalogManager* cat) {
    if (line.empty() || line[0] != '.') return false;

    // Split first whitespace-separated token.
    std::string cmd;
    std::size_t i = 0;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
        cmd += static_cast<char>(std::tolower(static_cast<unsigned char>(line[i])));
        ++i;
    }
    // skip trailing whitespace
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    std::string arg = line.substr(i);
    while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.back()))) arg.pop_back();

    if (cmd == ".tables") {
        if (cat == nullptr) {
            std::cout << "(no catalog)\n";
        } else {
            auto tables = cat->listTables();
            if (tables.empty()) {
                std::cout << "(no tables)\n";
            } else {
                for (const auto& t : tables) std::cout << t << "\n";
            }
        }
        return true;
    }
    if (cmd == ".quit" || cmd == ".exit" || cmd == ".q") {
        std::exit(0);
    }
    if (cmd == ".help") {
        std::cout <<
            "MiniDB dot-commands:\n"
            "  .tables             list tables\n"
            "  .quit / .exit / .q  exit the REPL\n"
            "  .help               show this message\n"
            "\n"
            "SQL statements end with ';' (e.g. SELECT * FROM users;).\n";
        return true;
    }
    if (cmd == ".schema") {
        if (cat == nullptr) {
            std::cout << "(no catalog)\n";
        } else {
            auto tables = cat->listTables();
            if (tables.empty()) {
                std::cout << "(no tables)\n";
            } else {
                auto typeName = [](catalog::Type t) -> const char* {
                    switch (t) {
                        case catalog::Type::INT:     return "INT";
                        case catalog::Type::FLOAT:   return "FLOAT";
                        case catalog::Type::VARCHAR: return "VARCHAR";
                        case catalog::Type::BOOL:    return "BOOL";
                    }
                    return "?";
                };
                for (const auto& t : tables) {
                    const auto* info = cat->getTable(t);
                    if (info == nullptr) continue;
                    std::cout << t << "(";
                    const auto& cols = info->schema.columns();
                    for (std::size_t k = 0; k < cols.size(); ++k) {
                        if (k) std::cout << ", ";
                        std::cout << cols[k].name << " " << typeName(cols[k].type);
                        if (cols[k].type == catalog::Type::VARCHAR && cols[k].length > 0) {
                            std::cout << "(" << cols[k].length << ")";
                        }
                        if (cols[k].isPrimaryKey) std::cout << " PRIMARY KEY";
                    }
                    std::cout << ");\n";
                }
            }
        }
        return true;
    }

    // Unknown dot command — let the caller print an error.
    std::cout << "Unknown dot-command '" << cmd << "' (try .help)\n";
    return true;
}

static int runInteractive(executor::QueryEngine& qe, catalog::CatalogManager* cat) {
    std::string line;
    std::string buffer;
    std::cout << "minidb> " << std::flush;
    while (std::getline(std::cin, line)) {
        // Strip leading/trailing whitespace for empty checks.
        if (!buffer.empty()) buffer += ' ';
        buffer += line;

        // Trim to check for empty / quit.
        size_t start = buffer.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            buffer.clear();
            std::cout << "minidb> " << std::flush;
            continue;
        }
        size_t end = buffer.find_last_not_of(" \t\r\n");
        std::string trimmed = buffer.substr(start, end - start + 1);

        if (trimmed == "\\q" || trimmed == "\\quit" || trimmed == "quit") {
            break;
        }
        if (trimmed.empty()) {
            buffer.clear();
            std::cout << "minidb> " << std::flush;
            continue;
        }

        // Dot-commands are single-line shortcuts. We process them only
        // when no SQL has been accumulated yet, and only when the line
        // itself starts with '.' (not when it appears mid-buffer).
        if (buffer.size() == line.size() && !buffer.empty() && buffer[0] == '.') {
            if (tryDotCommand(trimmed, cat)) {
                buffer.clear();
                std::cout << "minidb> " << std::flush;
                continue;
            }
        }

        // Keep accumulating lines until the buffer ends with ';'.
        if (buffer.back() != ';') {
            std::cout << "    -> " << std::flush;
            continue;
        }

        std::string stmt = buffer;
        buffer.clear();

        try {
            std::vector<executor::Tuple> rows;
            Status st = runStatement(qe, stmt, rows);
            if (st == Status::OK) {
                if (rows.empty()) {
                    // Distinguish "DML succeeded" from "SELECT returned
                    // zero rows". Without this, INSERT/CREATE/DROP and
                    // empty SELECTs are indistinguishable to the user.
                    if (isSelectLike(stmt)) {
                        std::cout << "(0 rows)\n";
                    } else {
                        std::cout << "OK\n";
                    }
                } else {
                    for (const auto& t : rows) {
                        std::cout << t.toString() << "\n";
                    }
                    std::cout << "(" << rows.size() << " row"
                              << (rows.size() == 1 ? "" : "s") << ")\n";
                }
            } else {
                std::cout << "Status: " << toString(st) << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }
        std::cout << "minidb> " << std::flush;
    }
    std::cout << "\n";
    return 0;
}

static int runFile(executor::QueryEngine& qe, const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "minidb: cannot open script '" << path << "'\n";
        return 1;
    }

    std::stringstream ss;
    ss << in.rdbuf();
    std::string script = ss.str();

    // Split the script on ';'. Real scripts are simple enough that a
    // single-line / single-statement-per-';' split is fine.
    std::string stmt;
    for (char c : script) {
        if (c == ';') {
            std::string trimmed = stmt;
            // trim
            size_t s = trimmed.find_first_not_of(" \t\r\n");
            size_t e = trimmed.find_last_not_of(" \t\r\n");
            if (s != std::string::npos) {
                trimmed = trimmed.substr(s, e - s + 1);
            } else {
                trimmed.clear();
            }
            if (!trimmed.empty() && trimmed != "\\q") {
                try {
                    std::vector<executor::Tuple> rows;
                    Status st = runStatement(qe, trimmed + ";", rows);
                    if (st == Status::OK) {
                        if (rows.empty()) {
                            if (isSelectLike(trimmed)) {
                                std::cout << "(0 rows)\n";
                            } else {
                                std::cout << "OK\n";
                            }
                        } else {
                            for (const auto& t : rows) {
                                std::cout << t.toString() << "\n";
                            }
                            std::cout << "(" << rows.size() << " row"
                                      << (rows.size() == 1 ? "" : "s") << ")\n";
                        }
                    } else {
                        std::cout << "Status: " << toString(st) << "\n";
                    }
                    std::cout.flush();
                } catch (const std::exception& ex) {
                    std::cerr << "Error: " << ex.what() << "\n";
                    return 1;
                }
            }
            stmt.clear();
        } else {
            stmt += c;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    printBanner();

    // 1. Disk manager. The path is the file inside the data/ folder.
    //    DiskManager creates the parent directory as needed.
    storage::DiskManager disk("data/minidb.db");

    // 2. Buffer pool on top of the disk manager.
    storage::BufferPool bp(&disk, /*numFrames=*/64);

    // 3. Catalog.
    catalog::CatalogManager cat(&disk);
    Status cs = cat.load();
    if (cs != Status::OK && cs != Status::UNIMPLEMENTED) {
        std::cerr << "minidb: catalog load failed (" << toString(cs) << ")\n";
        return 1;
    }

    // 4. Index manager (owns B+ tree instances).
    index::IndexManager idx(&bp, &cat);

    // 5. Transaction manager (provides LockManager + MVCC).
    transaction::TransactionManager txnMgr;

    // 6. WAL + recovery. RecoveryManager::runAtStartup() replays the log
    //    before any new traffic arrives.
    recovery::WAL wal("data/wal/minidb.wal");
    recovery::RecoveryManager recMgr(&wal, &bp, &cat, &idx, &txnMgr);
    Status rs = recMgr.runAtStartup();
    if (rs != Status::OK && rs != Status::UNIMPLEMENTED) {
        std::cerr << "minidb: recovery failed (" << toString(rs) << ")\n";
        return 1;
    }

    // 7. QueryEngine. It owns the optimizer and an executor context.
    executor::QueryEngine qe(&bp, &cat, &idx, &txnMgr, &recMgr);

    // 8. REPL or file mode.
    int rc = 0;
    if (argc >= 2) {
        rc = runFile(qe, argv[1]);
    } else {
        rc = runInteractive(qe, &cat);
    }

    // 9. Graceful shutdown: flush dirty pages, persist the catalog, and
    //    drop the WAL. Order matters — WAL last so the catalog & pages
    //    are durable first.
    cat.flush();
    bp.flushAll();
    disk.flush();
    return rc;
}
