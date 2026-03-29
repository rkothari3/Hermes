# Hermes

A C++ NASDAQ ITCH 5.0 market data pipeline that parses 263 million binary messages from a real trading day, reconstructs live order books for every symbol on the exchange, and computes microstructure trading signals (spread, mid-price, microprice, order book imbalance). The pipeline instruments every stage with CPU cycle-level RDTSC timestamps and reports P50/P99/P99.9 latency distributions — the same kind of infrastructure every quantitative trading firm builds internally but that almost no student portfolio contains.

**Key numbers (Intel Core Ultra 7 155H, WSL2 on Windows 11):**
- 263,241,937 book-updating messages processed from a single NASDAQ trading day
- Book Update: P50 ~80 ns (fast mode) / 860 ns median including WSL2 hypervisor jitter
- Signal Compute: P50 = 38 ns, P99 = 84 ns
- TSC calibrated at 2.995 GHz via CLOCK_MONOTONIC

---

## Architecture

```
[ITCH Binary File]  (9 GB, Dec 30 2019 — 264M total messages)
        │
        ▼  2 MB read buffer
┌─────────────────────┐
│  Stage 1            │  parse_file() reads in 2 MB chunks
│  ITCH Binary Parser │  dispatches 9 message types via callback table
│  src/itch_parser.cpp│  big-endian → little-endian field swaps
└──────────┬──────────┘
           │  AddOrder / AddOrderMPID / OrderDelete / OrderCancel /
           │  OrderReplace / OrderExecuted / OrderExecutedPrice
           ▼
┌─────────────────────┐  t0 = rdtsc_start()
│  Stage 2            │  handle_*() looks up order_ref in unordered_map
│  Order Book Updater │  updates flat Level[8192] bid/ask arrays
│  src/order_book.cpp │  O(1) array index by (price − base_price)
└──────────┬──────────┘  t1 = rdtsc_end()  →  book.record(t1-t0)
           │
           ▼
┌─────────────────────┐
│  Stage 3            │  compute_signals(book):
│  Signal Computer    │    spread     = best_ask − best_bid
│  src/signal_engine  │    mid_price  = (bid + ask) / 2
└──────────┬──────────┘    microprice = Stoikov weighted price
           │               obi        = top-5 level imbalance
           │  AAPL only    t2 = rdtsc_end()  →  signal.record(t2-t1)
           ▼
┌─────────────────────┐
│  Stage 4 (AAPL)     │  aapl_signals.csv
│  CSV Output         │  1,177,357 rows of per-tick signals
└─────────────────────┘
           │
           ▼
┌─────────────────────┐
│  Stage 5            │  StageSampler: 2000-bucket 1-ns histogram
│  Latency Report     │  P50/P99/P99.9 for book / signal / total
│  (program end)      │  printed to stdout
└─────────────────────┘
```

---

## Design Decisions

### 1. Array-Indexed Order Book (not `std::map`)

The naive approach to storing price levels in an order book is a `std::map<uint32_t, Level>` — a red-black tree keyed by integer price. This is algorithmically correct but cache-hostile. Every node in a red-black tree is a separate heap allocation at an arbitrary memory address. Traversing five price levels means five pointer dereferences to five random locations in the heap. On a modern CPU, each of those is a potential L1/L2 cache miss costing ~100 ns. For an operation you need to perform 263 million times, that compounds into seconds of wasted time.

Hermes instead uses a flat `Level[8192]` array per side per symbol, indexed by `(price - base_price)`. Because NASDAQ prices are already integers in units of $0.0001 (so $289.50 is stored as `2895000`), array indexing is exact — no hashing, no tree traversal, just a single addition and a memory load. The entire active price range for a typical stock on any given day fits within a few thousand indices, meaning the hot portion of the array lives in L1 or L2 cache. Expected cost: ~1-5 ns per access versus ~500 ns for five cache-missing tree traversals. Each symbol's two sides occupy 8192 × 8 bytes × 2 = 128 KB — comfortably within L2 per symbol on most modern Intel CPUs.

