# ExchangeLab learning and implementation plan

Status: approved four-phase scope; Phases 1 and 2 complete; Phase 3 not started

Purpose: résumé project based on the Optiver market-data design problem and a guided introduction to C++ systems programming

Active scope: four phases ending in a continuously running interactive client/server system and a focused benchmark laboratory

The original ExchangeLab handoff describes a broader product vision. This document is the active implementation scope. Anything excluded here is not a commitment and must not be added unless the project owner deliberately approves a new scope after the core project is complete.

## 1. Project outcome

ExchangeLab will be a local C++20 system that:

- Runs a market-data server continuously until interrupted.
- Simulates 10 exchanges publishing prices for 50,000 instruments over 10 logical localhost TCP feeds.
- Stores the latest accepted price from every exchange in the existing dense market-state representation.
- Validates monotonically increasing sequence numbers independently for each exchange.
- Serves multiple query clients on a separate TCP port while updates continue.
- Lets an interactive terminal client repeatedly request one instrument and receive its latest cross-exchange prices sorted by price and then exchange ID.
- Handles ordinary publisher and client disconnects, reconnects, and clean shutdown.
- Compares global and striped reader/writer locking.
- Compares sort-on-read and sort-on-write using the same dense representation.
- Measures direct engine behavior and one representative end-to-end TCP path.
- Reports actual throughput and latency results with enough context to reproduce and defend them.

The goal is not production completeness. The goal is a finished, understandable system that demonstrates modern C++, CMake, TCP networking, concurrent server design, correctness testing, and evidence-based performance reasoning.

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
- Explain every new CMake target, dependency, socket, thread, mutex, shared mutex, condition variable, queue, and protocol before or as it appears.
- Show representative code paths and connect them to the higher-level design.
- Prefer readable, direct C++ over clever abstractions.
- Add comments for invariants and non-obvious behavior, not comments that simply restate syntax.
- Explain compiler or test failures and what the fix teaches.
- Do not expect the project owner to write code or already know C++ terminology.
- Pause for questions at natural module boundaries when useful, without turning the process into a quiz.

At the end of each phase, Codex should provide:

- A modular walkthrough of every file added or materially changed.
- An end-to-end trace of one representative operation.
- A short glossary of new C++ and systems concepts.
- The exact build, run, and test commands, with an explanation of what each command does.
- Actual verification results.
- The important tradeoffs and limitations.
- A concise progress update in this document.

### Session model

Use one fresh session per phase as the target. A phase may use additional sessions if the implementation, testing, or learning walkthrough needs more room. Do not rush into the next phase merely to fit a session target.

At the start of a new session, read:

- This plan.
- The current source tree.
- The `Current progress` section below.
- Relevant build and test output in the repository, if any.

At the end of a phase, update only the `Current progress` section unless implementation discoveries require an explicitly approved plan correction. Separate handoff documents and architecture-decision logs are not required.

## 3. Simplicity rules

- Implement only the current phase.
- Do not create placeholder files, commands, interfaces, or directories for later phases.
- Add an abstraction only when two real implementations require it.
- Prefer a concrete class and direct function calls over factories, plugin systems, or deep inheritance.
- Prefer standard-library facilities when they are sufficient.
- Keep configuration to documented command-line arguments that serve a demonstrated use case.
- Preserve the completed Phase 1 code unless a later requirement makes a focused change necessary.
- Keep the existing dense market-state representation for every engine and sorting strategy.
- Do not add a hash-based engine merely to manufacture a benchmark comparison.
- Keep logging and result collection outside measured hot paths where practical.
- Do not add dependencies or features merely for résumé keywords.
- Do not claim performance before measuring it.
- Stop and ask before adding anything outside these four phases.

## 4. Stable semantics and selected design

These rules remain stable throughout the project.

### Data model

- Use C++20 and fixed-width integer types at data and protocol boundaries.
- Exchange IDs and instrument IDs are dense, bounded, and zero-based internally.
- Prices use `std::int64_t` fixed-point micros instead of floating point.
- Default dimensions are 10 exchanges and 50,000 instruments.
- Each exchange/instrument entry stores price, the sequence that produced that price, source timestamp, and validity.
- Per-exchange sequence state records the last accepted sequence number from each feed.

