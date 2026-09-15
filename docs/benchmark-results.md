# Phase 4 benchmark results

These results describe one controlled run of ExchangeLab. They are evidence
for this machine and workload, not universal limits for C++, TCP, or the
locking primitives.

## Environment and method

- Apple M2 Pro, 10 logical CPUs, arm64.
- macOS 26.5.1, Darwin 25.5.0.
- Apple Clang 17.0.0, C++20, Release build.
- Source base `0b12dbc03003` with a clean working tree.
- 10 exchanges, 50,000 instruments, seed 42, and 64 stripes.
- Five repetitions per published scenario. Tables report the median result.
- Modes were rotated between repetitions to reduce fixed run-order bias.
- Workloads were generated before timing. Every compared mode received the
  same update stream, query IDs, dimensions, counts, ratio, and distribution.
- Direct suites used 5,000,000 measured operations after 500,000 warm-up
  operations.
- The TCP mode suite used 2,000,000 measured operations after 200,000 warm-up
  operations. The TCP worker suite used 500,000 after 50,000 warm-up
  operations.
- TCP tests used 10 persistent feed sockets and 16 persistent query clients.
- Query percentiles use nearest rank. Busy responses are not counted as
  successful low-latency queries.

The harness records raw repetitions in:

- [Direct locking](../results/phase4/direct-locking.csv)
- [Direct sorting](../results/phase4/direct-sorting.csv)
- [Direct worker scaling](../results/phase4/direct-workers.csv)
- [TCP runtime modes](../results/phase4/tcp-modes.csv)
- [TCP worker scaling](../results/phase4/tcp-workers.csv)

All 135 published rows recorded `correct=true`, matching logical and oracle
checksums, zero unexpected errors, zero rejected updates, and zero queue-full
responses.

## How to read the tables

Updates/sec and queries/sec are separate because an update and a sorted query
do different work. Direct latency measures only the engine query call. TCP
latency measures the complete client round trip, including the socket,
parsing, queueing, engine access, response formatting, and return write.

The p50 column describes the typical query. p95 and p99 show the slower tail.
The p99 result must be considered alongside throughput: adding workers can
raise aggregate capacity while making a small fraction of requests wait much
longer.

## Direct lock comparison

The concurrent modes use 10 ordered writer roles and four query readers. The
single-threaded baseline runs the identical logical workload serially and has
no worker pool.

| Workload | Mode | Updates/sec | Queries/sec | p50 us | p95 us | p99 us |
|---|---|---:|---:|---:|---:|---:|
| 90/10 uniform | single-threaded | 47,012,221 | 5,223,580 | 0.125 | 0.208 | 0.250 |
| 90/10 uniform | global-read | 1,310,093 | 145,566 | 0.209 | 167.167 | 584.833 |
| 90/10 uniform | striped-read | 4,884,393 | 542,710 | 0.333 | 3.500 | 41.375 |
| 50/50 uniform | single-threaded | 7,081,539 | 7,081,539 | 0.083 | 0.167 | 0.209 |
| 50/50 uniform | global-read | 851,604 | 851,604 | 0.250 | 12.250 | 57.750 |
| 50/50 uniform | striped-read | 3,328,332 | 3,328,332 | 0.209 | 0.750 | 25.916 |
| 50/50 hot | single-threaded | 6,253,370 | 6,253,370 | 0.084 | 0.208 | 0.250 |
| 50/50 hot | global-read | 892,598 | 892,598 | 0.209 | 11.375 | 60.042 |
| 50/50 hot | striped-read | 3,270,275 | 3,270,275 | 0.250 | 0.792 | 25.709 |

Striping is a clear improvement over the one global lock. In balanced uniform
traffic it raises each separate rate from about 0.85 million/sec to 3.33
million/sec and lowers p99 from 57.8 us to 25.9 us. The improvement remains
under hot traffic, although concentrated instruments still share locks.

The multithreaded modes do not beat the single-threaded direct baseline. Each
operation touches only a small dense slice of 10 prices, so thread scheduling,
atomics, sequence mutexes, and shared locks cost more than the available
parallel work saves on this machine. The project therefore does not claim that
Phase 3 scaled throughput above the Phase 2 baseline.

## Direct sorting comparison

Both modes use the same striped engine, 10 writers, four query readers, and 64
stripes.

| Workload | Sorting | Updates/sec | Queries/sec | p50 us | p95 us | p99 us |
|---|---|---:|---:|---:|---:|---:|
| 90/10 uniform | on read | 4,955,906 | 550,656 | 0.333 | 4.667 | 40.959 |
| 90/10 uniform | on write | 4,664,511 | 518,279 | 0.333 | 5.750 | 40.458 |
| 50/50 uniform | on read | 3,271,562 | 3,271,562 | 0.209 | 0.875 | 27.333 |
| 50/50 uniform | on write | 3,126,547 | 3,126,547 | 0.250 | 0.958 | 27.000 |
| 10/90 uniform | on read | 856,078 | 7,704,700 | 0.167 | 0.416 | 3.458 |
| 10/90 uniform | on write | 766,052 | 6,894,465 | 0.208 | 0.417 | 3.833 |
| 50/50 hot | on read | 3,325,731 | 3,325,731 | 0.250 | 0.834 | 25.750 |
| 50/50 hot | on write | 3,115,171 | 3,115,171 | 0.209 | 0.916 | 28.625 |

