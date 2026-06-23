// =============================================================================
// include/executor/executor.h
// -----------------------------------------------------------------------------
// Executor base contract + Tuple/Value types. See include/executor/README.md
// for the Volcano pattern.
// =============================================================================
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catalog/catalog_manager.h"
#include "common/status.h"
#include "index/index_manager.h"
#include "storage/buffer_pool.h"
#include "transaction/transaction_manager.h"

namespace minidb::executor {

// ----- Value / Tuple -----
struct Value {
    enum class Tag { INT, FLOAT, STRING, BOOL, NULL_ };
    Tag         tag  = Tag::NULL_;
    int32_t     i    = 0;
    float       f    = 0.0f;
    std::string s;
    bool        b    = false;

    static Value makeInt  (int32_t v)            { Value x; x.tag = Tag::INT;   x.i = v;       return x; }
    static Value makeFloat(float v)              { Value x; x.tag = Tag::FLOAT; x.f = v;       return x; }
    static Value makeStr  (std::string v)        { Value x; x.tag = Tag::STRING;x.s = std::move(v); return x; }
    static Value makeBool (bool v)               { Value x; x.tag = Tag::BOOL;  x.b = v;       return x; }
    static Value makeNull ()                     { Value x; x.tag = Tag::NULL_;            return x; }

    std::string toString() const;
};

struct Tuple {
    std::vector<Value> values;

    std::string toString() const;     // comma-separated, for CLI output
};

// ----- ExecutorContext (deps passed down to every executor) -----
struct ExecutorContext {
    storage::BufferPool*          bp;
    catalog::CatalogManager*      cat;
    index::IndexManager*          idx;
    transaction::TransactionManager* txn;
};

// ----- Base class for every executor -----
class Executor {
public:
    explicit Executor(ExecutorContext* ctx) : ctx_(ctx) {}
    virtual ~Executor() = default;

    virtual Status init()                 = 0;
    virtual Status next (Tuple& out)      = 0;
    virtual Status close()                = 0;

protected:
    ExecutorContext* ctx_;
};

} // namespace minidb::executor