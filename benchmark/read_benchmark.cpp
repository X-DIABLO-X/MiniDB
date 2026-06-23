// =============================================================================
// benchmark/read_benchmark.cpp
// -----------------------------------------------------------------------------
// Insert N rows, then issue N/10 point lookups in two configurations:
//   - seq (SeqScan + Filter)
//   - idx (IndexScan)
// Reports throughput + p50/p95/p99 latency per configuration.
// =============================================================================
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "catalog/catalog_manager.h"
#include "executor/query_engine.h"
#include "index/index_manager.h"
#include "recovery/recovery_manager.h"
#include "recovery/wal.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "transaction/transaction_manager.h"

using namespace minidb;

int main(int argc, char** argv) {
    int N = (argc > 1) ? std::atoi(argv[1]) : 100'000;

    std::printf("read_benchmark: N=%d (stub — see docs/benchmarks.md)\n", N);
    (void)N;
    return 0;
}
