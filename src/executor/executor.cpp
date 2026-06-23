// =============================================================================
// src/executor/executor.cpp
// -----------------------------------------------------------------------------
// Value::toString and Tuple::toString formatters. Used by the CLI to render
// result rows. Format conventions:
//   NULL   -> "NULL"
//   int32  -> "123"
//   float  -> "1.5"
//   string -> 'foo'   (single-quoted, no escaping for v1)
//   bool   -> "true" / "false"
// =============================================================================
#include "executor/executor.h"

#include <sstream>
#include <string>
#include <vector>

namespace minidb::executor {

// Render a single Value to its CLI string form.
std::string Value::toString() const {
    switch (tag) {
        case Tag::NULL_:  return "NULL";
        case Tag::INT:    return std::to_string(i);
        case Tag::FLOAT:  return std::to_string(f);
        case Tag::BOOL:   return b ? "true" : "false";
        case Tag::STRING: return std::string("'") + s + "'";
    }
    return "NULL";
}

// Render a Tuple as a comma-separated list of its values' string forms.
std::string Tuple::toString() const {
    std::ostringstream os;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) os << ", ";
        os << values[i].toString();
    }
    return os.str();
}

} // namespace minidb::executor