### Dense market state

The selected representation is the existing contiguous vector indexed by:

```text
index = instrument_id * exchange_count + exchange_id
```

Dense indexing is the chosen design because the ID ranges are known, compact, and contiguous. It provides constant-time address calculation, contiguous storage, predictable iteration, and good cache locality. It avoids hashing, buckets, node allocations, and pointer chasing. The README and learning walkthrough must explain these advantages, but the project will not build or benchmark a hash-based alternative solely to prove the selection.

### Sequence handling

Sequence numbers are monotonic per exchange feed, not per instrument.

- A sequence greater than the last accepted sequence is accepted.
- An equal sequence is a duplicate and does not change state.
- A lower sequence is stale or out of order and does not change state.
- A jump greater than one is accepted and increments a sequence-gap counter.
- Invalid IDs or prices do not change either price state or the last accepted sequence.
- Phase 3 feed ownership must preserve the received order within each exchange.

Ordinary correctness tests for duplicate, stale, out-of-order, and gapped sequences remain required. They are tests of feed correctness, not configurable failure injection.

### Query handling

- A query returns every exchange with a valid price for the requested instrument.
- Results are sorted by ascending price and then ascending exchange ID.
- Exchanges without a valid price are reported explicitly.
- Once concurrency exists, a query copies its instrument state while holding the appropriate shared lock.
- With sort-on-read, the query releases the lock before sorting the independent copy.
- A query must never observe a partially written price entry.

### Deterministic finite runs and checksum

- Simulation uses `std::mt19937_64` with an explicit seed.
- The same implementation, seed, dimensions, distribution, and finite event count must generate the same logical update stream.
- The logical final-state checksum from Phase 1 remains available for correctness comparisons.
- The checksum covers stable logical state and excludes wall-clock or socket timing.
- Controlled finite streams are used to compare later engines with the Phase 1 reference result.

### Ordinary connection behavior

- Publishers and query clients may disconnect and reconnect without stopping the server.
- Reconnection behavior should be simple and understandable, such as a fixed retry interval where automatic retry is useful.
- Clean EOF, socket errors, and Ctrl+C shutdown must not hang or leave callbacks referring to destroyed objects.

## 5. Phase overview

| Phase | Status | Main result |
|---:|---|---|
| 1 — Reference engine | Complete | Correct single-threaded simulation, dense state, and sorted queries |
| 2 — Continuous TCP system | Not started | Independent server, exchange simulator, and interactive query client |
| 3 — Multithreading and strategies | Not started | Dedicated feed threads, bounded query workers, locking, and sorting variants |
| 4 — Benchmarks and polish | Not started | Focused measurements, final verification, documentation, and résumé evidence |

## 6. Phase 1 — Single-threaded reference engine

### Goal

Build the smallest correct solution to the original market-data problem without networking or concurrency.

Phase 1 is complete. Do not unnecessarily refactor or rewrite it while implementing later phases.

### Concepts taught

- What source files, header files, object files, libraries, and executables are.
- What CMake does during configure, generate, build, and test steps.
- Basic C++ value types, structs, classes, vectors, references, and ownership.
- Why fixed-point integers are preferable to floating point for prices.
- Why dense indexing is simpler and more cache-friendly than nested maps here.
- How deterministic pseudorandom simulation works.
- How unit tests protect behavioral rules.

### Completed implementation checkpoints

#### 1A — Minimal toolchain

- Created only the directories and files required by Phase 1.
- Added a small top-level `CMakeLists.txt`.
- Built one core library, one CLI executable, and one test executable.
- Used GoogleTest with straightforward dependency setup.
- Enabled useful GCC and Clang warnings.
- Documented configure, build, test, and run commands.

#### 1B — Domain model and dense state

- Added simple structs for `MarketUpdate`, stored price entries, query entries, query results, and counters.
- Implemented one concrete `MarketState` class.
- Stored entries in one dense vector using the selected indexing formula.
- Stored the last accepted sequence once per exchange.
- Validated IDs, prices, and sequence rules before mutation.

#### 1C — Queries and checksum

- Copied valid prices for one instrument into a result.
- Reported missing exchanges.
- Sorted by price and then exchange ID.
- Calculated a stable checksum of the logical final state.

