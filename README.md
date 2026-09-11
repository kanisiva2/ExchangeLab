# ExchangeLab

ExchangeLab is a local C++20 market-data laboratory. It simulates exchanges publishing price updates, stores the latest accepted price from each exchange, and returns a sorted cross-exchange view for a requested instrument.

The project is being built in learning-focused phases. Phase 1 is a correct, deterministic, single-threaded reference implementation. Networking, concurrency, fault injection, replay, and performance measurements will be added only in their later phases.

## Phase 1 architecture

```text
seed and CLI options
        |
        v
    Simulator  --MarketUpdate-->  MarketState
                                      |
                                      +--> sorted instrument query
                                      +--> counters
                                      +--> final-state checksum
```

The default market contains 50,000 instruments and 10 exchanges. State is stored in one dense vector. The entry for one instrument/exchange pair is found with:

```text
index = instrument_id * exchange_count + exchange_id
```

Prices are signed 64-bit integers measured in micros. For example, `$101.250000` is stored as `101250000`. This avoids floating-point comparison and rounding issues.

Sequence numbers are tracked per exchange feed:

- A newer sequence is accepted.
- An equal sequence is a duplicate.
- A lower sequence is stale.
- A jump is accepted and counted as a gap.
- Invalid updates do not change prices or sequence state.

Queries return valid exchange prices sorted by price and then exchange ID. Exchanges that have not supplied a price for that instrument are listed as missing.

## Build and test

Requirements:

- CMake 3.24 or newer
- A C++20 compiler such as Apple Clang, Clang, or GCC
- Internet access during the first configuration if GoogleTest is not installed locally

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The first command configures the project into the ignored `build/` directory. The second compiles the core library, CLI, and tests. The third runs all registered tests and prints details if one fails.

## Run the deterministic demo

```bash
./build/exchangelab \
  --seed 42 \
  --updates 1000000 \
  --exchanges 10 \
  --instruments 50000 \
  --distribution uniform \
  --query 1234
```

Run the same command twice to receive the same query result, counters, and checksum. View every option with:

```bash
./build/exchangelab --help
```

## Current limits

- State is single-threaded.
- Updates are generated in process rather than received over a network.
- Benchmark numbers have not been collected, so no performance claims are made.
- The checksum detects different logical outcomes; it is not a cryptographic security feature.

See [the learning and implementation plan](docs/implementation-plan.md) for the five-phase scope.
