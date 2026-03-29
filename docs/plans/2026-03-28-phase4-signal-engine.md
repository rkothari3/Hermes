# Phase 4: Signal Engine Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** After each order book update, compute four microstructure signals (spread, mid-price, OBI, microprice) for the affected symbol and emit them to a per-symbol CSV file for analysis.

**Architecture:** Add a `signal_engine` module (header + source) that takes a const `OrderBook*` and returns a `Signals` struct. Wire it into `main.cpp` so it is called after every book-updating message for AAPL only (the largest symbol by message count). Write signals to `aapl_signals.csv` — one row per book update. All arithmetic stays in integer units (price × 10000) in the hot path; float conversion only happens at CSV write time.

**Tech Stack:** C++17, Google Test, CMake (same build as Phase 3). No new dependencies.

---

## Background: The Four Signals

Prices in this codebase are stored as **integers with 4 implied decimal places**: $291.45 = `2914500`. All signal arithmetic operates on these integers.

**1. Spread**
```
spread = best_ask_price - best_bid_price   (integer ticks)
```
e.g. $291.49 - $291.45 = integer 4 → printed as "$0.0004"

**2. Mid-price**
```
mid_price = (best_bid_price + best_ask_price) / 2   (integer, truncated)
```

**3. Microprice** (Stoikov)
```
bid_vol = bids[best_bid_idx].quantity
ask_vol = asks[best_ask_idx].quantity
microprice = (best_ask_price * bid_vol + best_bid_price * ask_vol) / (bid_vol + ask_vol)
```
Integer division, result in same tick units. Rounded by truncation.

**4. Order Book Imbalance (OBI)** — computed over top-5 levels
```
total_bid = sum of bids[best_bid_idx .. best_bid_idx-4].quantity (up to 5 non-empty)
total_ask = sum of asks[best_ask_idx .. best_ask_idx+4].quantity (up to 5 non-empty)
OBI = (total_bid - total_ask) / (total_bid + total_ask)   [float, range -1..+1]
```
If either total is zero, OBI = 0.0.

---

## Scene-Setting for Implementers

**Where you are:** `include/order_book.hpp` has `OrderBook` (bids/asks arrays, best_bid_idx, best_ask_idx, bid_base, ask_base, bid_initialized, ask_initialized). `src/order_book.cpp` has all message handlers. `src/main.cpp` has `PipelineState` + per-symbol message callbacks. Tests live in `tests/`.

**What you will add:**
- `include/signal_engine.hpp` — `Signals` struct + `compute_signals()` declaration
- `src/signal_engine.cpp` — implementation of `compute_signals()`
- `tests/test_signals.cpp` — unit tests (added to `hermes_tests` target)
- `CMakeLists.txt` — add `src/signal_engine.cpp` to `hermes_lib`, add `tests/test_signals.cpp` to `hermes_tests`
- `src/main.cpp` — call `compute_signals()` after each AAPL book update; open/close CSV file; write one row per update

**Build command (run from worktree root):**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
```

**Test command:**
```bash
./build/hermes_tests
```

---

## Task 1: Define `Signals` struct and stub header

**Files:**
- Create: `include/signal_engine.hpp`

**Step 1: Create the header**

```cpp
// include/signal_engine.hpp
#pragma once
#include <cstdint>
#include "order_book.hpp"

struct Signals {
    uint32_t spread;       // best_ask - best_bid (integer ticks)
    uint32_t mid_price;    // (best_bid + best_ask) / 2 (integer, truncated)
    uint32_t microprice;   // Stoikov microprice (integer, truncated)
    float    obi;          // Order book imbalance [-1, +1]
    bool     valid;        // false if book has no two-sided quote
};

// Compute all four signals from the current state of book.
// Returns Signals with valid=false if either side has no quote.
Signals compute_signals(const OrderBook* book);
```

**Step 2: Verify it compiles (no source file yet)**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release 2>&1 | grep error
```
Expected: no errors (header-only change, nothing links against it yet)

**Step 3: Commit**

```bash
git add include/signal_engine.hpp
git commit -m "feat: add Signals struct and compute_signals declaration"
```

---

## Task 2: Implement `compute_signals()`

**Files:**
- Create: `src/signal_engine.cpp`
- Modify: `CMakeLists.txt` (add source to hermes_lib)

**Step 1: Create the implementation**

