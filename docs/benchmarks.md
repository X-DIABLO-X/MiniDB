# MiniDB Benchmarks

We ship three standalone benchmark programs (not in ctest — they're for
the report, not for CI):

| File | Question it answers |
|---|---|
| `benchmark/read_benchmark.cpp` | How fast is point + range lookup? seq vs index. |
| `benchmark/write_benchmark.cpp` | How many `INSERT`s per second under 2PL vs MVCC? |
| `benchmark/join_benchmark.cpp`   | Nested-loop vs hash join on a 2-table workload. |

All benchmarks live in the `minidb::bench` namespace, share a small
harness (`include/bench/`) and emit CSV on stdout.

---

## 1. Harness

```cpp
// include/bench/harness.h
namespace minidb::bench {

struct Config {
    std::string dbPath;
    int scaleFactor;          // number of rows in the main table
    int warmupSeconds;
    int measureSeconds;
    int threadCount;          // 1 for sequential benchmarks
};

struct Result {
    std::string name;
    double      throughput;    // ops/sec
    double      p50LatencyUs;
    double      p95LatencyUs;
    double      p99LatencyUs;
};

Result run(const std::string& name, Config cfg,
           std::function<void(const Config&)> workload);

} // namespace minidb::bench
```

The harness:

1. Deletes the DB at `dbPath`.
2. Runs the workload to populate it.
3. Sleeps `warmupSeconds`.
4. Times `measureSeconds` of steady-state execution.
5. Emits a CSV row: `name,scaleFactor,threads,throughput,p50,p95,p99`.

---

## 2. Read benchmark

Workload: `SELECT * FROM t WHERE id = ?`

- Phase 1: insert `scaleFactor` rows.
- Phase 2: pick `measureSeconds * throughput_estimate` random ids.
- For each id, run the query and time `init + next + close`.

Two configurations:

- **seq** — uses `SeqScan + Filter`.
- **idx** — uses `IndexScan`.

Expected outcome: `idx` wins for `scaleFactor ≥ 1e4` and selectivity ≤ 1%.

---

## 3. Write benchmark (Track B report)

Workload: 4 writer threads, each doing `INSERT INTO t VALUES (...)`.

- Same schema, two runs:
  - 2PL baseline (`LockManager` enabled)
  - MVCC snapshot isolation
- For each run, vary contention by changing the fraction of writes that
  hit the same primary-key bucket.

Expected outcome: MVCC sustains throughput as contention rises, 2PL
collapses.

---

## 4. Join benchmark

Workload: `SELECT * FROM a JOIN b ON a.id = b.aid WHERE a.x < ?;`

- `a` is `scaleFactor` rows, `b` is `10 * scaleFactor` rows.
- Two configurations: nested-loop join, hash join.

---

## 5. How to run

```bash
cmake --build build --target bench_read_benchmark bench_write_benchmark bench_join_benchmark
./build/benchmarks/bench_read_benchmark   --scale 100000 --threads 1
./build/benchmarks/bench_write_benchmark  --scale 100000 --threads 4
./build/benchmarks/bench_join_benchmark   --scale 50000  --threads 1
```

---

## 6. What goes in the report

For each benchmark:

- **Setup** — hardware, OS, build type, scale factor, thread count.
- **Method** — exact SQL, exact workload generator, warmup & measurement
  durations.
- **Results** — the CSV rows + a chart.
- **Analysis** — does the result match the theory? If not, why?

The report itself lives at `docs/benchmark_report.md` (added at M5).
