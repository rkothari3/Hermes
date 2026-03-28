# Phase 3 Design: Order Book Updater

## Goal
Maintain a live limit order book per symbol. Process Add/Delete/Cancel/Replace/Execute
messages to update bid/ask state. Verify best_bid < best_ask invariant after every update.
Dump top 5 levels for 5 symbols every 1M messages.

## New Files
- `include/order_book.hpp` — Level, Order, OrderBook, MarketState structs + update_book() declaration
- `src/order_book.cpp`    — update_book() implementation
- `tests/test_order_book.cpp` — unit tests
- `CMakeLists.txt`        — updated to include new source and test files

## Data Structures

```
Level      { quantity, order_count }          8 bytes
Order      { order_ref, price, quantity,      20 bytes (1 cache line)
             locate, side, pad }
OrderBook  { symbol[9], locate, base_price,   ~131KB per symbol
             best_bid_idx, best_ask_idx,
             bids[8192], asks[8192],
             initialized }
MarketState { books[65536],                   lazily heap-allocated books
              unordered_map<uint64_t,Order> } order_map (Phase 5: replace with pool)
```

MAX_LEVELS = 8192 (covers 8192 * $0.0001 = $0.82 price range per side).
Books allocated when Stock Directory message arrives (~8906 total, ~1.1GB peak).
base_price set on first Add Order = price - MAX_LEVELS/2.

## Message Handling

| Message | Action |
|---------|--------|
| Add (A/F) | allocate book if null, bounds-check idx, add to level + order_map |
| Delete (D) | remove full quantity from level, erase order_map, re-scan best |
| Cancel (X) | reduce quantity in level and order, erase if zero |
| Replace (U) | delete old ref, insert new ref at new price/qty |
| Executed (E/C) | reduce quantity, delete if zero |

Best bid/ask re-scan: incremental from current best, O(1) typical case.

## Sanity Check
After every update (debug builds only):
  assert(best_bid_idx < best_ask_idx)  // bid price always < ask price

## Output (main.cpp)
Every 1M messages: print top 5 bid/ask levels for AAPL, MSFT, TSLA, AMZN, NVDA.
Prices formatted as price/10000.0 (only float conversion in Phase 3).

## Order Tracking
std::unordered_map<uint64_t, Order> — correctness now, memory pool in Phase 5.

## Tests
- Add order → best bid/ask updates correctly
- Delete last order at level → best re-scans to next level
- Replace → old ref gone, new ref and price correct
- Cancel partial → quantity reduced, order still in map
- Executed full → order removed from map and book