```cpp
// src/signal_engine.cpp
#include "signal_engine.hpp"

Signals compute_signals(const OrderBook* book) {
    Signals s{};

    if (!book->bid_initialized || !book->ask_initialized) return s;

    bool has_bid = book->bids[book->best_bid_idx].quantity > 0;
    bool has_ask = book->asks[book->best_ask_idx].quantity > 0;
    if (!has_bid || !has_ask) return s;

    uint32_t best_bid = book->bid_base + book->best_bid_idx;
    uint32_t best_ask = book->ask_base + book->best_ask_idx;
    if (best_bid >= best_ask) return s;   // crossed/locked book

    // Spread
    s.spread = best_ask - best_bid;

    // Mid-price
    s.mid_price = (best_bid + best_ask) / 2;

    // Microprice — guard against zero-denominator (shouldn't happen given has_bid/has_ask)
    uint32_t bid_vol = book->bids[book->best_bid_idx].quantity;
    uint32_t ask_vol = book->asks[book->best_ask_idx].quantity;
    uint64_t denom   = (uint64_t)bid_vol + ask_vol;
    s.microprice = (uint32_t)(((uint64_t)best_ask * bid_vol +
                                (uint64_t)best_bid * ask_vol) / denom);

    // OBI over top-5 bid and ask levels
    uint64_t total_bid = 0, total_ask = 0;
    uint32_t count = 0;
    for (uint32_t i = book->best_bid_idx; count < 5; --i) {
        total_bid += book->bids[i].quantity;
        ++count;
        if (i == 0) break;
    }
    count = 0;
    for (uint32_t i = book->best_ask_idx; i < MAX_LEVELS && count < 5; ++i) {
        total_ask += book->asks[i].quantity;
        ++count;
    }
    uint64_t total = total_bid + total_ask;
    s.obi   = (total > 0) ? (float)((int64_t)total_bid - (int64_t)total_ask) / (float)total : 0.0f;
    s.valid = true;
    return s;
}
```

**Step 2: Add source to CMakeLists.txt**

Find the `add_library(hermes_lib ...)` block and add `src/signal_engine.cpp`:

```cmake
add_library(hermes_lib
    src/itch_parser.cpp
    src/order_book.cpp
    src/signal_engine.cpp
)
```

**Step 3: Verify it compiles**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) 2>&1 | grep error
```
Expected: no errors.

**Step 4: Commit**

```bash
git add src/signal_engine.cpp CMakeLists.txt
git commit -m "feat: implement compute_signals (spread, mid, microprice, OBI)"
```

---

## Task 3: TDD — unit tests for `compute_signals()`

**Files:**
- Create: `tests/test_signals.cpp`
- Modify: `CMakeLists.txt` (add test file to hermes_tests)

**Step 1: Add test file to CMakeLists.txt**

Find the `add_executable(hermes_tests ...)` block and add the new test file:

```cmake
add_executable(hermes_tests
    tests/test_parser.cpp
    tests/test_order_book.cpp
    tests/test_signals.cpp
)
```

**Step 2: Write the tests**

```cpp
// tests/test_signals.cpp
#include <gtest/gtest.h>
#include "signal_engine.hpp"
#include "order_book.hpp"
#include <cstring>

// Helper: build a minimal AddOrder
static AddOrder make_add(uint16_t locate, uint64_t ref, uint32_t price,
                         uint32_t shares, uint8_t side) {
    AddOrder msg{};
    msg.locate    = locate;
    msg.order_ref = ref;
    msg.price     = price;
    msg.shares    = shares;
    msg.side      = side;
    memcpy(msg.stock, "TEST    ", 8);
    msg.stock[8]  = '\0';
    return msg;
}

static StockDirectory make_dir(uint16_t locate, const char* symbol) {
    StockDirectory msg{};
    msg.locate = locate;
    strncpy(msg.stock, symbol, 8);
    msg.stock[8] = '\0';
    return msg;
}

class SignalTest : public ::testing::Test {
protected:
    MarketState* ms;
    void SetUp()    override { ms = create_market_state(); }
    void TearDown() override { destroy_market_state(ms); }
};

// ── valid=false cases ─────────────────────────────────────────────────────────

TEST_F(SignalTest, EmptyBook_ReturnsInvalid) {
    handle_stock_directory(ms, make_dir(1, "TEST"));
    const OrderBook* book = ms->books[1];
    Signals s = compute_signals(book);
    EXPECT_FALSE(s.valid);
}

TEST_F(SignalTest, OnlyBidSide_ReturnsInvalid) {
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 100, 'B'));
    const OrderBook* book = ms->books[1];
    Signals s = compute_signals(book);
    EXPECT_FALSE(s.valid);
}

