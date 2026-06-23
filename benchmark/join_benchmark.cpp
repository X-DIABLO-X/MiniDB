// =============================================================================
// benchmark/join_benchmark.cpp
// -----------------------------------------------------------------------------
// Compares NestedLoopJoin vs HashJoin on a 2-table workload. See
// docs/benchmarks.md §4.
// =============================================================================
#include <cstdio>

int main(int argc, char** argv) {
    int scale = (argc > 1) ? std::atoi(argv[1]) : 50'000;
    std::printf("join_benchmark: scale=%d (stub — see docs/benchmarks.md)\n", scale);
    (void)scale;
    return 0;
}
