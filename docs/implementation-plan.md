# ExchangeLab learning and implementation plan

Status: proposed scope awaiting project-owner approval; Phase 1 not started  
Purpose: résumé project and guided introduction to C++ systems programming  
Active scope: five phases ending in a benchmarked, polished local project

The original ExchangeLab handoff describes the broader product vision. This shorter plan is the active implementation scope. Features excluded here are not commitments and should not be added unless the project owner deliberately revisits the scope after the core project is finished.

## 1. Project outcome

ExchangeLab will be a local C++20 program that:

- Simulates 10 exchanges publishing prices for 50,000 instruments.
- Stores the latest accepted price from every exchange.
- Answers queries for one instrument with prices sorted across exchanges.
- Validates per-exchange sequence numbers.
- Receives simulated feeds over localhost TCP.
- Supports concurrent feed ingestion and queries.
- Injects a small set of deterministic failures.
- Records and replays the delivered update stream.
- Compares a few simple implementation strategies with real measurements.

The goal is not production completeness. The goal is a finished, understandable systems project that demonstrates modern C++, networking, concurrency, correctness testing, and performance reasoning.

## 2. Learning is a first-class requirement

The project owner is learning C++, networking, and CMake while Codex implements the code. Completion speed is secondary to being able to understand and defend the finished system.

### How each phase should be taught

Before editing in a phase, Codex should explain:

1. What the phase adds and why it exists.
2. The relevant concepts in plain language.
3. The small modules that will be created or changed.
4. How data and control will flow between those modules.
5. Which choices are important and which are merely implementation details.

During implementation:

- Work in small, visible checkpoints rather than producing the whole phase in one unexplained patch.
- Introduce files only when they are needed.
- Explain every new CMake target, library, dependency, thread, lock, socket, and binary format before or as it appears.
- Show representative code paths and connect them to the higher-level design.
- Prefer readable, direct C++ over clever abstractions.
- Add comments for invariants and non-obvious behavior, not comments that simply restate syntax.
- Explain compiler or test failures and what the fix teaches.
- Do not expect the project owner to write code or already know C++ terminology.
- Pause for questions at natural module boundaries when useful, without turning the process into a quiz.

At the end of each phase, Codex should provide:

- A modular walkthrough of every file added or materially changed.
- An end-to-end trace of one representative operation.
- A short glossary of new C++/systems concepts.
- The exact build, run, and test commands, with an explanation of what each command does.
- Actual verification results.
- The important tradeoffs and limitations.
- A concise progress update in this document.

### Session model

Use one fresh session per phase as the target. A phase may use additional sessions if tests are failing or the learning walkthrough needs more room. Do not rush into the next phase merely to keep the five-session target.

At the start of a new session, read:

- This plan
- The current source tree
- The `Current progress` section below
- The relevant build/test output from the repository, if any

At the end of a phase, update only the `Current progress` section. Separate handoff documents and architecture-decision logs are not required.

## 3. Simplicity rules

- Build only the current phase. Do not create placeholder files, commands, or interfaces for later phases.
- Add an abstraction only when two real implementations need it.
- Prefer a concrete class and a direct function call over factories, plugin systems, or deep inheritance.
- Prefer standard-library facilities when they are sufficient.
- Keep configuration to a few documented command-line arguments.
- Use dense arrays because exchange and instrument IDs are bounded and contiguous.
- Keep logs and measurements off hot paths when benchmarking.
- Do not claim performance before measuring it.
- Stop and discuss any proposed feature that is not listed in the five phases.

## 4. Core semantics

These rules remain stable throughout the project.

### Data model

- C++20 with fixed-width integer types at data boundaries.
- Exchange IDs and instrument IDs are dense and zero-based internally.
- Prices use `std::int64_t` fixed-point micros instead of floating point.
- Default dimensions are 10 exchanges and 50,000 instruments.
- Each exchange/instrument entry stores price, last sequence number, source timestamp, and validity.

### Sequence handling

Sequence numbers are monotonic per exchange feed, not per instrument.