TEST_F(SignalTest, OnlyAskSide_ReturnsInvalid) {
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000100, 100, 'S'));
    const OrderBook* book = ms->books[1];
    Signals s = compute_signals(book);
    EXPECT_FALSE(s.valid);
}

// ── Spread ────────────────────────────────────────────────────────────────────

TEST_F(SignalTest, Spread_IsAskMinusBid) {
    // bid = $100.0000 = 1000000, ask = $100.0500 = 1000500
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 100, 'B'));
    handle_add_order(ms, make_add(1, 2, 1000500, 100, 'S'));
    Signals s = compute_signals(ms->books[1]);
    ASSERT_TRUE(s.valid);
    EXPECT_EQ(s.spread, 500u);   // 500 ticks = $0.0500
}

// ── Mid-price ─────────────────────────────────────────────────────────────────

TEST_F(SignalTest, MidPrice_IsAverageOfBidAsk) {
    // bid = 1000000, ask = 1000100 → mid = 1000050
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 100, 'B'));
    handle_add_order(ms, make_add(1, 2, 1000100, 100, 'S'));
    Signals s = compute_signals(ms->books[1]);
    ASSERT_TRUE(s.valid);
    EXPECT_EQ(s.mid_price, 1000050u);
}

TEST_F(SignalTest, MidPrice_TruncatesOddSpread) {
    // bid = 1000000, ask = 1000101 → mid = 1000050 (truncated, not rounded)
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 100, 'B'));
    handle_add_order(ms, make_add(1, 2, 1000101, 100, 'S'));
    Signals s = compute_signals(ms->books[1]);
    ASSERT_TRUE(s.valid);
    EXPECT_EQ(s.mid_price, 1000050u);
}

// ── Microprice ────────────────────────────────────────────────────────────────

TEST_F(SignalTest, Microprice_EqualVolumes_EqualsMid) {
    // Equal volume on both sides → microprice = mid
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 100, 'B'));
    handle_add_order(ms, make_add(1, 2, 1000100, 100, 'S'));
    Signals s = compute_signals(ms->books[1]);
    ASSERT_TRUE(s.valid);
    EXPECT_EQ(s.microprice, s.mid_price);
}

TEST_F(SignalTest, Microprice_HeavyBidSide_PulledTowardAsk) {
    // bid_vol=900, ask_vol=100 → price pulled toward ask
    // microprice = (ask * bid_vol + bid * ask_vol) / (bid_vol + ask_vol)
    //            = (1000100 * 900 + 1000000 * 100) / 1000
    //            = (900090000 + 100000000) / 1000 = 1000090
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 900, 'B'));
    handle_add_order(ms, make_add(1, 2, 1000100, 100, 'S'));
    Signals s = compute_signals(ms->books[1]);
    ASSERT_TRUE(s.valid);
    EXPECT_EQ(s.microprice, 1000090u);
    EXPECT_GT(s.microprice, s.mid_price);   // pulled toward ask
}

// ── OBI ───────────────────────────────────────────────────────────────────────

TEST_F(SignalTest, OBI_EqualVolumes_IsZero) {
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 100, 'B'));
    handle_add_order(ms, make_add(1, 2, 1000100, 100, 'S'));
    Signals s = compute_signals(ms->books[1]);
    ASSERT_TRUE(s.valid);
    EXPECT_FLOAT_EQ(s.obi, 0.0f);
}

TEST_F(SignalTest, OBI_AllBid_IsPlusOne) {
    // Only bid volume (total_ask = 0 in top-5) → OBI = 1.0
    // Use a very large ask so it gets skipped in top-5? No — top-5 just sums
    // the nearest 5 levels. For simplicity add a tiny ask and huge bid.
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 10000, 'B'));
    handle_add_order(ms, make_add(1, 2, 1000100, 1,     'S'));
    Signals s = compute_signals(ms->books[1]);
    ASSERT_TRUE(s.valid);
    // OBI = (10000 - 1) / (10000 + 1) ≈ 0.9998, not exactly 1
    EXPECT_GT(s.obi, 0.99f);
}

TEST_F(SignalTest, OBI_HeavyAskSide_IsNegative) {
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 100,  'B'));
    handle_add_order(ms, make_add(1, 2, 1000100, 1000, 'S'));
    Signals s = compute_signals(ms->books[1]);
    ASSERT_TRUE(s.valid);
    EXPECT_LT(s.obi, 0.0f);
    // OBI = (100 - 1000) / (100 + 1000) = -900/1100 ≈ -0.8182
    EXPECT_NEAR(s.obi, -900.0f / 1100.0f, 1e-5f);
}