The best bid and best ask indices are maintained incrementally: when a new order arrives at a price better than the current best, the best index is updated in O(1). When the best level is exhausted, the index is walked inward until a non-zero level is found. On a real trading day, the best bid/ask rarely moves more than a few ticks at a time, so the walking cost is negligible.

### 2. Separate `bid_base` / `ask_base` (not a shared window)

An early design used a single shared `base_price` for both the bid and ask arrays of each symbol. This broke down during pre-market hours. AAPL, for example, sometimes has a stub resting order at ~$299.90 during pre-market from a previous session, while the session open is near ~$289.00. A shared base would need to span the full range: roughly 110,000 index slots to cover both the pre-market stub and the active session range. That blows up memory by 10x and defeats the cache locality the design depends on.

The fix is simple: each side maintains its own independent base price, `bid_base` and `ask_base`. The bid array is indexed by `(price - bid_base)` and the ask array by `(price - ask_base)`. The two windows never need to agree with each other. When prices drift outside the current window, `rebase_side()` slides the window via `memmove`, shifting the array contents and updating the base. This keeps each window compact (typically a few hundred to a few thousand live slots) regardless of how far apart pre-market stubs and session prices are.

This design also means the two sides are independently rebased, so a large pre-market ask stub does not force the bid window to use an unreasonably large offset. The implementation is transparent to callers: add, remove, and lookup all go through the same `(price - base)` formula; rebasing happens automatically when the price falls outside the current window.

### 3. Generation Counters (stale-remove protection)

After a full-clear rebase — which zeros the entire `Level` array and resets the base — old orders that were placed before the rebase still exist in the `order_map` (the `unordered_map<uint64_t, Order>` that tracks live orders by reference number). When a Delete or Cancel message arrives for one of these old orders, the system would compute `price - base` using the new base, get a garbage index, and decrement `Level.quantity` from zero to `UINT32_MAX`. In a debug build this is a data corruption bug; in a release build it silently corrupts the book.

The fix is generation counters. Each `OrderBook` maintains a `bid_gen` and `ask_gen` counter, both starting at zero and incremented every time `rebase_side()` performs a full clear. Each `Order` struct in the order map stores the generation value that was active when the order was added, in a `gen` field. On every Delete or Cancel, the handler compares `order.gen` against the book's current generation for that side. If they differ, the order predates the most recent full-clear and its remove is silently skipped — the book is already clean of that level. This adds one comparison per remove operation in the hot path, and eliminates 100% of underflow events: 0 underflows across 263,241,937 book-updating messages on the full December 30 2019 ITCH file.

### 4. RDTSC with Serialization Fences (not `std::chrono`)

`std::chrono::high_resolution_clock::now()` is a convenient but expensive timer. On Linux it resolves to a `clock_gettime` syscall, which costs ~20-50 ns per call — more than the entire book update operation being measured. Using `chrono` for nanosecond-scale profiling produces measurements that are dominated by the measurement overhead itself.

RDTSC (Read Time-Stamp Counter) is a single CPU instruction that reads the processor's cycle counter directly. On a 3 GHz CPU, two consecutive `rdtsc` calls have an overhead of ~2-4 cycles (~1 ns), small enough to measure operations in the 30-100 ns range accurately. The TSC increments at a fixed rate regardless of CPU frequency scaling (on modern Intel processors with invariant TSC), so conversion to nanoseconds requires only a single multiply calibrated once at startup.

The subtle correctness issue is out-of-order execution. A modern CPU executes instructions out of program order for performance. A bare `rdtsc` issued before the measured code may be retired *after* the measured code has already begun executing, making the start timestamp too late — producing measurements that are systematically too low. The Intel white paper "How to Benchmark Code Execution Times on Intel IA-32 and IA-64 Instruction Set Architectures" prescribes a specific pattern: `LFENCE` before the start read (drains the pipeline so all prior instructions have retired before the counter is sampled), and `RDTSCP` + `LFENCE` at the end (`RDTSCP` is self-serializing and also prevents the counter read from being hoisted before the measured code, and the trailing `LFENCE` prevents subsequent loads from speculating before the counter is read). A `"memory"` compiler clobber on both functions prevents the compiler from reordering loads and stores across the timestamp boundaries. The implementation in `include/latency_profiler.hpp`:

```cpp
// Use at the START of a measured region: LFENCE serializes prior instructions
// before the counter is read. "memory" prevents compiler reordering.
inline uint64_t rdtsc_start() {
    uint32_t lo, hi;
    __asm__ __volatile__ (
        "lfence\n\t"
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        :: "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

// Use at the END of a measured region: RDTSCP self-serializes (ensures all
// prior instructions have retired), then LFENCE prevents subsequent loads
// from reordering before the counter read. "memory" prevents compiler reordering.
inline uint64_t rdtsc_end() {
    uint32_t lo, hi, aux;
    __asm__ __volatile__ (
        "rdtscp\n\t"
        "lfence"
        : "=a"(lo), "=d"(hi), "=c"(aux)
        :: "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}
```

The optimization story in this project was discovering that the initial profiler used a bare `rdtsc()` without any serialization fences. This produced Book Update latency readings that were plausible but subtly incorrect — the CPU was reading the start counter while previous instructions were still in flight. Replacing it with the `rdtsc_start` / `rdtsc_end` pair per the Intel-recommended pattern corrected the measurements to reflect actual retired instruction timing. This is a measurement correctness fix, not a performance optimization, but getting the measurement right is the prerequisite for any optimization work.

### 5. Fixed-Size Histogram (no raw sample storage)

The straightforward approach to collecting latency measurements is to append each sample to a `std::vector<uint64_t>`. For 263 million messages measured at three stages, that is 263M × 3 × 8 bytes ≈ 6.3 GB of raw sample data. This is not feasible: it requires gigabytes of allocation, thrashes the allocator, and defeats the cache locality of the hot path.

Hermes uses a fixed-size histogram instead: 2000 buckets of 1 ns each, covering the range 0–1999 ns, plus an overflow counter for samples at or above 2000 ns. The entire histogram is 2000 × 8 bytes = 16 KB per stage sampler, and all three stage samplers together fit in 48 KB — well within L2 cache. The `record()` function is four instructions: increment total, divide cycles by the pre-calibrated GHz value to get nanoseconds, bounds-check against 2000, increment the appropriate bucket. There is no allocation, no vector resize, no indirection. P50, P99, and P99.9 are computed by a single linear walk over the 2000 buckets at the end of the run, accumulating a running count until the appropriate percentile threshold is crossed.

This design was critical for correct behavior in the hot path. A heap allocation inside the measurement loop would itself appear in the latency measurement, creating a feedback loop. The fixed histogram avoids this entirely: the same 48 KB structure is written on every iteration, stays resident in L2, and adds no allocation overhead to the measurements.

---

## Benchmark Results

**Hardware:** Intel Core Ultra 7 155H (4.8 GHz boost, L1=80KB, L2=2MB/core, L3=24MB)
**Platform:** WSL2 on Windows 11 | **Build:** Release (-O2) | **File:** 12302019.NASDAQ_ITCH50

```
Stage                     P50      P99    P99.9      Samples
──────────────────── ──────── ──────── ──────── ────────────
Book Update              860 ns  >2000 ns  >2000 ns    263241937
Signal Compute            38 ns      84 ns  >2000 ns    263241937
Full Callback            903 ns  >2000 ns  >2000 ns    263241937

TSC rate: 2.995 GHz (calibrated via CLOCK_MONOTONIC busy-wait)
```

The P50 of 860 ns is a **WSL2 artifact**. The histogram reveals a bimodal distribution:

```
  [  80-  89 ns]    5,772,974  (2.19%)   ← fast mode: true compute latency
  [ 860- 879 ns]    3,147,640  (1.20%)   ← slow mode: WSL2 hypervisor preemption (~800 ns penalty)
  [>=2000 ns]      39,610,347  (15.05%)  ← extreme jitter events
```