- A sequence greater than the last accepted sequence is accepted.
- An equal sequence is a duplicate and does not change state.
- A lower sequence is stale and does not change state.
- A jump greater than one is accepted and increments a sequence-gap counter.
- Invalid IDs or prices do not change either price state or the last accepted sequence.

### Query handling

- A query returns every exchange with a valid price for the requested instrument.
- Results are sorted by ascending price, then ascending exchange ID.
- Exchanges without a valid price are reported explicitly.
- Once concurrency exists, each result must be copied from a state protected by the appropriate lock. Sorting may happen after releasing the lock if the copied values are already independent.

### Determinism

- Simulation uses `std::mt19937_64` with an explicit seed.
- The same implementation, seed, and arguments must generate the same logical update stream.
- The final-state checksum covers stable logical state and excludes wall-clock receive timing.
- Replay equivalence means the same engine acceptance counters and final-state checksum. It does not attempt to reproduce network timing.

## 5. Phase overview

| Phase | Status | Main result |
|---:|---|---|
| 1 — Reference engine | Not started | Correct single-threaded simulation and queries |
| 2 — TCP feeds | Not started | Ten simple localhost binary feeds |
| 3 — Concurrency | Not started | Global and striped locking plus sorting comparison |
| 4 — Faults and replay | Not started | Reproducible failures and matching replay |
| 5 — Benchmarks and polish | Not started | Measured results and portfolio-ready documentation |

## 6. Phase 1 — Single-threaded reference engine

### Goal

Build the smallest correct solution to the original market-data problem without networking or concurrency.

### Concepts to teach

- What source files, header files, object files, libraries, and executables are.
- What CMake does during configure, generate, build, and test steps.
- Basic C++ value types, structs, classes, vectors, references, and ownership.
- Why fixed-point integers are preferable to floating point for prices.
- Why dense indexing is simpler and more cache-friendly than nested maps here.
- How deterministic pseudorandom simulation works.
- How unit tests protect behavioral rules.

### Implementation checkpoints

#### 1A — Minimal toolchain

- Create only the directories and files required by Phase 1.
- Add a small top-level `CMakeLists.txt`.
- Build one core library, one CLI executable, and one test executable.
- Use GoogleTest if dependency setup remains straightforward.
- Enable useful GCC/Clang warnings.
- Document the configure, build, test, and run commands.

Learning checkpoint: explain how a `.cpp` file becomes an executable and how CMake targets connect the pieces.

#### 1B — Domain model and dense state

- Add simple structs for `MarketUpdate`, stored price entries, query entries, query results, and counters.
- Implement one concrete `MarketState` class.
- Store entries in one dense vector using:

```text
index = instrument_id * exchange_count + exchange_id
```

- Store the last accepted sequence once per exchange.
- Validate IDs, prices, and sequence rules before mutation.

Learning checkpoint: trace the memory lookup for one instrument/exchange pair and explain the difference between logical state and object layout.

#### 1C — Queries and checksum

- Copy valid prices for one instrument into a result.
- Record missing exchanges.
- Sort by price and then exchange ID.
- Calculate a stable checksum of the logical final state.

Learning checkpoint: walk from `query(instrument_id)` through dense indexing, copying, sorting, and output.

#### 1D — Seeded simulator and demo

- Use `std::mt19937_64` with an explicit seed.
- Generate per-exchange sequence numbers.
- Generate bounded random-walk prices.
- Support uniform and hot-instrument selection with a small option.
- Add basic CLI arguments for seed, event count, dimensions, distribution, and query instrument.
- Print the selected query, acceptance counters, and checksum.

Learning checkpoint: follow one simulated update from random generation through validation into stored state.

#### 1E — Focused tests

- Valid update and replacement.
- Duplicate, stale, and sequence-gap behavior.
- Invalid exchange, instrument, and price behavior.
- Stable price/exchange sorting.
- Missing exchanges.
- Known-seed simulator output.
- Two identical runs produce identical counters, selected query, and checksum.
- Default 10-exchange/50,000-instrument run.

