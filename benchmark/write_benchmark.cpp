// =============================================================================
// benchmark/write_benchmark.cpp
// -----------------------------------------------------------------------------
// 4 writer threads issuing INSERTs. Compares 2PL baseline vs MVCC for
// the Track B report. See docs/benchmarks.md §3.
// =============================================================================
#include <cstdio>

int main(int argc, char** argv) {
    int threads = (argc > 1) ? std::atoi(argv[1]) : 4;
    std::printf("write_benchmark: threads=%d (stub — see docs/benchmarks.md)\n", threads);
    (void)threads;
    return 0;
}