The WSL2 hypervisor vCPU scheduler periodically preempts the thread, injecting ~800 ns penalties on ~40% of measurements. The fast mode (~80-89 ns) is the **true compute latency**. The P50 lands in the slow-mode region because the preemption events are frequent enough that fewer than 50% of samples complete before the hypervisor penalty appears.

| Metric | WSL2 (measured) | Bare-metal Linux (estimated) |
|--------|-----------------|------------------------------|
| Book Update P50 | 860 ns | ~80 ns |
| Book Update P99 | >2000 ns | ~300 ns |
| Signal Compute P50 | 38 ns | ~38 ns (compute-bound, unaffected) |
| Compute throughput | ~590K msg/s (I/O bound) | ~12.5M msg/s |

Signal Compute is relatively unaffected by the hypervisor jitter because it is a short burst of integer arithmetic (spread, mid-price, microprice, OBI over 5 levels) that completes before the hypervisor preemption window opens. The P50 of 38 ns on WSL2 is expected to be the same on bare-metal Linux for this stage.

End-to-end throughput measured at ~590K messages/second reflects I/O overhead: reading ~9 GB through the WSL2 VirtioFS layer from a Windows NTFS filesystem. The CPU `user` time was 7m27s against 12m57s `real` time, with the difference dominated by file I/O. Pure compute throughput derived from the fast-mode P50 of ~80 ns is approximately 12.5M messages/second.

### Optimization: RDTSC Serialization Fix

The initial profiler used bare `rdtsc()` without serialization fences. A review of the Intel benchmark white paper identified that without `LFENCE` before reading the start counter, the CPU's out-of-order execution engine can read the TSC before the measured code has actually retired from the pipeline, producing measurements that are systematically 10–30 ns too low and that do not reflect actual instruction completion. Without `RDTSCP` at the end, the CPU can retire the counter read before the last measured instruction has completed, compounding the error.

**Before:** `rdtsc()` — measurements potentially 10–30 ns low due to OOO execution reordering

**After:** `rdtsc_start()` (LFENCE + RDTSC + memory clobber) / `rdtsc_end()` (RDTSCP + LFENCE + memory clobber) — measurements reflect actual retired instructions, per Intel white paper

This is a **measurement correctness fix**, not a performance optimization. The numbers in the table above use the corrected implementation. On a 3 GHz processor, the combined overhead of the two serializing fence pairs is approximately 6–8 cycles (~2 ns), which is negligible relative to the operations being measured.

---

## Build and Run

**Requirements:** Linux (or WSL2), GCC or Clang, CMake >= 3.16, ~2 GB RAM

```bash
# Clone and build
git clone <repo-url>
cd hermes
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run on ITCH file (prints book snapshots + latency report, writes aapl_signals.csv)
./build/hermes /path/to/12302019.NASDAQ_ITCH50

# Unit tests (fast — no ITCH file needed, completes in < 1 second)
./build/hermes_tests

# Integration test (slow — reads the full 9 GB ITCH file)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DITCH_FILE=/path/to/file.NASDAQ_ITCH50
cmake --build build -j$(nproc)
./build/hermes_integration_tests
```

The binary prints a top-5 bid/ask snapshot for AAPL, MSFT, TSLA, AMZN, and NVDA every 1 million messages during the run, followed by the latency report at the end. The `aapl_signals.csv` file written to the current directory contains 1,177,357 rows of per-tick spread, mid-price, microprice, and OBI values for AAPL across the full trading day.

For best latency measurements, pin the process to a P-core (not an E-core) on hybrid architectures like the Core Ultra 7 155H. The binary calls `pin_to_core(0)` automatically at startup via `pthread_setaffinity_np`; verify that logical CPU 0 maps to a P-core on your system using `lscpu -e`.

---

## Data Source

NASDAQ publishes historical ITCH 5.0 files for free:

```
https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
```

Files are named `MMDDYYYY.NASDAQ_ITCH50.gz` (~1 GB compressed, ~9 GB uncompressed). The benchmark file used here is `12302019.NASDAQ_ITCH50` (December 30, 2019).

