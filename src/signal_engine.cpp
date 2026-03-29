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

    // OBI over top-5 bid and ask levels (skip empty levels)
    uint64_t total_bid = 0, total_ask = 0;
    uint32_t count = 0;
    for (int32_t i = (int32_t)book->best_bid_idx; i >= 0 && count < 5; --i) {
        if (book->bids[i].quantity > 0) {
            total_bid += book->bids[i].quantity;
            ++count;
        }
    }
    count = 0;
    for (uint32_t i = book->best_ask_idx; i < MAX_LEVELS && count < 5; ++i) {
        if (book->asks[i].quantity > 0) {
            total_ask += book->asks[i].quantity;
            ++count;
        }
    }
    uint64_t total = total_bid + total_ask;
    s.obi   = (total > 0) ? (float)((int64_t)total_bid - (int64_t)total_ask) / (float)total : 0.0f;
    s.valid = true;
    return s;
}
