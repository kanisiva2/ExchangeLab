# ExchangeLab

ExchangeLab is a local C++20 market-data laboratory based on the Optiver
market-data design problem. It runs as three independent processes:

- `exchange-simulator` publishes prices over 10 logical TCP feeds.
- `exchangelab-server` stores the latest accepted prices and serves queries.
- `exchangelab-client` repeatedly queries live state from an interactive
  terminal.

The project is learning-focused. Phase 1 established a deterministic
single-threaded reference engine. Phase 2 added the continuously running TCP
system. Phase 3 preserves that server as a selectable baseline and adds
dedicated feed threads, a bounded query-worker pool, reader/writer locking,
and two sorting placements. Phase 4 will measure those choices; no performance
claims have been made yet.

## Selectable server modes

One server binary exposes all four runtime modes so later comparisons can use
the same protocols, simulator, dimensions, seeds, and workload generator:

| `--mode` | Execution model | Locking | Sorting |
|---|---|---|---|
| `single-threaded` | Phase 2 event loop | none | on read |
| `global-read` | threaded | one global reader/writer lock | on read |
| `striped-read` | threaded | striped reader/writer locks | on read |
| `striped-write` | threaded | striped reader/writer locks | on write |

`single-threaded` remains the default. It still applies feed updates and
answers queries directly on the caller-owned Asio event-loop thread using the
original Phase 1 `MarketState`. It creates no feed threads and no query-worker
threads. The other three rows are the only concurrent market-state
configurations implemented in Phase 3.

## Thread and ownership model

The default threaded server has 15 application threads:

```text
exchangelab-server process
|
+-- main Asio I/O thread (owned by main)
|   +-- feed and query acceptors
|   +-- feed bootstrap sessions
|   +-- query sockets and session lifetimes
|   +-- response writes and signal handling
|
+-- 10 feed threads (owned and joined by MarketServer)
|   +-- feed 0 thread owns exchange 0 socket + decoder
|   +-- feed 1 thread owns exchange 1 socket + decoder
|   +-- ...
|   +-- feed 9 thread owns exchange 9 socket + decoder
|
+-- 4 query workers by default (owned and joined by MarketServer)
    +-- take complete query tasks from one bounded queue
```

A thread is one independently scheduled path through a process. Threads share
the process's memory, which makes communication cheap but also means concurrent
access must be coordinated. The server never creates a thread per update, per
query, or per client, and it never detaches a thread.

The Phase 2 baseline has only the main event-loop thread inside the server.
That path deliberately avoids locks and worker-pool overhead so it remains a
fair behavioral and performance baseline.

## Exchange-feed flow and ordering

TCP is an ordered byte stream, but it does not preserve application-message
boundaries. One 31-byte update can arrive in several reads, and several updates
can be waiting at once.

In a threaded mode, the main I/O thread reads exactly one complete frame from
a new feed connection. That frame identifies the exchange. The server then
hands the socket and first update to that exchange's permanent worker:

```text
TCP accept on main thread
        |
read and validate first 31-byte frame
        |
choose worker by exchange_id
        |
transfer socket ownership
        v
exchange feed thread -> decode frames in receive order -> engine.apply(update)
```

Only the assigned feed thread reads that exchange socket and calls `apply` for
its frames. TCP preserves byte order, the stream decoder emits frames in byte
order, and the worker calls `apply` in that same order. This is how per-feed
ordering is preserved without a thread per update.

Sequence numbers remain monotonic per exchange, not per instrument:

- A newer sequence is accepted.
- An equal sequence is a duplicate.
- A lower sequence is stale or out of order.
- A jump is accepted and counted as a gap.
- Invalid data changes neither the price nor the accepted sequence.

One permanent worker slot exists for each exchange ID. A healthy connection
cannot be replaced by a second claimant. When a feed disconnects, its worker
closes and releases the socket, resets its decoder, and notifies the main I/O
thread that the slot may accept a replacement. The worker thread itself stays
alive, and sequence state stays in the market engine. A reconnect therefore
continues the existing sequence history.

## Persistent clients and the bounded query pool

An idle query client has an asynchronous socket read registered with the main
I/O thread. It has no worker assigned. A worker is used only after a complete
line has arrived and parsed successfully.

```text
client socket
    |
main I/O thread reads one complete request
    |
    +-- malformed ----------------------> main writes ERROR
    |
    +-- valid --> bounded task queue
                     |
              condition variable wakes one worker
                     |
              worker calls engine.query
                     |
              worker formats an independent response
                     |
              post response to main I/O thread
                     |
              main writes to the original client socket
                     |
              read next request from that client
```

The task queue is a `std::deque` protected by one `std::mutex`. A producer
locks the mutex, checks capacity, pushes one task, unlocks, and calls
`notify_one`. Each worker waits on a `std::condition_variable`, which puts the
thread to sleep without repeatedly checking the queue. It wakes when work is
available or shutdown begins, removes one task while holding the mutex, then
unlocks before running the query. The market-state locks and queue lock are
therefore independent.