Sort-on-read has higher throughput at every tested ratio. Its advantage ranges
from about 4.6% in the balanced case to 11.8% in the 90%-query case. With only
10 prices, sorting the query's private copy is cheap. Maintaining and copying
a second dense ordered view on every accepted update does not repay its cost,
even in the query-heavy workload. Tail-latency differences are small and do
not consistently favor either sorting mode.

This result contradicts the provisional résumé wording that expected
sort-on-write to increase query capacity. That wording is not used.

## Worker-count comparison

| Level | Query workers | Updates/sec | Queries/sec | p50 us | p95 us | p99 us |
|---|---:|---:|---:|---:|---:|---:|
| Direct 50/50 | 1 | 2,731,301 | 2,731,301 | 0.084 | 0.417 | 3.541 |
| Direct 50/50 | 4 | 3,193,726 | 3,193,726 | 0.250 | 0.916 | 26.500 |
| Direct 50/50 | 8 | 3,158,446 | 3,158,446 | 0.333 | 5.083 | 53.500 |
| TCP 50/50 | 1 | 92,445 | 92,445 | 160.167 | 206.083 | 271.791 |
| TCP 50/50 | 4 | 80,245 | 80,245 | 185.750 | 235.792 | 301.375 |
| TCP 50/50 | 8 | 78,624 | 78,624 | 190.125 | 240.583 | 313.625 |

Four direct readers increase each rate by about 17% over one reader, but p99
rises from 3.5 us to 26.5 us. Eight readers reduce median throughput slightly
and push p99 to 53.5 us. In the complete TCP system, one query worker is best for both
throughput and latency. The query is too small to offset the extra scheduling
and contention, and the 10 feed threads already consume the machine's 10
logical CPUs.

## End-to-end TCP modes

This representative workload is 90% updates and 10% queries with uniform
traffic, 10 persistent feeds, and 16 persistent clients. Threaded modes use
four query workers.

| Runtime mode | Updates/sec | Queries/sec | p50 us | p95 us | p99 us |
|---|---:|---:|---:|---:|---:|
| single-threaded | 485,875 | 53,986 | 193.792 | 1,006.917 | 1,889.250 |
| global-read | 481,095 | 53,455 | 192.292 | 547.083 | 3,149.750 |
| striped-read | 468,872 | 52,097 | 191.625 | 495.708 | 3,023.833 |
| striped-write | 477,782 | 53,087 | 191.375 | 486.042 | 2,927.750 |

The single-threaded runtime has the highest median update and query rates and
the lowest median p99 in this TCP test. The threaded modes are within roughly
3.5% of its throughput, which suggests the loopback publisher, text response,
and client round trip dominate the small engine-level differences. Within the
threaded implementations, striped-write has the lowest median p99, about 7%
below the global lock, but the run-to-run p99 ranges overlap enough that this
should be presented as local evidence rather than a universal claim.

## Correctness and verification

The benchmark prepares an untimed Phase 1 `MarketState` oracle from the exact
update stream. Timed direct loops only confirm that a query returned; detailed
validation remains outside the timing boundary so benchmark bookkeeping does
not reduce the reported engine throughput. Each direct mode compares final
counters, checksum, and every instrument query with the oracle during its first
repetition. TCP runs classify success, busy, and error responses, compare final
counters and checksum, and run a full untimed TCP response comparison during
the first repetition of every case. Any failure marks the row incorrect and
prevents a successful benchmark exit.

Verification on this host:

- Debug: 47 of 47 tests passed.
- Fresh Release configure/build: 47 of 47 tests passed.
- UndefinedBehaviorSanitizer: 47 of 47 tests passed.
- Release three-process demo: 10 feeds connected, two persistent queries
  completed, and shutdown reported zero rejected update frames.
- ThreadSanitizer: compilation succeeds, but the Apple runtime exits with code
  139 before the focused test starts. No clean TSan runtime claim is made.
- AddressSanitizer: compilation succeeds, but the Apple runtime hangs before
  `main()`, including for `--help`. No clean ASan runtime claim is made.

## Checkpoint completion

- 4A: the small CLI harness, deterministic workload preparation, latency
  sampling, Release enforcement, and raw CSV schema are complete.
- 4B: direct locking, sorting, distribution, and 1/4/8-worker comparisons are
  complete.
- 4C: loopback TCP mode and 1/4/8-worker comparisons using the real server,
  feed protocol, query pool, and persistent clients are complete.
- 4D: Debug, Release, UBSan, three-process, oracle, and output validation are
  complete; the host-specific ASan and TSan runtime failures are documented.
- 4E: the README, this report, raw results, and evidence-based résumé wording
  are complete.

## Résumé evidence

These bullets reflect the measured results instead of the provisional
outcomes in the implementation plan:

- Built and benchmarked a C++20 market-data server sustaining a median 485,875
  updates/sec and 53,986 sorted queries/sec across 10 TCP feeds and 16
  persistent clients at 1.89 ms p99 query latency on an Apple M2 Pro.
- Improved balanced concurrent direct-engine capacity from 0.85 million to
  3.33 million updates/sec and the same query rate by replacing one global
  reader/writer lock with 64 striped locks, while reducing p99 from 57.8 us to
  25.9 us.
- Built a deterministic two-level benchmark harness with Phase 1 oracle
  checks, fixed workloads, warm-up, five repetitions, separate update/query
  rates, p50/p95/p99 latency, and reproducible CSV environment metadata.
