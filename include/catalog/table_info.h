// =============================================================================
// include/catalog/table_info.h
// -----------------------------------------------------------------------------
// TableInfo = the metadata the executor and index need to work with a
// table. It is the *value* the CatalogManager hands out.
// =============================================================================
#pragma once

#include <string>

#include "catalog/schema.h"
#include "common/types.h"

namespace minidb::catalog {

struct TableInfo {
    std::string name;                 // unique within the database
    Schema      schema;
    PageId      firstPageId = 0;     // 0 = heap file not yet allocated
    std::string primaryIndexName;     // "" = no primary index
};

} // namespace minidb::catalog