#### 1D — Seeded simulator and demo

- Used `std::mt19937_64` with an explicit seed.
- Generated per-exchange sequence numbers.
- Generated bounded random-walk prices.
- Supported uniform and hot-instrument selection.
- Added CLI arguments for seed, event count, dimensions, distribution, and query instrument.
- Printed the selected query, acceptance counters, and checksum.

#### 1E — Focused tests

- Valid update and replacement.
- Duplicate, stale, out-of-order, and sequence-gap behavior.
- Invalid exchange, instrument, and price behavior.
- Stable price/exchange sorting.
- Missing exchanges.
- Known-seed simulator output.
- Two identical runs produce identical counters, selected query, and checksum.
- Default 10-exchange/50,000-instrument run.

### Phase 1 exit condition — complete

- The project configures, builds, tests, and runs from documented commands.
- The default-scale simulation works correctly.
- A repeated seeded run produces the same result.
- The project owner received a modular walkthrough of the build system and Phase 1 execution flow.
- Phase 1 contains no networking, locks, future engine interfaces, or placeholder commands.

## 7. Phase 2 — Continuous TCP server, exchange simulator, and interactive client

### Goal

Replace the one-shot networking idea with three independently runnable processes that form a continuously running, interactive local system.

### Concepts to teach

- Processes, localhost, IP addresses, ports, clients, servers, sockets, and connections.
- What TCP guarantees and what it does not preserve about application messages.
- Why one send does not necessarily equal one receive.
- Bytes, binary encoding, network byte order, framing buffers, and text protocols.
- Event loops, asynchronous callbacks, and object lifetime.
- The difference between a long-running service and a one-shot program.
- Signals, Ctrl+C, orderly shutdown, disconnects, and reconnects.

### Required runnable components

#### `exchangelab-server`

- Runs as a separate process until interrupted with Ctrl+C.
- Owns the current `MarketState`.
- Listens for exchange price feeds on one TCP port.
- Listens for query clients on a separate TCP port.
- Processes exchange updates continuously while serving queries.
- Supports 10 simultaneous logical exchange connections.
- Supports multiple query-client connections.
- Allows publishers and clients to disconnect and reconnect.
- Shuts down cleanly without hangs or invalid asynchronous object lifetimes.
- Initially uses one Asio event-loop thread so the existing `MarketState` remains single-threaded during Phase 2.

#### `exchange-simulator`

- Runs independently from the server and client.
- Opens 10 logical TCP exchange feeds by default.
- Generates updates across 50,000 instruments by default.
- Maintains an independent monotonically increasing sequence number for each exchange.
- Supports a configurable update rate.
- Supports `--continuous`, which publishes until interrupted.
- Supports finite `--events N` mode with an explicit seed for deterministic tests and later benchmarks.
- Supports uniform and hot-instrument distributions.
- Uses simple ordinary reconnect behavior if a server connection is lost.

#### `exchangelab-client`

- Runs independently from the server and simulator.
- Connects to the server's query port.
- Provides a simple interactive terminal loop.
- Lets the user repeatedly query instrument IDs while updates continue.
- Prints the latest valid price from each exchange sorted by ascending price and then exchange ID.
- Clearly reports exchanges that do not yet have a valid price.
- Can disconnect and reconnect without stopping the server.

### Protocols

#### Binary exchange-update protocol

- Use one small fixed-size frame.
- Include only protocol version, exchange ID, instrument ID, fixed-point price, sequence number, and source timestamp.
- Encode and decode every field explicitly in network byte order.
- Do not transmit raw C++ structs.
- Document the byte layout beside the codec.
- Reject an invalid version, ID, or price without mutating market state.
- Do not add CRCs, encryption, extensible message types, or production protocol machinery.

#### Text query protocol

- Use a simple newline-delimited request such as `QUERY 1234\n`.
- Use a straightforward text response that clearly delimits one complete query result.
- Bound input length and reject malformed commands cleanly.
- Keep the protocol human-readable and intentionally small.

### Implementation checkpoints

#### 2A — Processes, CMake targets, and lifecycle