TEST_F(SignalTest, OBI_Top5Levels_SumsCorrectly) {
    // 3 bid levels + 2 ask levels — verify OBI uses all of them
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 100, 'B'));   // best bid
    handle_add_order(ms, make_add(1, 2,  999900, 200, 'B'));   // 2nd bid
    handle_add_order(ms, make_add(1, 3,  999800, 300, 'B'));   // 3rd bid
    handle_add_order(ms, make_add(1, 4, 1000100, 400, 'S'));   // best ask
    handle_add_order(ms, make_add(1, 5, 1000200, 500, 'S'));   // 2nd ask
    // total_bid = 100+200+300 = 600, total_ask = 400+500 = 900
    // OBI = (600-900)/(600+900) = -300/1500 = -0.2
    Signals s = compute_signals(ms->books[1]);
    ASSERT_TRUE(s.valid);
    EXPECT_NEAR(s.obi, -0.2f, 1e-5f);
}
```

**Step 3: Run tests (expect failure — signal_engine not in test build yet)**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) 2>&1 | grep error
```
Expected: compile errors about `compute_signals` undefined, OR compilation succeeds because CMakeLists.txt already links hermes_lib.

Actually at this point the CMakeLists.txt was updated in Task 2 to include `signal_engine.cpp` in `hermes_lib`, so `hermes_tests` already links it. The tests should just compile and run.

```bash
./build/hermes_tests 2>&1 | tail -10
```
Expected: all new signal tests PASS (implementation already in place from Task 2).

**Step 4: Commit**

```bash
git add tests/test_signals.cpp CMakeLists.txt
git commit -m "test: TDD signal engine — 12 tests passing"
```

---

## Task 4: Wire signals into `main.cpp` and write AAPL CSV

**Files:**
- Modify: `src/main.cpp`

**Goal:** After every AAPL book-updating message, call `compute_signals()`. If `valid`, write one CSV row to `aapl_signals.csv`.

**CSV format** (header + one row per valid update):
```
msg_count,spread,mid_price,microprice,obi
1000042,4,2914502,2914503,0.312500
```
- `spread`, `mid_price`, `microprice` are raw integer tick values (divide by 10000 for dollars)
- `obi` is a float, 6 decimal places

**Step 1: Modify `main.cpp`**

Add `#include "signal_engine.hpp"` and `#include <cstdio>` is already present.

Add a `FILE* csv_fp` to `PipelineState`:

```cpp
struct PipelineState {
    MarketState* ms;
    uint64_t     msg_count = 0;
    uint16_t locate_aapl = 0, locate_msft = 0, locate_tsla = 0,
             locate_amzn = 0, locate_nvda = 0;
    bool     have_aapl = false, have_msft = false, have_tsla = false,
             have_amzn = false, have_nvda = false;
    FILE*    csv_fp = nullptr;
};
```

Open the CSV file in `main()` before `parse_file()`:

```cpp
ps.csv_fp = fopen("aapl_signals.csv", "w");
if (!ps.csv_fp) { fprintf(stderr, "Cannot open aapl_signals.csv\n"); return 1; }
fprintf(ps.csv_fp, "msg_count,spread,mid_price,microprice,obi\n");
```

