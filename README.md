# ExchangeLab

ExchangeLab is a local C++20 market-data laboratory based on the Optiver
market-data design problem. It now runs as three independent processes:

- `exchange-simulator` publishes prices over 10 logical TCP feeds.
- `exchangelab-server` stores the latest accepted prices and serves queries.
- `exchangelab-client` repeatedly queries live state from an interactive
  terminal.

The project is learning-focused. Phase 1 established a deterministic,
single-threaded reference engine. Phase 2 adds a continuously running TCP
system while intentionally retaining one event-loop thread inside the server.
Threaded ingestion, reader/writer locks, worker pools, sorting-strategy
comparisons, and benchmarks belong to later phases.

## Phase 2 architecture

```text
exchange-simulator process
  Simulator
      |
      +-- feed 0 --\
      +-- feed 1 ----\       fixed 31-byte binary frames
      +-- ... --------+---- TCP 127.0.0.1:9000 ----\
      +-- feed 9 ----/                                 |
                                                         v
                                               exchangelab-server process
                                                one Asio event-loop thread
                                                         |
                                           decode and validate MarketUpdate
                                                         |
                                                    MarketState
                                                         |
                                            sorted instrument query
                                                         ^
                                                         |
exchangelab-client process                               |
  interactive terminal ---- text TCP 127.0.0.1:9001 ----/
```

The server owns the only `MarketState`. The simulator and client are separate
processes with separate memory, so they communicate exclusively through TCP.

The feed and query ports are separate because they carry different protocols:

- Port `9000` receives fixed-size binary update frames.
- Port `9001` receives newline-delimited text queries.

## Market-state rules

The default market contains 50,000 instruments and 10 exchanges. State uses
one dense vector. The entry for an instrument/exchange pair is found with:

```text
index = instrument_id * exchange_count + exchange_id
```

Prices are signed 64-bit integers measured in micros. For example,
`$101.250000` is stored as `101250000`. This avoids floating-point comparison
and rounding problems.

Sequence numbers are tracked per exchange feed:

- A newer sequence is accepted.
- An equal sequence is a duplicate.
- A lower sequence is stale or out of order.
- A jump is accepted and counted as a gap.
- Invalid updates do not change price or sequence state.

Queries return valid exchange prices sorted by price and then exchange ID.
Exchanges that have not supplied a price for an instrument are reported as
missing.

## TCP protocols

TCP is an ordered byte stream; it does not preserve application-message
boundaries. A 31-byte update may arrive in several reads, and several updates
may arrive in one read. Each server feed session therefore accumulates bytes,
decodes every complete frame, and preserves an incomplete tail for its next
read.

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
numbers are rejected before they reach `MarketState`. A cleanly decoded
duplicate or stale sequence reaches `MarketState`, which applies the normal
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

Malformed, oversized, and out-of-range requests receive an `ERROR` line
followed by `END`. The terminator lets the persistent client identify one
complete response even though TCP has no message edges.

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

The test suite covers the Phase 1 engine, byte-exact encoding, every useful
frame split, combined frames, invalid and truncated input, ten feed
connections, multiple query clients, reconnects, direct-versus-TCP
equivalence, and shutdown with active sockets.

## Run the continuous three-terminal demo

Start the server in terminal 1:

```bash
./build/exchangelab-server
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

The two queries may show different prices because the server continues
processing feed updates between them. A missing exchange simply has not yet
published a valid price for that instrument.

Press Ctrl+C in the simulator and server terminals to stop them cleanly.

View each program's options with:

```bash
./build/exchangelab-server --help
./build/exchange-simulator --help
./build/exchangelab-client --help
```

## Run a finite deterministic publisher

Finite mode is useful for correctness tests and later controlled benchmarks:

```bash
./build/exchange-simulator \
  --events 1000000 \
  --exchanges 10 \
  --instruments 50000 \
  --rate 1000000 \
  --seed 42 \
  --distribution uniform
```

For the same implementation and arguments, the simulator generates the same
logical update stream. TCP timing and interleaving between different sockets
are not deterministic, but order within each exchange connection is
preserved. Since sequence and price state are independent per exchange, the
finite TCP path reaches the same logical result as applying that stream
directly.

The publisher pauses generation while one of its bounded feed queues cannot
accept another update. It retries a lost connection after a fixed delay and
retains its in-memory sequence state across that reconnect.

Restarting the entire simulator resets its sequence numbers. If the server is
still holding higher sequences from an earlier simulator process, it will
correctly reject the restarted feed's lower sequences as stale. The protocol
intentionally has no production-style session epoch or reset handshake.

## Current limits

- The server runs one Asio event-loop thread, so updates and queries are
  serialized rather than executed in parallel.
- There are no locks or query-worker threads yet.
- Update-rate scheduling is a target, not a real-time guarantee.
- Prices do not expire when a publisher disconnects; the last accepted price
  remains queryable.
- The binary protocol intentionally has no CRC, TLS, authentication, or
  extensible message framework.
- Benchmark results have not been collected, so no performance claims are
  made.

See [the learning and implementation plan](docs/implementation-plan.md) for
the four-phase scope.