- Add Standalone Asio as the networking dependency.
- Add only the three required executable targets and the concrete shared modules they currently need.
- Define the two server ports and the minimum command-line options.
- Establish continuous run and Ctrl+C shutdown behavior.
- Explain which objects own the event loop, acceptors, sockets, and market state.

Learning checkpoint: draw the three processes, two server ports, and 10 logical feed connections, then distinguish a process, thread, socket, and connection.

#### 2B — Binary update codec

- Implement byte-exact encoding and decoding for the fixed-size update frame.
- Use explicit network-byte-order conversion for every multi-byte field.
- Keep validation separate enough to test without a live socket.

Learning checkpoint: encode one known update into bytes and reconstruct every field manually.

#### 2C — TCP framing buffer

- Accumulate received bytes in a reusable buffer.
- Decode a frame only when all fixed-size bytes are present.
- Preserve an incomplete tail for the next read.
- Decode several complete frames from one read.
- Pass only successfully decoded and validated updates to the Phase 1 update path.

Learning checkpoint: demonstrate a frame split across several reads, several frames in one read, and one complete frame followed by a partial frame.

#### 2D — Continuous server

- Add separate acceptors for feed and query ports.
- Maintain feed and query connection objects with explicit lifetime ownership.
- Apply accepted feed updates to the existing `MarketState` on the single event-loop thread.
- Parse newline-delimited query commands and return formatted query results.
- Continue accepting connections after an ordinary peer disconnect.
- Stop acceptors, sockets, callbacks, and the event loop cleanly on Ctrl+C.

Learning checkpoint: trace an update from the simulator through kernel socket buffers, framing, decoding, validation, and dense storage, then trace a query through the other port.

#### 2E — Continuous and finite exchange simulator

- Reuse the Phase 1 seeded generation rules where practical.
- Maintain one connection and sequence counter for each logical exchange.
- Rate-limit continuous publication using a simple understandable mechanism.
- Preserve deterministic logical update generation in finite `--events N` mode.
- Support uniform and hot-instrument distributions.
- Implement ordinary disconnect and reconnect behavior without configurable failure scenarios.

Learning checkpoint: compare a deterministic finite run with a rate-controlled continuous run and explain why network timing is not deterministic.

#### 2F — Interactive query client

- Read repeated terminal commands without restarting the client.
- Send one newline-delimited query at a time over a persistent connection.
- Read and print one complete server response.
- Report invalid commands and connection loss clearly.
- Permit a normal reconnect or clean restart without affecting the server.

Learning checkpoint: query the same instrument several times while updates continue and trace why the displayed prices can change.

#### 2G — Tests and three-terminal demonstration

- Byte-exact update encode/decode round trip.
- Every useful fixed-frame split boundary.
- Multiple frames in one receive buffer.
- One complete frame followed by a partial frame.
- Invalid protocol version, exchange ID, instrument ID, and price.
- Malformed and oversized query commands.
- Ten simultaneous logical exchange connections.
- Multiple query-client connections.
- TCP and in-process paths produce the same result from the same finite logical updates.
- Publisher and client disconnect/reconnect integration tests.
- Clean shutdown tests with active sockets.

The expected demonstration uses three terminals:

```bash
# Terminal 1
./exchangelab-server

# Terminal 2
./exchange-simulator \
  --continuous \
  --exchanges 10 \
  --instruments 50000 \
  --rate 100000 \
  --seed 42

# Terminal 3
./exchangelab-client
> query 1234
```

Repeated queries may return different prices because updates continue in the background.

### Phase 2 exit condition

- All three programs build and run independently.
- The server and simulator remain active until interrupted.
- The simulator maintains 10 logical exchange connections and continuous per-exchange sequences.
- The server accepts updates and serves multiple clients on separate ports.
- The client repeatedly queries changing live prices.
- Fragmented and combined TCP reads work correctly.
- Invalid network data cannot mutate market state.
- Publishers and clients can disconnect and reconnect.
- Ctrl+C shutdown completes without hangs or invalid object lifetimes.
- The project owner can explain both protocols and trace an update and query end to end.

## 8. Phase 3 — Multithreading, locking, and sorting strategies

### Goal