### Phase 1 exit condition

- The project configures, builds, tests, and runs from documented commands.
- The default-scale simulation works correctly.
- A repeated seeded run produces the same result.
- The project owner has received a modular walkthrough of the build system and Phase 1 execution flow.
- No networking, locks, future engine interfaces, or placeholder commands exist.

## 7. Phase 2 — Simple TCP feeds

### Goal

Send simulated exchange updates over ten localhost TCP connections and decode them safely.

### Concepts to teach

- What TCP guarantees and what it does not preserve about application messages.
- Why one send does not necessarily equal one receive.
- Sockets, connections, ports, buffers, and asynchronous callbacks.
- Network byte order and explicit binary encoding.
- Object lifetime during asynchronous operations.

### Implementation checkpoints

#### 2A — Small binary message format

- Define one fixed-size update frame containing protocol version, exchange ID, instrument ID, price, sequence, and source timestamp.
- Encode and decode each field explicitly in network byte order.
- Do not serialize raw C++ structs.
- Keep the format documented beside the codec.

There will be no CRC, magic bytes, extensible type system, or forward-compatibility framework.

Learning checkpoint: inspect the bytes for one example update and reconstruct its fields manually.

#### 2B — Framing buffer

- Accumulate received bytes in a reusable buffer.
- Decode a frame only when all fixed-size bytes are present.
- Preserve an incomplete tail for the next read.
- Decode several complete frames from one read.
- Reject invalid decoded fields before updating market state.

Learning checkpoint: demonstrate one frame split across reads and two frames combined in one read.

#### 2C — Localhost transport

- Add Standalone Asio.
- Run one local receiver and ten simulated exchange connections.
- Initially run socket callbacks on one event-loop thread so `MarketState` remains single-threaded.
- Connect decoded updates to the Phase 1 update path.
- Support clean connection close and application shutdown.

Learning checkpoint: trace an update from an exchange simulator, through the kernel socket buffers and decoder, into market state.

#### 2D — Tests

- Byte-exact encode/decode round trip.
- Every useful frame split boundary.
- Multiple frames in one receive buffer.
- Invalid version, IDs, and price.
- Truncated connection input.
- Ten simultaneous localhost connections.
- TCP and in-process paths produce the same result from the same logical updates.
- Shutdown completes without hanging.

### Phase 2 exit condition

- Ten feeds stream concurrently over localhost.
- Fragmented and combined reads work correctly.
- Invalid network data cannot mutate market state.
- The project owner can explain why TCP requires application-level framing and how the asynchronous flow is structured.

## 8. Phase 3 — Concurrency and strategy comparison

### Goal

Allow feed updates and queries to execute on multiple threads, using understandable locks and only the implementations required for the final comparison.

### Concepts to teach

- Threads, data races, mutexes, shared mutexes, and critical sections.
- The difference between correctness and performance under concurrency.
- Lock granularity and contention.
- Why consistent lock ordering matters.
- Sort-on-read versus sort-on-write tradeoffs.
- What ThreadSanitizer can and cannot establish.

### Implementation checkpoints

#### 3A — Small engine interface

- Introduce an engine interface only now that several real implementations are required.
- Adapt the Phase 1 implementation as the behavioral reference.
- Keep update, query, counters, and checksum operations small and explicit.

Learning checkpoint: explain why the interface is useful now and why it was unnecessary in Phase 1.

#### 3B — Global-lock baseline

- Implement one mutex protecting update, query-copy, and state traversal.
- Release the lock after a query has copied its values.
- Use this as the simplest correct concurrent baseline.

Learning checkpoint: draw two writers and one reader contending for the same global lock.

#### 3C — Striped-lock implementation

- Divide instruments across a modest fixed number of locks.
- Protect per-exchange sequence acceptance without using one global state lock.
- Use one consistent lock-acquisition order.
- Let unrelated instrument stripes proceed concurrently.
- Keep queries coherent by copying under the selected stripe lock.

Learning checkpoint: compare contention when two operations target the same stripe versus different stripes.