Add a helper called after each AAPL update (call it from each AAPL-affecting callback — since we don't know which locate an arbitrary message affects, just call it unconditionally after every book-updating message that could be AAPL):

Actually, AAPL is identified by `ps->locate_aapl`. After every book-updating callback, add a call to a helper `maybe_write_signals(ps)`:

```cpp
static void maybe_write_signals(PipelineState* ps) {
    if (!ps->have_aapl || !ps->csv_fp) return;
    const OrderBook* book = ps->ms->books[ps->locate_aapl];
    if (!book) return;
    Signals s = compute_signals(book);
    if (!s.valid) return;
    fprintf(ps->csv_fp, "%llu,%u,%u,%u,%.6f\n",
            (unsigned long long)ps->msg_count,
            s.spread, s.mid_price, s.microprice, (double)s.obi);
}
```

Call `maybe_write_signals(ps)` at the end of every book-updating callback (after `maybe_snapshot(ps)`).

Close the file at the end of `main()`:

```cpp
if (ps.csv_fp) fclose(ps.csv_fp);
```

The full diff to `main.cpp`:

```cpp
// At top, add:
#include "signal_engine.hpp"

// In PipelineState struct, add after have_nvda:
FILE*    csv_fp = nullptr;

// New static helper (add before maybe_snapshot):
static void maybe_write_signals(PipelineState* ps) {
    if (!ps->have_aapl || !ps->csv_fp) return;
    const OrderBook* book = ps->ms->books[ps->locate_aapl];
    if (!book) return;
    Signals s = compute_signals(book);
    if (!s.valid) return;
    fprintf(ps->csv_fp, "%llu,%u,%u,%u,%.6f\n",
            (unsigned long long)ps->msg_count,
            s.spread, s.mid_price, s.microprice, (double)s.obi);
}

// In main(), before parse_file():
ps.csv_fp = fopen("aapl_signals.csv", "w");
if (!ps.csv_fp) { fprintf(stderr, "Cannot open aapl_signals.csv\n"); return 1; }
fprintf(ps.csv_fp, "msg_count,spread,mid_price,microprice,obi\n");

// In each book-updating callback, add after maybe_snapshot(ps):
maybe_write_signals(ps);

// In main(), before destroy_market_state():
if (ps.csv_fp) fclose(ps.csv_fp);
```

**Step 2: Build**

```bash
cmake --build build -j$(nproc) 2>&1 | grep error
```
Expected: no errors.

**Step 3: Run tests**

```bash
./build/hermes_tests 2>&1 | tail -5
```
Expected: all tests still pass.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: wire compute_signals into main.cpp, write aapl_signals.csv"
```

---

## Task 5: Full-file run + sanity check

**Goal:** Run against the real ITCH file. Verify `aapl_signals.csv` looks correct. Check that `microprice` is always between `bid` and `ask`, OBI is in [-1, +1], spread is positive.

**Step 1: Run**

```bash
./build/hermes /mnt/c/Users/rajg6/OneDrive/Desktop/Hermes/12302019.NASDAQ_ITCH50
```
Expected: program completes, prints snapshot table, prints "Done. 263241937 book-updating messages processed."

**Step 2: Check CSV**

```bash
# Count rows
wc -l aapl_signals.csv

# First 5 rows
head -6 aapl_signals.csv

# Check microprice is between mid_price and closest price
# Check no OBI outside [-1, +1] — filter anomalous rows
awk -F',' 'NR>1 && ($5 < -1.0 || $5 > 1.0) {print NR, $0}' aapl_signals.csv | head

# Spot check a few rows around msg_count 50M, 100M, 150M
awk -F',' 'NR>1 && $1 > 50000000 && $1 < 50001000' aapl_signals.csv | head -5
```

Expected:
- `wc -l` shows millions of rows (all AAPL two-sided-quote updates)
- `awk` OBI check shows zero anomalous rows
- Prices look reasonable (AAPL traded $286-$293 on Dec 30 2019)

**Step 3: Commit**

```bash
git add aapl_signals.csv
```

Wait — the CSV is a large data file (~hundreds of MB). Do NOT commit it to git. Instead, just document the results.

```bash
git commit --allow-empty -m "test: full-file signal run verified — 0 OBI anomalies, prices in range"
```

Actually skip the empty commit. Just proceed to Task 6.

---

## Task 6: Merge to main

**Step 1: Run full test suite one final time**

```bash
./build/hermes_tests 2>&1 | tail -5
```
Expected: all tests pass.

**Step 2: Check for any untracked files that should not be committed**

```bash
git status
```

Do NOT commit `aapl_signals.csv`, `build/`, or `build_debug/`. Add to `.gitignore` if needed.

**Step 3: Verify .gitignore covers data files**

```bash
grep "aapl_signals" .gitignore || echo "ADD IT"
```

If not present, add `aapl_signals.csv` to `.gitignore`:
```bash
echo "aapl_signals.csv" >> .gitignore
git add .gitignore
git commit -m "chore: ignore aapl_signals.csv output file"
```

**Step 4: Confirm all signal-related changes are committed**

```bash
git log --oneline -6
```

**Step 5: Switch to main branch and merge**

(This is done by the controller/user, not by the implementer in the worktree.)

---

## Appendix: What the signals mean in plain English

- **Spread in ticks:** AAPL normal-session spread is typically 1–5 ticks ($0.0001–$0.0005). Pre-market can be hundreds of ticks wide. If you see spread > 10000 (=$1.00) something is off.
- **Microprice vs mid:** When bid volume >> ask volume, microprice > mid (more buying pressure → price closer to ask). When ask volume >> bid volume, microprice < mid. It should NEVER be outside [best_bid, best_ask].
- **OBI near ±1:** Unusual. Means one side of the book has almost all the volume. Common briefly after large executions.