Introduce deliberate concurrent access using an explicit 10-feed architecture, a bounded query-worker pool, two understandable locking designs, and two sorting strategies.

### Concepts to teach

- Threads, thread ownership, data races, critical sections, and happens-before relationships.
- Ordinary mutexes, reader/writer locks, condition variables, and protected resources.
- Exclusive versus shared access.
- Task queues, bounded capacity, backpressure, and fixed worker pools.
- Lock granularity, contention, consistent lock ordering, and coherent snapshots.
- Sort-on-read versus sort-on-write.
- What ThreadSanitizer can and cannot establish.

### Selected server thread architecture

#### Exchange feeds

- Use one long-running feed-processing thread for each of the 10 exchange connections.
- Each feed thread receives and decodes updates from its assigned exchange.
- Each feed thread preserves received order within that exchange.
- Each feed thread applies valid updates to the shared market state.
- Do not create a new thread for each update.
- Make single-writer ownership of each exchange's sequence state explicit and aggregate counters safely.

This architecture is selected because the number of exchange feeds is fixed and small, and per-exchange thread ownership is easy to understand and defend.

#### Query clients

- Keep query-client connection acceptance and socket I/O separate from query execution.
- Parse complete requests from persistent clients and submit query work to a bounded, fixed-size worker pool.
- Use an ordinary mutex-protected task queue and `std::condition_variable` unless the existing networking code offers an equally simple design.
- Make the worker count configurable, with a reasonable default of four.
- Do not create an unlimited number of client threads.
- Do not let an idle persistent client occupy a query worker; workers receive only complete query tasks.
- Allow multiple workers to process unrelated instruments concurrently.
- Serialize responses safely for each persistent connection so replies are not interleaved or written concurrently.
- Choose one simple full-queue policy during the Phase 3 design walkthrough, such as returning a `BUSY` response or applying bounded backpressure. Do not build an elaborate load-shedding system.

### Required market-state configurations

Implement only:

1. Global reader/writer locking with sort-on-read.
2. Striped reader/writer locking with sort-on-read.
3. Striped reader/writer locking with sort-on-write.

Use the existing dense representation for all three configurations. Do not implement every possible combination of engines and strategies. Introduce a shared engine interface only when the second working implementation actually requires it. Do not create factories, plugin systems, or placeholder engine classes.

These comparisons are required because they provide the project's primary measured engineering conclusions.

### Implementation checkpoints

#### 3A — Concurrent engine boundary

- Identify the smallest common update, query, counters, and checksum operations needed by two real implementations.
- Preserve the Phase 1 reference engine as the correctness oracle for controlled finite streams.
- Refactor completed code only where the new concurrent boundary genuinely requires it.
- Document which layer owns per-exchange ordering and sequence state.

Learning checkpoint: explain why an interface is justified now and why it was unnecessary in Phase 1.

#### 3B — Dedicated feed threads

- Accept and associate one logical connection with each exchange.
- Run one long-lived receiver/decoder loop per exchange connection.
- Preserve order within that connection and never create per-update threads.
- Handle feed disconnect, replacement connection, and shutdown without leaking or detaching threads.

Learning checkpoint: follow two exchanges through two independent threads and show why updates within one exchange stay ordered.

#### 3C — Bounded query-worker pool

- Add the fixed-size worker pool and bounded task queue.
- Protect the queue with one ordinary mutex and coordinate workers with `std::condition_variable` unless an equally simple existing mechanism is preferable.
- Ensure idle clients consume connection resources but not worker threads.
- Define and test the selected full-queue behavior.
- Coordinate response delivery safely with persistent client connections.

Learning checkpoint: trace a query from socket parsing, into the bounded queue, through one worker, and back to the correct client.

#### 3D — Global reader/writer lock

- Protect the entire market state with one `std::shared_mutex` or equivalent ordinary reader/writer lock.
- Updates acquire exclusive access while changing state.
- Queries acquire shared access long enough to copy one instrument's values.
- Release the lock before sorting the independent query copy.
- Protect counters and checksum traversal consistently.

This is the simplest correct concurrent baseline.

Learning checkpoint: draw 10 writers and several readers contending for the same global lock.

#### 3E — Striped reader/writer locks

- Divide instruments across a modest fixed number of reader/writer locks.
- Select a stripe with:

```ini
stripe = instrument_id % stripe_count
```

- Updates acquire exclusive access to the relevant stripe.
- Queries acquire shared access to the relevant stripe long enough to copy the instrument's values.
- Operations on different stripes can proceed concurrently.
- Use one consistent lock-acquisition order wherever an operation requires more than one stripe.
- Use a straightforward fixed stripe count or small configurable set.
- Do not use one lock per instrument unless a concrete requirement makes it necessary.

Learning checkpoint: compare operations targeting the same stripe with operations targeting different stripes.

#### 3F — Sorting modes

- `sort-on-read`: store the latest price by exchange, copy the 10 valid prices during a query, release the lock, and sort the copy.
- `sort-on-write`: maintain a sorted fixed-size view while applying an update so a query can copy an already ordered result.
- Use dense storage for both modes.
- Implement sort-on-write only for the striped engine needed by the controlled comparison.

Learning checkpoint: identify which path pays the sorting cost and why only 10 exchange prices makes the likely tradeoff worth measuring rather than assuming.

#### 3G — Correctness, concurrency, and shutdown tests

- All implementations produce the same logical results as the Phase 1 reference for controlled finite streams.
- Duplicate, stale, out-of-order, invalid, and gapped sequence behavior remains correct.
- Queries never expose partially written price entries.
- Feed threads preserve ordering within their assigned exchanges.
- Multiple workers process unrelated instrument queries correctly.
- The bounded queue's full behavior is deterministic and documented.
- Global and striped engines remain correct under hot-instrument stress.
- Publisher and query-client reconnects work with the threaded server.
- Shutdown works while feed threads and query workers are active.
- Focused ThreadSanitizer tests report no project data races on a supported platform.

### Phase 3 exit condition

- The server uses 10 dedicated long-running feed threads and never creates a thread per update.
- Persistent query clients use a bounded, configurable worker pool without monopolizing workers while idle.
- Global and striped reader/writer locking produce the same logical results as the reference engine.
- Queries are coherent and sorted under concurrent load.
- Sort-on-read and sort-on-write both work using dense state.
- Focused ThreadSanitizer tests report no project data races on their documented platform.
- Shutdown succeeds while feed and query work is active.
- The project owner can identify every thread, queue, lock, protected resource, and lock boundary.

## 9. Phase 4 — Benchmarks, testing, documentation, and résumé polish

### Goal

Measure the project’s primary engineering tradeoffs with a small benchmark matrix, verify the complete system, and produce concise portfolio evidence using only actual results.

### Concepts to teach

- Throughput versus latency.
- Median and tail percentiles: p50, p95, and p99.
- Warm-up, repeated runs, and benchmark noise.
- Debug, sanitizer, and release builds.
- Contention and how workload distribution changes it.
- Direct engine benchmarks versus end-to-end system measurements.
- How to turn measured evidence into an engineering conclusion and résumé bullet.

### Measurement types

#### Direct engine benchmarks

- Call update and query operations without TCP.
- Use the real architecture's 10 feed-writer roles.
- Use these benchmarks to compare locks and sorting without socket overhead obscuring the internal differences.
- Use controlled seeds and workloads so configurations receive equivalent logical work.

#### End-to-end TCP benchmark

- Run one representative scenario through the real server, 10 exchange feeds, and query clients.
- Demonstrate complete-system throughput and responsiveness.
- Do not use the TCP measurement alone to decide which internal sorting strategy is faster.

### Focused comparison matrix

#### Global versus striped locking

Compare:

- Global locking with sort-on-read.
- Striped locking with sort-on-read.

Run:

- Update-heavy uniform traffic.
- Mixed uniform traffic.
- Mixed hot-instrument traffic.

The purpose is to measure when striped locking reduces contention and how hot instruments limit that benefit.

#### Sort-on-read versus sort-on-write

Compare:

- Striped locking with sort-on-read.
- Striped locking with sort-on-write.

Run:

- Update-heavy workloads.
- Balanced update/query workloads.
- Query-heavy workloads.

The purpose is to identify when paying the sorting cost during updates is worthwhile for only 10 exchange prices per instrument.

#### Thread counts