#### 3D — Sorting modes

- `sort-on-read`: store by exchange and sort the copied query result.
- `sort-on-write`: maintain a sorted fixed-size view while holding the instrument stripe lock.
- Support these modes in the striped implementation without creating several additional engine architectures.

Learning checkpoint: count the extra work paid by updates and queries in each mode.

#### 3E — Concurrent runtime and tests

- Run network processing on multiple workers.
- Add one or more concurrent query workers.
- Compare all implementations against the single-threaded reference on controlled streams.
- Stress a hot set where writers and readers collide frequently.
- Run ThreadSanitizer on focused concurrency tests.
- Test shutdown while work is active.

### Phase 3 exit condition

- Global-lock and striped-lock engines produce the same logical final state as the reference.
- Queries remain coherent and sorted under load.
- Focused ThreadSanitizer tests report no project data races.
- The project owner can explain the lock boundaries and expected performance tradeoffs.
- No optimistic, atomic-snapshot, lock-free, or additional engine implementation exists.

## 9. Phase 4 — Deterministic faults and simple replay

### Goal

Demonstrate reproducible failures and show that the delivered event stream can be replayed to the same logical result.

### Concepts to teach

- Fault injection and why seeded failures are easier to debug.
- How drops, duplicates, and reordering interact with sequence numbers.
- The difference between reproducing logical input and reproducing network timing.
- Simple binary file headers and fixed-size records.
- File I/O failure and truncation handling.

### Implementation checkpoints

#### 4A — Seeded fault injector

- Use one `std::mt19937_64` fault generator initialized from the run seed.
- Support configurable drop, duplicate, reorder, and disconnect probabilities.
- Implement reordering with a small bounded buffer.
- Use one simple reconnect behavior after injected disconnects.
- Print the seed and fault settings with the run summary.

There will be no corruption, artificial delay, truncation injection, burst mode, independent random stream per fault, or simulated deterministic clock.

Learning checkpoint: show the original and faulted order for a short hand-readable event sequence.

#### 4B — Simple recording

- Record updates at the receiver immediately before applying them to the engine.
- Serialize recorder writes with one ordinary mutex.
- Write a small header containing format version and configured dimensions.
- Append fixed-size logical update records.
- Stop the run with a clear error if recording fails.

There will be no background recorder, connection-event records, fault-decision records, CRC, crash-safe trailer, or backpressure protocol.

Learning checkpoint: inspect a tiny recording and map its bytes back to its update fields.

#### 4C — Replay

- Validate the recording header.
- Read fixed-size updates in recorded order.
- Reject truncated records cleanly.
- Apply updates directly to a selected engine without TCP timing.
- Compare engine acceptance counters and final-state checksum with the recorded live run.

Learning checkpoint: trace why the replay reaches the same state even though it does not recreate connection timing.

#### 4D — Tests

- One focused test for each fault.
- Same seed and arguments produce the same faulted logical stream.
- A different seed produces a different valid stream.
- Recording round trip.
- Truncated or invalid recording rejection.
- Recorded live run and replay have identical engine counters and checksum.
- Disconnect and reconnect integration test.

### Phase 4 exit condition

- Drop, duplicate, reorder, and disconnect faults work and are reproducible.
- Recorded delivered input replays to the same logical outcome.
- The project owner understands what replay proves and what it intentionally does not reproduce.

## 10. Phase 5 — Benchmarks and portfolio polish

### Goal

Measure the important design tradeoffs honestly and turn the completed implementation into a concise portfolio project.

### Concepts to teach

- Throughput versus latency.
- Median and tail percentiles.
- Warm-up, repeated runs, and benchmark noise.
- Why debug and sanitizer builds are not performance builds.
- How workload distribution changes contention.
- How to turn measurements into an engineering conclusion.

### Implementation checkpoints

#### 5A — Small benchmark harness

- Use a custom end-to-end harness built around `std::chrono::steady_clock`.
- Collect latency in thread-local vectors and merge after the measured run.
- Report update/query throughput and p50, p95, and p99 latency.
- Emit a simple CSV file.
- Store compiler, build type, hardware, thread counts, workload arguments, seed, and Git revision beside results.
- Keep result calculation and logging outside the measured operation path.

