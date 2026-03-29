# PROJECT: HERMES
## Real-Time Market Data Signal Pipeline with Nanosecond Latency Profiling

### WHAT THIS PROJECT IS — THE ONE-PARAGRAPH SUMMARY

Hermes is a C++ system that reads real binary market data published by NASDAQ, parses it message by message, reconstructs a live view of the buy/sell order book for each stock symbol in real-time, computes trading signals derived from the structure of that order book, and measures the latency of every stage in the pipeline using CPU cycle-level timestamps. The output is a benchmark report showing exactly how fast each component of the pipeline runs and where the bottlenecks are. The project is not a trading strategy. It is a high-performance data processing engine — the type of infrastructure that every quantitative trading firm builds internally but that almost no student project portfolio contains.

---

## BACKGROUND: THE FINANCIAL CONCEPTS YOU NEED

Before understanding the code, you need to understand what you are building and why it matters to trading firms.

#### What is a Stock Exchange?

A stock exchange like NASDAQ is fundamentally a computer system that keeps a list of buy orders and sell orders for every stock (AAPL, TSLA, MSFT, etc.) and matches them when prices agree. Every time someone places an order to buy 100 shares of AAPL at $220.00, that order enters the exchange's system and waits. When someone else places a sell order at $220.00 or lower, the exchange matches them and a trade happens.

#### What is a Limit Order Book?

The Limit Order Book (LOB) for a single stock is the full list of all unexecuted buy and sell orders sitting at the exchange, organized by price. It looks like this conceptually:

```
--- AAPL ORDER BOOK ---

SELL SIDE (ASKS) — people willing to sell
$221.50 → 300 shares available
$221.00 → 1200 shares available
$220.50 → 800 shares available

--- SPREAD ---

BUY SIDE (BIDS) — people willing to buy
$220.00 → 500 shares available    ← best bid
$219.50 → 2000 shares available
$219.00 → 1500 shares available
```

The **best bid** is the highest price any buyer is currently willing to pay ($220.00). The **best ask** is the lowest price any seller is currently willing to accept ($220.50). The **gap** between them is the **spread** ($0.50). The **mid-price** is the simple average of best bid and best ask ($220.25).

This book changes hundreds or thousands of times per second as new orders arrive, existing orders are canceled, and trades execute.

#### What is NASDAQ ITCH 5.0?

ITCH 5.0 is the binary protocol that NASDAQ uses to broadcast the full state of the order book to subscribers in real-time. Think of it as a continuous stream of tiny update messages, each describing one event that changed the book:

- "A new buy order for 200 shares of AAPL at $220.00 arrived" → **Add Order message**
- "That order was canceled" → **Order Cancel message**
- "That order was fully executed, a trade happened" → **Order Executed message**
- "The order's quantity was reduced from 200 to 50 shares" → **Order Replace message**

Every message is binary encoded — not JSON, not text, not CSV. Raw bytes. Each message type has a specific layout defined in the ITCH 5.0 specification document (a publicly available PDF from NASDAQ). For example, an Add Order message is exactly 36 bytes long, with specific byte offsets for each field (message type, timestamp, order reference number, buy/sell flag, shares, stock symbol, price).

A full trading day on NASDAQ produces approximately 270 million ITCH messages totaling around 8–12 GB of binary data. NASDAQ publishes sample daily files for free at https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ — these are real historical files, not simulated data.

#### What is an Order Reference Number?

Each order has a unique 64-bit integer ID called an **Order Reference Number**. When NASDAQ sends an Add Order message, it includes this number. When that same order is later modified, partially filled, or canceled, subsequent messages reference the same Order Reference Number. Your system must maintain a hash map of `{OrderRefNumber → Order}` to track the state of every live order.

#### What is Spread, Mid-Price, and Microprice?

- **Spread** = Best Ask Price − Best Bid Price. A tight spread (small gap) means the market is liquid. A wide spread means it's harder to trade.
- **Mid-Price** = (Best Bid + Best Ask) / 2. The naive "fair price" estimate.
- **Microprice** = A smarter fair price estimate that weights the mid-price toward whichever side of the book has less volume. Invented by researcher Sasha Stoikov. The formula is:

$$\text{Microprice} = \text{Best Ask} \times \frac{V_{\text{bid}}}{V_{\text{bid}} + V_{\text{ask}}} + \text{Best Bid} \times \frac{V_{\text{ask}}}{V_{\text{bid}} + V_{\text{ask}}}$$

where $V_{\text{bid}}$ = volume at best bid, $V_{\text{ask}}$ = volume at best ask.

**Intuition:** If there are 1000 shares sitting at the best bid and only 100 at the best ask, buyers heavily outnumber sellers at the top of the book. The true price is probably closer to the ask than to the mid. The microprice captures this asymmetry. It is a better short-term price predictor than the mid-price and is used by real trading firms.

#### What is Order Book Imbalance (OBI)?

Order Book Imbalance is a number between -1 and +1 that measures whether the buy side or sell side of the book is currently dominant:

$$\text{OBI} = \frac{\text{BidVolume} - \text{AskVolume}}{\text{BidVolume} + \text{AskVolume}}$$

where BidVolume and AskVolume are the total shares at the best bid and best ask (or the top N price levels).

- **OBI = +1.0** → Huge buying pressure, almost no selling pressure. Price likely to move up.
- **OBI = -1.0** → Huge selling pressure, almost no buying pressure. Price likely to move down.
- **OBI = 0.0** → Balanced.

Research by Cont, Kukanov, and Stoikov (published 2010) shows that OBI has a near-linear relationship with short-horizon price changes. Trading firms use it as a real-time signal.

#### What is Trade Flow and VWAP?

**Trade Flow** tracks whether recent trades are predominantly buyer-initiated (someone aggressively buying into asks) or seller-initiated (someone aggressively selling into bids). This is a directional indicator.

**VWAP** (Volume-Weighted Average Price) is the average price of all trades weighted by their size. It is used as a benchmark by institutional traders and is easy to compute from execution messages.

---

## BACKGROUND: THE SYSTEMS CONCEPTS YOU NEED

#### Why is This a Hard Systems Problem?

At peak market hours, NASDAQ sends several hundred thousand to over a million messages per second. Each message must be:

- Read from the file (or network)
- Parsed into a structured C++ object
- Used to update the order book for the correct symbol
- Used to compute new signal values
- Timestamped at each step with nanosecond precision

The entire pipeline must process each message in well under 1 microsecond (1,000 nanoseconds) on average to keep up with live data. The bottlenecks are data structure choice, memory layout, and unnecessary allocations — not algorithm complexity.

This is the same class of problem HRT, Jump, and Optiver engineers work on every day.

#### What is Cache Efficiency and Why Does It Matter?