Use 10 feed writers and a small set of query-worker counts, initially:

- 10 feed writers and 1 query worker.
- 10 feed writers and 4 query workers.
- 10 feed writers and 8 query workers.

Adjust the exact worker counts to the development machine's available CPU cores. Do not create a huge benchmark matrix.

### Implementation checkpoints

#### 4A — Small benchmark harness

- Use `std::chrono::steady_clock` for timing.
- Collect latency samples in thread-local storage and merge them after the measured interval.
- Keep result calculation and logging outside the measured hot path where practical.
- Emit simple CSV output.
- Support the focused workloads and configurations listed above without building a general benchmark framework.
- Run five repetitions for each final published scenario.
- Use a release build for performance measurements.

Do not add Google Benchmark, custom histogram libraries, coordinated-omission infrastructure, or advanced profiling frameworks unless a concrete problem makes one necessary.

Learning checkpoint: calculate a percentile from a small sample and explain what p99 does and does not mean.

#### 4B — Required reporting

Report:

- Update throughput.
- Query throughput.
- Query p50, p95, and p99 latency.
- Update latency where it is measured meaningfully.
- Workload type and update/query ratio.
- Uniform or hot-instrument distribution.
- Writer and query-worker counts.
- Stripe count.
- Hardware and operating system.
- Compiler and build type.
- Seed and Git revision.

Learning checkpoint: form a hypothesis before each comparison, then compare it with the measured result and explain any surprise.

#### 4C — Final correctness and portability pass

- Run the complete focused unit and integration suite.
- Run AddressSanitizer and UndefinedBehaviorSanitizer.
- Run focused ThreadSanitizer concurrency tests on a supported platform.
- Verify clean-build instructions from a fresh build directory.
- Verify the three-terminal continuous demonstration.
- Verify deterministic finite-stream comparison against the reference engine.
- Verify disconnect, reconnect, bounded-queue, and active-shutdown behavior.

#### 4D — README and learning documentation

- Explain what ExchangeLab does and why it was built.
- Include one compact architecture diagram.
- Include a verified three-terminal quick start.
- Explain the dense cache-oriented representation, direct indexing, locality, and avoided pointer chasing.
- Explain sequence and coherent-query semantics.
- Explain the dedicated feed threads and bounded query-worker pool.
- Explain global versus striped reader/writer locking.
- Explain sort-on-read versus sort-on-write.
- Explain uniform versus hot-instrument contention.
- Include one measured-results table with hardware, compiler, workload, and repetition context.
- Explain when each measured strategy wins or loses.
- Document limitations without presenting explicitly excluded production features as unfinished requirements.
- Do not include invented or placeholder performance numbers.

#### 4E — Résumé evidence and interview walkthrough

- Write two or three résumé bullets using only measurements produced by the final controlled benchmarks.
- Replace bracketed examples only after the relevant numbers exist.
- Conduct a final interview-style walkthrough of the architecture, one correctness decision, the locking comparison, the sorting comparison, and one benchmark conclusion.

Candidate bullets should resemble:

- Built a continuously running C++20 market-data server ingesting binary TCP feeds from 10 simulated exchanges across 50,000 instruments while serving real-time sorted-price queries to concurrent interactive clients.
- Designed cache-conscious dense state, per-exchange sequence validation, dedicated feed threads, and a bounded client worker pool; maintained coherent reads using global and striped reader/writer locks.
- Benchmarked global versus striped locking and sort-on-read versus sort-on-write across uniform and hot-instrument workloads, reaching `[X updates/sec]`, `[Y queries/sec]`, and `[Z microseconds p99 query latency]`.

The bracketed values are structural examples only. They must not appear as claims in the final README and must be replaced only with actual measurements.

### Phase 4 exit condition

- Direct benchmarks isolate the required locking and sorting comparisons.
- One end-to-end TCP benchmark demonstrates complete-system behavior.
- Final published scenarios have five repetitions and complete environment context.
- Unit, integration, sanitizer, and concurrency checks pass on their documented platforms.
- All documented commands work from a clean build.
- The README contains verified architecture, results, tradeoffs, and limitations.
- Every published number comes from an actual controlled benchmark.
- The project owner can explain and defend the system at a modular level.