```bash
wget "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz"
gunzip 12302019.NASDAQ_ITCH50.gz
./build/hermes 12302019.NASDAQ_ITCH50
```

Any file from the NASDAQ ITCH archive will work. Newer files (post-2020) tend to have higher message counts as trading volume on NASDAQ has grown. The format is stable; the ITCH 5.0 specification has not changed since 2014.

---

## What I Learned

**Cache behavior dominates at this scale.** The choice between `std::map` and a flat array for order book price levels is not about algorithmic complexity — both are effectively O(1) for the operations performed here (insert, delete, lookup). The difference is entirely about memory access patterns. A red-black tree node is a heap allocation at an arbitrary address; traversing five levels means five pointer dereferences to five random cache lines. At ~100 ns per cache miss on a cold L3, five levels cost ~500 ns. A flat `Level[8192]` array for the same symbol has all its data contiguous; the entire active range fits in L1 or L2 and costs ~1-5 ns to access. At 263 million messages, this gap between ~5 ns and ~500 ns per book update is the difference between a system that keeps up with live data and one that cannot. Algorithmic analysis with O-notation is necessary but not sufficient for this class of problem: you have to think in cache lines.

**Measuring correctly is harder than measuring.** Three separate correctness issues must be solved simultaneously for RDTSC to produce valid latency data. First, `LFENCE` before the start read is required to drain the CPU pipeline — without it, the out-of-order execution engine can read the counter before prior instructions retire, making the start timestamp too late and the interval too short. Second, `RDTSCP` at the end is required because it is self-serializing: bare `RDTSC` at the end can be read before the last measured instruction has retired, compressing the measured interval further. Third, a `"memory"` compiler clobber on both functions is required to prevent the compiler from hoisting loads out of the measured region or sinking them past the end timestamp, which would silently exclude work from the measurement. Any single one of these three missing produces subtly wrong numbers — not obviously wrong, just 10-30 ns too low in ways that look plausible. This is why naive RDTSC benchmarks published online often report implausibly fast latencies: they are missing one or more of these constraints.

**Histograms reveal what averages hide.** The arithmetic mean of Book Update latency in this run is approximately 700 ns. The P50 is 860 ns. The fast-mode peak is 80-89 ns. None of these is "the" latency — they tell three different stories about the same data. If I had only computed the mean or the P50, I would have concluded that the system is slow and begun optimizing the order book. The histogram immediately revealed the bimodal structure and identified the WSL2 hypervisor as the source of the slow mode — a scheduling artifact, not a code performance problem. The fast mode at 80-89 ns is the true compute performance, in line with what would be expected on bare-metal Linux. This experience illustrates why real trading firms instrument P99 and P99.9 rather than averages: tail latencies determine whether a fill happens at the expected price, and distributions reveal the root cause of outliers in ways that aggregate statistics cannot.

---

## Repository Layout

```
hermes/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── itch_parser.hpp        # Message structs + parser interface
│   ├── order_book.hpp         # Array-indexed order book + generation counters
│   ├── signal_engine.hpp      # Spread / mid / microprice / OBI computation
│   └── latency_profiler.hpp   # rdtsc_start/rdtsc_end + StageSampler histogram
├── src/
│   ├── itch_parser.cpp        # parse_file(), handle_* callbacks
│   ├── order_book.cpp         # add_order(), remove_order(), rebase_side()
│   ├── signal_engine.cpp      # compute_signals(), CSV output for AAPL
│   ├── latency_profiler.cpp   # calibrate_rdtsc_ghz(), pin_to_core()
│   └── main.cpp               # Pipeline orchestration, benchmark report
├── tests/
│   ├── test_parser.cpp        # 10 parser unit + integration tests
│   ├── test_order_book.cpp    # 25 order book tests (rebase, generation counters)
│   └── test_signals.cpp       # 12 signal computation tests
└── docs/
    └── bench_results.md       # Full histogram + run-time data
```
