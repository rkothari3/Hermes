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