## 10. Explicitly excluded scope

Do not plan or implement the following unless the four-phase project is complete and the project owner explicitly approves a new scope:

- Hash-based market-state implementations or dense-versus-hash benchmarks.
- Fault-injection infrastructure.
- Recording and replay.
- UDP.
- Optimistic, atomic-snapshot, or lock-free engines.
- Formal memory-ordering proofs.
- Real market-data integrations.
- CRC, TLS, protocol extensibility frameworks, or other production protocol features.
- Order books and matching.
- NUMA, CPU affinity, custom allocators, or zero-copy experiments.
- Rust rewrites.
- Dashboards or other visualization products.
- Cloud deployment.
- Databases, Kafka, or Kubernetes.
- User accounts or trading integrations.
- Factories, plugin systems, placeholder engines, or directories created for hypothetical future work.

These ideas are excluded because finishing, measuring, understanding, and presenting the selected system provides more résumé and learning value than expanding its feature count.

## 11. Current progress

- The revised four-phase scope was approved on 2026-09-11.
- Phase 1 completed on 2026-09-11 using CMake 3.29.2 and Apple Clang 17.0.0.
- Phase 1 added a concrete single-threaded `MarketState`, dense storage, fixed-point prices, per-exchange sequence validation, sorted queries, deterministic checksum, seeded simulator, CLI, and focused tests.
- Phase 2 completed on 2026-09-11 with three independent executables: a continuous server, a ten-feed exchange simulator, and an interactive query client.
- Added Standalone Asio 1.38.2, a byte-exact 31-byte update protocol, fixed-frame stream decoding, a bounded text query protocol, persistent clients, feed identity binding, bounded publisher queues, fixed-delay publisher reconnects, and signal-driven clean shutdown.
- The Phase 2 server uses one event-loop thread, so updates and queries are serialized and the Phase 1 `MarketState` remains safely single-threaded.
- `ctest --test-dir build --output-on-failure`: 29 of 29 tests passed, including ten-feed TCP integration, fragmentation and combination, invalid and truncated inputs, multiple clients, reconnects, direct-versus-TCP equivalence, and active-socket shutdown.
- The documented Phase 1 demo processed 1,000,000 updates and produced checksum `0xc5b853573d929037` for seed 42. Two runs produced byte-identical output.
- The verified three-process demonstration maintained 10 continuous feed connections while one persistent client returned sorted results for all exchanges; repeated queries showed changing live prices, and both server and simulator stopped cleanly with Ctrl+C.
- Phase 3 multithreading has not started. No feed threads, worker pool, mutexes, shared mutexes, striped locks, sorting alternatives, or benchmark scaffolding exist.
- Next session: explain the selected Phase 3 thread architecture, bounded query-worker queue, engine boundary, reader/writer locks, and sorting strategies before editing code.

## 12. Core definition of done

ExchangeLab is complete when:

- The server runs continuously until interrupted.
- Ten exchange connections continuously publish updates.
- The market state supports 50,000 instruments using the existing dense representation.
- Fixed-point prices and per-exchange sequence validation remain correct.
- A separate interactive client can repeatedly query the live server.
- Multiple clients can query concurrently through a bounded worker pool.
- Publishers and clients can disconnect and reconnect.
- Duplicate, stale, out-of-order, invalid, and gapped updates are handled correctly.
- Fixed-size TCP frames survive fragmented and combined reads.
- Ten dedicated feed threads preserve per-exchange update order.
- Global and striped reader/writer locking produce correct, coherent query results.
- Sort-on-read and sort-on-write have been compared using dense state.
- Uniform and hot-instrument workloads have been measured.
- Direct benchmarks report the internal concurrency and sorting tradeoffs.
- One end-to-end TCP benchmark demonstrates complete-system throughput and responsiveness.
- Benchmarks report throughput and p50, p95, and p99 latency with complete workload and environment context.
- Unit, integration, sanitizer, and concurrency tests pass on their documented platforms.
- All documented commands work from a clean build.
- The README explains the architecture, measurements, tradeoffs, and limitations.
- Every published number comes from an actual controlled benchmark.
- The project owner has received a modular walkthrough of every phase and can explain the system rather than merely possessing completed code.