The queue capacity counts tasks waiting for a worker. Its full policy is an
immediate, deterministic `ERROR busy` response. Blocking the main I/O thread
would prevent it from serving other sockets, while an unbounded queue could
consume memory and hide overload behind growing tail latency.

Only one request-response cycle is active per client session. While a worker
handles a request, that session does not start its next read. The worker never
writes a socket: it posts the completed string to the main I/O thread, which
performs the write and then resumes reading. This keeps responses attached to
the correct session and prevents overlapping or reordered writes.

Configure the pool with `--query-workers N` and its waiting capacity with
`--query-queue N`. Their defaults are four workers and 256 waiting tasks.

## Dense market state

Every implementation preserves the same dense canonical representation for
50,000 instruments and 10 exchanges. An entry is found with:

```text
index = instrument_id * exchange_count + exchange_id
```

Dense indexing is useful here because both ID ranges are bounded and compact.
It gives constant-time address calculation, contiguous storage, and predictable
iteration without hash buckets, per-entry allocation, or pointer chasing.

Prices are signed 64-bit integers measured in micros. For example,
`$101.250000` is stored as `101250000`. This avoids floating-point comparison
and rounding problems.

The two real concurrent implementations share only a small
`ConcurrentMarketState` interface: `apply`, `query`, `stats`, and
`logical_checksum`. The original single-threaded `MarketState` stays concrete
and remains the correctness oracle. No factory or plugin system is needed for
four explicit modes.

## Data races, mutexes, and reader/writer locks

A data race occurs when two threads access the same memory at the same time,
at least one access writes it, and synchronization does not order those
accesses. In C++, a data race is undefined behavior: it is not merely a
slightly stale result.

An ordinary `std::mutex` permits one owner at a time. It protects the query
task queue and each exchange's sequence metadata in the striped engine. A
`std::shared_mutex` is a reader/writer lock: many readers may hold shared
access together, but a writer's exclusive access excludes every other reader
and writer. It protects dense price entries.

Threaded server counters that do not need a multi-field transaction use
atomics. Session collections and exchange connection occupancy stay on the
main I/O thread. Feed socket and decoder state stay on their one owning feed
thread. These ownership rules avoid unnecessary locking.

## Exact market-state lock boundaries

### Global reader/writer locking with sort-on-read

```text
update: exclusive global lock
        [validate sequence -> write entry -> sequence -> counters]

query:  shared global lock
        [copy one instrument's entries]
        unlock -> sort private copy

stats:  shared global lock [copy counters]
hash:   shared global lock [read sequences and all canonical entries]
```

This is the simplest correct concurrent engine. Unrelated instruments still
contend because every operation uses the same lock.

### Striped reader/writer locking

An instrument selects protection independently of its dense storage address:

```text
stripe = instrument_id % stripe_count
```

The default is 64 stripes and can be changed with `--stripes N`.

```text
accepted update:
  lock exchange sequence mutex exclusively
  validate sequence
  lock target stripe exclusively
  [write dense entry -> accepted sequence -> optional sorted view]
  unlock stripe, then sequence mutex

query:
  lock target stripe shared
  [copy canonical entries or maintained sorted view for one instrument]
  unlock
  sort private copy only in striped-read mode

counters:
  relaxed atomic increments and snapshot loads

checksum:
  lock every exchange sequence mutex in ascending exchange order
  lock every stripe shared in ascending stripe order
  [hash sequences and canonical dense entries]
```

Two updates on different stripes may execute their entry-writing sections at
the same time. Two readers on the same stripe may also proceed together. A
writer and any other operation on the same stripe must wait. Instruments can
share a stripe, so striping reduces contention without creating 50,000 locks.

The checksum is the only current operation that needs multiple stripes. It
takes sequence locks first, then stripe locks, and always uses ascending order.
Updates use the same sequence-before-stripe order. A cycle of threads waiting
on one another therefore cannot form, which prevents deadlock in these paths.

## Sort-on-read and sort-on-write

Both modes return prices ordered by ascending price and then exchange ID.

- Sort-on-read stores the latest price in exchange order. A query copies at
  most 10 entries while holding a shared lock, releases the lock, and sorts its
  own copy. Updates stay cheap; every query pays for sorting.
- Sort-on-write rebuilds one instrument's fixed-size ordered view after each
  accepted update while holding that stripe exclusively. A query copies the
  already ordered view. Updates do more work; queries do less.

The sort-on-write view is derived data and is excluded from the logical
checksum. Phase 4 will test whether moving this work benefits query-heavy
traffic and whether sort-on-read remains better for update-heavy traffic. The
answer will be measured rather than assumed.

## TCP protocols

The feed and query ports are separate because they carry different protocols:

- Port `9000` receives fixed-size binary update frames.
- Port `9001` receives newline-delimited text queries.

### Binary update frame

Every multi-byte value is encoded most-significant byte first in network byte
order. Raw C++ structs are never transmitted.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 byte | Protocol version |
| 1 | 2 bytes | Exchange ID |
| 3 | 4 bytes | Instrument ID |
| 7 | 8 bytes | Positive fixed-point price in micros |
| 15 | 8 bytes | Sequence number |
| 23 | 8 bytes | Source timestamp in nanoseconds |
| | **31 bytes** | **Total** |

Unsupported versions, out-of-range IDs, nonpositive prices, and zero sequence
numbers are rejected before they reach market state. A cleanly decoded
duplicate or stale sequence reaches the engine, which applies the normal
sequence rules.

### Text query protocol

A request is one bounded line:

```text
QUERY 1234
```

A response is a human-readable group of lines ending in `END`:

```text
RESULT 1234
PRICE 7 $100.212500 sequence=812 source_ns=164000
PRICE 2 $100.237500 sequence=809 source_ns=159000
MISSING 0 1 3 4 5 6 8 9
END
```

Malformed, oversized, out-of-range, and busy requests receive an `ERROR` line
followed by `END`. The terminator lets a persistent client identify a complete
response even though TCP has no message edges.

## Shutdown

Ctrl+C is handled on the main I/O thread. Shutdown proceeds in ownership order:

1. Mark the server stopped and close both acceptors so no new work arrives.
2. Close feed-bootstrap, single-threaded-feed, and query sessions.
3. Ask each permanent feed worker to cancel and close its socket, stop its
   private event loop, and join its thread.
4. Stop query submissions, discard waiting tasks for closing clients, wake all
   query workers, let any short active query return, and join the workers.
5. Let canceled main-loop callbacks observe closed sessions and finish.

No callback owns a raw session pointer, and no application thread survives its
owner.

## Build and test

Requirements:

- CMake 3.24 or newer
- A C++20 compiler such as Apple Clang, Clang, or GCC
- Internet access during the first configuration if pinned dependencies are
  not already available

The project uses GoogleTest 1.15.2 and Standalone Asio 1.38.2.

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The tests cover the Phase 1 oracle, codecs and framing, all four runtime modes,
deterministic equivalence, sequence validation, coherent reads during writes,
hot-instrument concurrent updates, bounded-queue capacity, idle clients,
serialized persistent-client responses, feed reconnects, invalid and truncated
input, and shutdown with active sockets.

### ThreadSanitizer

ThreadSanitizer instruments memory accesses and reports data races on paths a
test actually executes. Enable it in a separate build on a supported Clang or
GCC platform:

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DEXCHANGELAB_ENABLE_THREAD_SANITIZER=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

A clean run is useful evidence, not a mathematical proof: unexecuted paths or
rare schedules may still contain a race. The sanitizer build compiles with the
available Apple Clang toolchain on the current development host, but its runtime
crashes before test startup. A focused runtime pass therefore remains to be
performed on a supported Linux Clang or GCC environment.

## Run the continuous three-terminal demo

Start a threaded server in terminal 1:

```bash
./build/exchangelab-server \
  --mode striped-read \
  --query-workers 4 \
  --query-queue 256 \
  --stripes 64
```

Start ten continuous exchange feeds in terminal 2:

```bash
./build/exchange-simulator \
  --continuous \
  --exchanges 10 \
  --instruments 50000 \
  --rate 100000 \
  --seed 42 \
  --distribution uniform
```

Start the interactive query client in terminal 3:

```bash
./build/exchangelab-client
```

Then enter repeated commands:

```text
> query 1234
> query 1234
> quit
```

The two queries may show different prices because feed workers continue
processing updates. Stop the simulator and server with Ctrl+C. The server's
startup output shows its selected mode and thread counts; its shutdown output
shows separate update/query counters and the logical checksum.

Run the same demonstration with `--mode single-threaded`, `global-read`, or
`striped-write` to observe identical protocol behavior. Phase 4 will automate
fair throughput and latency comparisons rather than treating this interactive
demonstration as a benchmark.

## Current limits

- No Phase 4 benchmark harness or performance results exist yet.
- Update-rate scheduling is a target, not a real-time guarantee.
- Prices do not expire when a publisher disconnects; the last accepted price
  remains queryable.
- Restarting the simulator resets its sequences; a still-running server will
  correctly reject those lower values as stale.
- The binary protocol intentionally has no CRC, TLS, authentication, session
  epoch, or extensible message framework.
- There is no fault injection, recording/replay, UDP, real market-data
  integration, hash-based state, per-instrument locking, optimistic or
  lock-free snapshotting, work stealing, or adaptive scheduling.

See [the learning and implementation plan](docs/implementation-plan.md) for
the four-phase scope and the controlled comparisons reserved for Phase 4.