Modern CPUs are extremely fast at computation but slow at fetching memory. An L1 cache hit (data already in the CPU's closest cache) takes ~1 nanosecond. An L2 cache miss and main memory access takes ~100 nanoseconds — 100x slower.

If your order book stores prices in a `std::map<int, Level>` (a red-black tree), each node is a separate heap allocation at a random memory address. Traversing it causes cache misses on every step. This is slow.

If your order book stores prices in a flat array indexed by price integer (since NASDAQ prices are integers in units of 1/10000 of a dollar), all the data is contiguous in memory. The CPU can prefetch it efficiently. This is fast.

The choice between pointer-based trees and contiguous arrays is one of the most important design decisions in your project, and explaining it in an interview is exactly the kind of depth that HRT engineers want to hear.

#### What is a Memory Pool (Slab Allocator)?

`new` and `delete` in C++ call into the OS heap allocator, which is slow and non-deterministic (it might take 50ns, it might take 5000ns depending on fragmentation). In a hot path that processes millions of messages per second, you cannot afford that.

A memory pool pre-allocates a large chunk of memory upfront. When you need a new Order object, you don't call `new` — you grab the next slot from your pre-allocated pool in ~1ns. When you're done, you return it to the pool. No OS interaction, no fragmentation, deterministic latency.

#### What is a Lock-Free SPSC Queue?

**SPSC** = Single Producer, Single Consumer. A lock-free SPSC queue is a ring buffer that allows one thread to write messages and another thread to read them without any mutex locks. Mutexes are expensive because they can cause threads to sleep and wake up (context switches), adding microseconds of latency.

In this project, Stage 1 (the parser) writes parsed messages into an SPSC queue, and Stage 2 (the order book updater) reads from it. They run in parallel on separate CPU cores with no blocking.

#### What is RDTSC?

**RDTSC** (Read Time-Stamp Counter) is a CPU instruction that returns the number of clock cycles since the processor was last reset. It is the fastest possible way to measure time in C++ with nanosecond resolution.

```cpp
inline uint64_t rdtsc() {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
```

At 3 GHz, each cycle = ~0.33 nanoseconds. You can convert cycles to nanoseconds by dividing by your CPU frequency in GHz.

You stamp every message at the start of each pipeline stage (parse start, parse end, book update start, book update end, signal compute start, signal compute end) and collect distributions of latencies across millions of messages.

#### What is P50/P99/P99.9 Latency?

When you collect 270 million latency measurements, the distribution matters more than the average. Quant firms care about:

- **P50** (median): Half of messages processed faster than this. The "typical" case.
- **P99**: 99% of messages processed faster than this. The 1 in 100 worst case.
- **P99.9**: 999 out of 1000 messages processed faster than this. The 1 in 1000 worst case.

A system with P50=50ns but P99=5000ns has a latency spike problem — 1% of the time it's 100x slower. Real trading systems optimize P99 and P99.9 to minimize "jitter."

Your benchmark README will report latencies in this format, just like HRT's actual published engineering work.

---

## THE PIPELINE — WHAT HERMES BUILDS

```
┌─────────────────────────────────────────────────────────────────────┐
│                         HERMES PIPELINE                             │
│                                                                     │
│  [ITCH Binary File]                                                 │
│         │                                                           │
│         ▼                                                           │
│  ┌─────────────┐   RDTSC stamp    ┌──────────────┐                 │
│  │   Stage 1   │ ────────────────▶│  SPSC Queue  │                 │
│  │  ITCH Parser│                  │  (ring buffer│                 │
│  │             │                  │   in memory) │                 │
│  └─────────────┘                  └──────┬───────┘                 │
│                                          │                         │
│                                          ▼                         │
│                                   ┌─────────────┐  RDTSC stamp     │
│                                   │   Stage 2   │                  │
│                                   │  Order Book │                  │
│                                   │  Updater    │                  │
│                                   └──────┬──────┘                  │
│                                          │                         │
│                              After each update                     │
│                                          ▼                         │
│                                   ┌─────────────┐  RDTSC stamp     │
│                                   │   Stage 3   │                  │
│                                   │  Signal     │                  │
│                                   │  Computer   │                  │
│                                   └──────┬──────┘                  │
│                                          │                         │
│                                          ▼                         │
│                                   ┌─────────────┐                  │
│                                   │   Stage 4   │                  │
│                                   │  Latency    │                  │
│                                   │  Profiler   │                  │
│                                   └─────────────┘                  │
│                                          │                         │
│                                          ▼                         │
│                              [Benchmark Report Output]             │
│                       P50/P99/P99.9 per stage, histogram           │
└─────────────────────────────────────────────────────────────────────┘
```

### Stage 1: ITCH Binary Parser

**What it does:** Opens the NASDAQ ITCH binary file, reads it in large chunks, and for each message, reads the 1-byte message type, then reads exactly the right number of bytes for that message type, and fills a C++ struct.

**Key message types to handle:**

| Message Type | Byte Code | What it means | What you do with it |
|---|---|---|---|
| Stock Directory | `R` | Maps stock locate code (integer) to symbol (e.g., "AAPL") | Store in symbol lookup table |
| Add Order | `A` or `F` | New order placed on book | Add to order map + order book |
| Order Cancel | `X` | Existing order's quantity reduced | Update order in book |
| Order Delete | `D` | Existing order removed entirely | Remove from order map + book |
| Order Replace | `U` | Order modified (cancel + re-add) | Update both maps |
| Order Executed | `E` | Order partially or fully filled | Reduce quantity, record trade |
| Order Executed with Price | `C` | Same but at a specific price | Same + update trade data |
| Trade (non-displayable) | `P` | Trade that didn't involve book orders | Record trade |

**Key implementation detail:** Every field in an ITCH message is big-endian (network byte order). x86 CPUs are little-endian. You must byte-swap every integer field using `__builtin_bswap16`, `__builtin_bswap32`, `__builtin_bswap64`. This is a common source of bugs for first-time implementers.

**Prices:** NASDAQ prices are transmitted as integers with 4 implied decimal places. So $220.5000 is transmitted as the integer `2205000`. You store them as integers throughout — never convert to float in the hot path.

### Stage 2: Order Book Updater

**What it does:** Maintains one order book per symbol (up to ~8,000 symbols on NASDAQ). For each book, maintains two sides (bid and ask), where each side maps a price level to the total quantity at that level.

**Data structure choice (the critical design decision):**

The **naive approach**: `std::map<int, int>` for each price level. A red-black tree. Cache-unfriendly, pointer-chasing, slow.

The **fast approach**: A flat array indexed by price. Since NASDAQ integer prices (in units of $0.0001) fit in a reasonable range for any given stock during a session, you can use an offset array: `quantity[price - min_price]`. Cache-hot, no pointer chasing, O(1) access. You also maintain `best_bid` and `best_ask` pointers into this array that update incrementally.

**Additionally:** a hash map `std::unordered_map<uint64_t, Order>` from Order Reference Number to the Order struct, so you can look up any live order in O(1) when a modify/cancel/execute message arrives.

**Memory pool:** All `Order` objects are allocated from a pre-allocated pool of 2 million slots (covers the max live orders on any given day). No heap allocation during the trading session.

### Stage 3: Signal Computer

After each order book update, compute 4 signals for the affected symbol. These are computed per tick (i.e., every time the book changes) and stored in a circular buffer for later analysis:

**Signal 1 — Spread:**
```
spread = best_ask_price - best_bid_price
```
In integer price units.

**Signal 2 — Mid-Price:**
```
mid_price = (best_bid_price + best_ask_price) / 2
```

**Signal 3 — Order Book Imbalance (OBI):**
```
bid_vol = total_quantity_at_best_bid
ask_vol = total_quantity_at_best_ask
OBI = (bid_vol - ask_vol) / (bid_vol + ask_vol)  [as float]
```

Compute this for the top 5 price levels (not just best bid/ask) for a more robust signal. Depth-weighted OBI uses:
```
OBI_depth = sum(bid_vol[i] for i in top 5 levels) / total
           - sum(ask_vol[i] for i in top 5 levels) / total
```

**Signal 4 — Microprice:**
```
microprice = (best_ask_price × bid_vol + best_bid_price × ask_vol)
             / (bid_vol + ask_vol)
```

Store these as `int64_t` (integer arithmetic only) in the hot path. Convert to float only when writing to the output log.

### Stage 4: Latency Profiler

Stamp every message at 6 checkpoints with `rdtsc()`:

1. Message read complete (T0)
2. Message parse complete (T1)
3. Order map lookup/update complete (T2)
4. Order book update complete (T3)
5. Signal computation complete (T4)
6. Write to output buffer complete (T5)

Collect `T1-T0`, `T2-T1`, `T3-T2`, `T4-T3` across all messages. At the end, compute:

- P50, P95, P99, P99.9 for each stage
- A latency histogram in 10ns buckets from 0 to 2000ns
- Total message throughput (messages per second)

Output a clean benchmark table like:

```
Stage              P50      P99      P99.9    Throughput
──────────────────────────────────────────────────────
Parse              32 ns    91 ns    280 ns   31.2M msg/s
Book Update        44 ns    120 ns   410 ns   22.7M msg/s
Signal Compute     8 ns     19 ns    45 ns    125M msg/s
Full Pipeline      89 ns    241 ns   780 ns   11.2M msg/s
```

---

## COMPLETE TECHNICAL SPECIFICATION

### Language and Toolchain

- **Language:** C++17 (or C++20)
- **Compiler:** GCC or Clang on Linux (Ubuntu recommended). Must compile with `-O2` or `-O3`
- **Build system:** CMake
- **Testing:** Google Test or Catch2 for unit tests on the parser and order book logic
- **Benchmarking:** Your own RDTSC-based profiler (no external benchmarking frameworks)
- **Platform:** Linux. This is important. `rdtsc`, CPU affinity (`pthread_setaffinity_np`), and hugepages are Linux-specific. Use WSL2 on Windows or a Linux VM.

### Dependencies (Minimal by Design)

- C++ standard library only for core logic
- `pthread` for threading
- Optional: `google-benchmark` for cross-validation of your RDTSC measurements
- Optional: Python + matplotlib for histogram visualization of your latency distributions

### Directory Structure

```
hermes/
├── CMakeLists.txt
├── README.md                  ← The most important file for recruiters
├── include/
│   ├── itch_parser.hpp        ← Message structs + parser interface
│   ├── order_book.hpp         ← Order book data structure
│   ├── signal_engine.hpp      ← Signal computation
│   ├── latency_profiler.hpp   ← RDTSC-based timestamping
│   └── memory_pool.hpp        ← Slab allocator
├── src/
│   ├── itch_parser.cpp
│   ├── order_book.cpp
│   ├── signal_engine.cpp
│   ├── latency_profiler.cpp
│   └── main.cpp               ← Pipeline orchestration
├── tests/
│   ├── test_parser.cpp
│   ├── test_order_book.cpp
│   └── test_signals.cpp
├── benchmarks/
│   └── bench_main.cpp         ← Standalone benchmark runner
└── docs/
    ├── bench_results.md       ← Benchmark results with hardware info
    └── architecture.md        ← Design decisions explained
```

### Core Data Structures

**Order struct (hot path — keep it small):**

```cpp
struct Order {
    uint64_t ref_number;   // 8 bytes
    uint32_t price;        // 4 bytes (integer, 4 decimal places)
    uint32_t quantity;     // 4 bytes
    uint16_t locate;       // 2 bytes (symbol ID)
    uint8_t  side;         // 1 byte (0=bid, 1=ask)
    uint8_t  pad;          // 1 byte padding
};                         // Total: 20 bytes — fits in cache line
```

**Price Level (in the order book array):**

```cpp
struct Level {
    uint32_t quantity;     // Total shares at this price
    uint32_t order_count;  // Number of orders at this price
};                         // 8 bytes
```

**Order Book for one symbol:**

```cpp
struct OrderBook {
    uint16_t locate;
    char     symbol[9];

    // Array-based price levels (cache-friendly)
    // Indexed by (price - base_price)
    Level    bids[MAX_LEVELS];   // Descending prices
    Level    asks[MAX_LEVELS];   // Ascending prices

    uint32_t best_bid_idx;       // Index into bids array
    uint32_t best_ask_idx;       // Index into asks array

    // Signals (updated every tick)
    int64_t  microprice;
    float    obi;
    uint32_t spread;
};
```

**Top-level state:**

```cpp
struct MarketState {
    // Symbol lookup: locate code -> OrderBook
    OrderBook books[MAX_SYMBOLS];    // ~8000 symbols, array-indexed by locate

    // Order tracking: ref number -> Order
    // Use a hash map here — open addressing, power-of-2 table size
    Order     order_pool[MAX_ORDERS];  // Pre-allocated pool
    // Hash map: ref_number -> index into order_pool
};
```

### Performance Targets

These are the numbers you should aim for and document. They are achievable on a modern laptop running Linux:

| Metric | Target |
|---|---|
| Full pipeline P50 latency | < 150 ns |
| Full pipeline P99 latency | < 500 ns |
| Parser throughput | > 20M messages/sec |
| Total session processing time | < 15 seconds for a full day file |
| Memory usage | < 2 GB for full session |

If you hit these targets and document them clearly, you have a resume bullet like:

> "Built a C++ NASDAQ ITCH 5.0 market data pipeline processing 270M messages with P50 parse latency of 89ns; profiled hot-path bottlenecks using RDTSC cycle counters and eliminated cache misses via array-indexed order book, reducing P99 from 420ns to 115ns."

---

## BUILD ORDER — HOW TO APPROACH THIS IN PHASES

#### Phase 1 — Message Counter (Week 1)

**Goal:** Open the binary file. Read it byte by byte. Count how many of each message type exist. Print a histogram. This proves you can read the binary format and parse the 1-byte type field correctly.

**Output:** A number like "268,744,780 total messages: 152M Add Orders, 114M Deletes, 5.8M Executions..."

#### Phase 2 — Full Parser with Structs (Weeks 2–3)

**Goal:** Parse every relevant message type into a C++ struct. Verify your byte-swapping is correct. Write unit tests that parse known bytes and check field values against the ITCH spec.

**Output:** A working parser that can decode all message types. No order book yet.

#### Phase 3 — Order Book (Weeks 3–5)

**Goal:** Build the order book data structure. Process Add/Delete/Cancel/Replace/Execute messages to update it. After processing every message, verify that `best_bid < best_ask` always holds (a basic sanity check). For 5 symbols (AAPL, MSFT, TSLA, AMZN, NVDA), dump the top 5 bid/ask levels every 1 million messages and manually check they look reasonable.

**Output:** A working, verified order book for all symbols.

#### Phase 4 — Signal Computation (Week 5)

**Goal:** After each order book update, compute OBI, microprice, spread, mid-price. Write them to a CSV file for a single symbol over the full trading day. Plot them in Python. Visually inspect that they move sensibly (microprice should be between bid and ask, OBI should swing between -1 and +1).

**Output:** A chart of intraday signal behavior for AAPL.

#### Phase 5 — Latency Profiler (Weeks 6–7)

**Goal:** Add RDTSC timestamps at each stage. Collect distributions. Output the benchmark table. Then **optimize** at least one bottleneck you find. Document what you changed and why the hardware behaved the way it did.

**Output:** Before/after benchmark table. This is the most important deliverable.

#### Phase 6 — README and Polish (Week 8)

**Goal:** Write the README. It must contain: project overview, architecture diagram, design decisions (why array-indexed book vs. map), benchmark results with hardware specs (CPU model, frequency, cache sizes), and the before/after optimization story.

---

## THE README — WHAT MAKES IT IMPRESSIVE

Your README is what a recruiter reads before your code. It must contain these sections:

1. **Project overview** — 3 sentences. What it is, what data it uses, what the output is.
2. **Architecture diagram** — ASCII is fine. Show the pipeline stages.
3. **Design decisions** — Why array-indexed order book? Why RDTSC over `chrono`? Why SPSC queue? Each decision should have a one-paragraph explanation that shows hardware understanding.
4. **Benchmark results** — The table above, plus: what CPU you ran it on (include model, GHz, cache sizes), what optimization you made, what changed.
5. **How to build and run** — `cmake -B build && cmake --build build && ./build/hermes /path/to/ITCH_file`
6. **Data source** — How to download a real NASDAQ ITCH file.
7. **What I learned** — Optional but powerful: a 3-paragraph section on what you learned about hardware performance engineering.

---

## DATA SOURCE

Download the real ITCH data here:
```
https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
```

The files are named like `10302019.NASDAQ_ITCH50.gz`. Download any one. Unzip it. It will be 8–12 GB. You use this actual real file as input to your program. This is real NASDAQ market data from a real trading day.

---

## HOW THIS MAPS TO WHAT HRT ACTUALLY DOES

For context on why this project is directly relevant, HRT's trading technology group does exactly this in production. Their engineers:

- Parse high-speed binary market data from exchanges using DPDK (kernel-bypass networking)
- Maintain real-time order books for every symbol on every exchange they trade
- Compute signals from order book state to drive trading decisions
- Profile every nanosecond of their pipeline to eliminate latency

The difference between Hermes and what HRT runs in production is: (1) they use live UDP multicast feeds over DPDK instead of a file, (2) their order books handle multiple exchanges simultaneously, (3) their signals are proprietary. The architecture is identical. You are building a scaled-down version of real trading infrastructure.

---

## WHAT TO SAY IN AN INTERVIEW

When an interviewer at HRT or Optiver asks "tell me about your projects," here is the structure of your answer:

**What it is:** "I built a C++ market data pipeline that parses real NASDAQ ITCH binary data — 270 million messages from a full trading day — reconstructs order books for every symbol, and computes microstructure signals."

**The interesting engineering problem:** "The bottleneck was the order book update. Initially I used std::unordered_map for price levels, which caused cache misses on every lookup. I replaced it with a flat array indexed by integer price and reduced P99 latency from [X] to [Y]."

**How you measured it:** "I used RDTSC cycle counters to timestamp each stage of the pipeline across all 270 million messages and collected P50/P99/P99.9 distributions. That's how I identified the hash map as the bottleneck — it showed up clearly in the P99 tail."

**What you learned:** "I learned that on modern hardware, cache behavior dominates. Algorithmic complexity matters much less than memory access patterns for data structures at this scale."

---

## SCOPE BOUNDARIES — WHAT THIS PROJECT IS NOT

- It is **not** a trading strategy. You are not predicting prices or simulating trades.
- It is **not** a backtester. You are not evaluating P&L.
- It is **not** a live trading system. No network, no exchange connectivity.
- It is **not** a machine learning project (that's Project Pulse — a future extension).

Keep it focused. The narrower the scope, the deeper the engineering, the more impressive it is.

---

## Current Status

**Phase 4 complete (2026-03-29).** Signal engine with AAPL CSV output.
- `compute_signals()`: spread, mid-price, microprice (Stoikov), OBI over top-5 non-empty levels
- All integer arithmetic in hot path; OBI as float; int32_t bid loop prevents uint32_t underflow
- 12 unit tests, 37/37 total passing
- `aapl_signals.csv`: 1,177,357 rows across full trading day; 0 OBI anomalies, 0 spread≤0

**Next: Phase 5** — RDTSC latency profiler (pin to P-core on Core Ultra 7 155H, P50/P99/P99.9 per stage).

**Phase 3 complete (2026-03-28).** Array-indexed order book with window rebasing.
- Flat `Level[8192]` arrays per side, O(1) add/remove, incremental best_bid/ask tracking
- Separate `bid_base`/`ask_base` per symbol; `rebase_side()` slides window via memmove when price drifts out of range
- Generation counters (`bid_gen`/`ask_gen`) prevent stale removes after full-clear rebase
- All 7 message types handled: AddOrder, AddOrderMPID, Delete, Cancel, Replace, Executed, ExecutedPrice
- `main.cpp` prints top-5 bid/ask for AAPL/MSFT/TSLA/AMZN/NVDA every 1M messages
- 25/25 tests passing; 263M messages, 0 underflows on real ITCH file; debug build clean

**Phase 2 complete (2026-03-28).** Full ITCH parser with structs, byte-swapping, and unit tests.
- All 9 message types parsed and tested field-by-field (A, F, E, C, X, D, U, P, R)
- Counts verified against Phase 1 output: 264,469,445 handled messages match exactly
- Google Test via FetchContent, CMake build system in place
- 10/10 tests passing (9 unit + 1 integration)
- All code in `include/itch_parser.hpp`, `src/itch_parser.cpp`, `src/main.cpp`, `tests/test_parser.cpp`

**Next: Phase 4** — Signal computation (OBI, microprice, spread, mid-price).