Google Benchmark, custom histogram libraries, and advanced coordinated-omission machinery are not required.

Learning checkpoint: calculate a percentile from a small latency sample and explain what p99 does and does not mean.

#### 5B — Required comparisons

Compare only:

- Global lock with sort on read.
- Striped locks with sort on read.
- Striped locks with sort on write.

Run at least:

- Update-heavy uniform traffic.
- Query-heavy uniform traffic.
- Mixed hot-instrument traffic.
- An optional ingestion-only ceiling if it adds a useful conclusion.
- Five repetitions for each final published scenario.

Learning checkpoint: form a hypothesis before each comparison, then compare it with the measured result.

#### 5C — Final correctness pass

- Focused unit and integration suite.
- AddressSanitizer and UndefinedBehaviorSanitizer build.
- ThreadSanitizer concurrency tests.
- Clean build and demo from documented commands.
- Repeatable record/replay example.

#### 5D — README and résumé evidence

- Explain what ExchangeLab does and why it was built.
- Include one compact architecture diagram.
- Explain sequence and query consistency semantics.
- Provide a quick start that has been run from a clean build directory.
- Include one measured results table and hardware/compiler context.
- Explain when the measured strategies win or lose.
- Document limitations without presenting excluded production features as defects.
- Write two résumé bullets using measured values only.

Learning checkpoint: conduct a final interview-style walkthrough of the architecture, one correctness decision, one concurrency tradeoff, and one benchmark conclusion.

### Phase 5 exit condition

- The complete project builds, tests, demos, records, and replays from documented commands.
- Final results include real context and repeated runs.
- No metric is invented or borrowed from an unrepresentative test.
- The README is concise enough for a recruiter and detailed enough for a technical interviewer.
- The project owner can explain the system at a modular level and defend its main design choices.

## 11. Explicitly excluded scope

Do not plan or implement these unless the core project is complete and the project owner starts a new scope discussion:

- Optimistic or lock-free snapshot engines
- Formal atomic memory-ordering proofs
- More than the reference, global-lock, and striped-lock approaches
- Custom pseudorandom generators
- Production-grade protocol versioning, CRC, TLS, or recovery protocols
- Fault corruption, artificial delay, truncation, and burst injection
- Timed replay, crash-safe recording, or recorder backpressure systems
- UDP multicast
- Order books or matching
- NUMA and CPU affinity
- Custom allocators or zero-copy designs
- Rust comparisons
- Live dashboards
- Cloud deployment
- User accounts, databases, Kafka, Kubernetes, or real trading integrations

These ideas are not rejected forever. They are excluded because they add much less résumé value than completing, measuring, understanding, and presenting the five-phase project.

## 12. Current progress

- Simplified five-phase plan proposed and recorded for review.
- Phase 1 has not started.
- No C++ source, CMake project, placeholder interface, or future-phase scaffold has been created.
- Next session: explain the Phase 1 module plan and CMake/C++ foundations, then implement Phase 1 in learning checkpoints only after the project owner asks to begin.

## 13. Core definition of done

ExchangeLab is finished when:

- It handles 10 exchanges and 50,000 instruments using dense state.
- Fixed-point prices, per-exchange sequences, missing values, and sorted queries are correct.
- Ten localhost TCP feeds handle fragmented and combined reads.
- Global and striped locking support concurrent updates and queries correctly.
- Sort-on-read and sort-on-write have been compared.
- Drop, duplicate, reorder, and disconnect faults are reproducible from a seed.
- A simple recording replays to the same counters and state checksum.
- Unit, integration, sanitizer, and concurrency tests pass on documented environments.
- Benchmarks report throughput and p50/p95/p99 with workload and machine context.
- The README contains verified commands, measured results, tradeoffs, and limitations.
- The project owner has received a modular walkthrough of every phase and can explain the system rather than merely possessing completed code.
