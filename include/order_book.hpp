// include/order_book.hpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include "itch_parser.hpp"

// Number of price levels per side per symbol.
// 8192 ticks × $0.0001/tick = $0.82 price range.
// base_price is set so the first order lands at index MAX_LEVELS/2,
// giving equal headroom above and below.
static constexpr uint32_t MAX_LEVELS = 8192;
static constexpr uint32_t MAX_LOCATE = 65536;

// One price level — 8 bytes, two fit in a cache line.
struct Level {
    uint32_t quantity    = 0;
    uint32_t order_count = 0;
};

// One live order tracked in the order map — 20 bytes.
struct Order {
    uint64_t order_ref;
    uint32_t price;
    uint32_t quantity;
    uint16_t locate;
    uint8_t  side;   // 'B' = bid, 'S' = ask
    uint8_t  pad;
};

// One symbol's full order book — ~131KB per symbol.
struct OrderBook {
    char     symbol[9]    = {};
    uint16_t locate       = 0;
    uint32_t base_price   = 0;    // array[idx] covers price = base_price + idx
    uint32_t best_bid_idx = 0;            // index of highest bid with quantity > 0
    uint32_t best_ask_idx = MAX_LEVELS - 1; // index of lowest ask with quantity > 0
    bool     initialized  = false;
    Level    bids[MAX_LEVELS] = {};
    Level    asks[MAX_LEVELS] = {};
};

// Top-level market state — one per process.
struct MarketState {
    OrderBook* books[MAX_LOCATE] = {};  // books[locate] = nullptr until Stock Directory
    std::unordered_map<uint64_t, Order> order_map;  // order_ref -> Order
};

// ── Public API ────────────────────────────────────────────────────────────────
MarketState* create_market_state();
void         destroy_market_state(MarketState* ms);

void handle_stock_directory      (MarketState* ms, const StockDirectory& msg);
void handle_add_order            (MarketState* ms, const AddOrder& msg);
void handle_add_order_mpid       (MarketState* ms, const AddOrderMPID& msg);
void handle_order_delete         (MarketState* ms, const OrderDelete& msg);
void handle_order_cancel         (MarketState* ms, const OrderCancel& msg);
void handle_order_replace        (MarketState* ms, const OrderReplace& msg);
void handle_order_executed       (MarketState* ms, const OrderExecuted& msg);
void handle_order_executed_price (MarketState* ms, const OrderExecutedPrice& msg);
