// src/order_book.cpp
#include "order_book.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>

MarketState* create_market_state() {
    auto* ms = new MarketState{};
    return ms;
}

void destroy_market_state(MarketState* ms) {
    for (uint32_t i = 0; i < MAX_LOCATE; ++i) {
        delete ms->books[i];
        ms->books[i] = nullptr;
    }
    delete ms;
}

void handle_stock_directory(MarketState* ms, const StockDirectory& msg) {
    if (ms->books[msg.locate]) return;  // already registered
    auto* book = new OrderBook{};
    book->locate = msg.locate;
    memcpy(book->symbol, msg.stock, sizeof(book->symbol));
    ms->books[msg.locate] = book;
}

// ── Internal helpers ──────────────────────────────────────────────────────────

// Add shares to one side of the book. Returns the index used (or MAX_LEVELS on bounds failure).
static uint32_t book_add(OrderBook* book, uint32_t price, uint32_t shares, bool is_bid) {
    if (!book->initialized) {
        book->base_price  = (price >= MAX_LEVELS / 2) ? price - MAX_LEVELS / 2 : 0;
        book->initialized = true;
    }
    if (price < book->base_price) return MAX_LEVELS;
    uint32_t idx = price - book->base_price;
    if (idx >= MAX_LEVELS) return MAX_LEVELS;

    if (is_bid) {
        book->bids[idx].quantity    += shares;
        book->bids[idx].order_count++;
        if (idx > book->best_bid_idx) book->best_bid_idx = idx;
    } else {
        book->asks[idx].quantity    += shares;
        book->asks[idx].order_count++;
        if (book->asks[book->best_ask_idx].quantity == 0 || idx < book->best_ask_idx)
            book->best_ask_idx = idx;
    }
    return idx;
}

// Remove shares from one side. Re-scans best if the removed level empties.
static void book_remove(OrderBook* book, uint32_t price, uint32_t shares, bool is_bid) {
    if (price < book->base_price) return;
    uint32_t idx = price - book->base_price;
    if (idx >= MAX_LEVELS) return;

    if (is_bid) {
        book->bids[idx].quantity -= shares;
        book->bids[idx].order_count--;
        if (idx == book->best_bid_idx && book->bids[idx].quantity == 0) {
            while (book->best_bid_idx > 0 &&
                   book->bids[book->best_bid_idx].quantity == 0)
                book->best_bid_idx--;
        }
    } else {
        book->asks[idx].quantity -= shares;
        book->asks[idx].order_count--;
        if (idx == book->best_ask_idx && book->asks[idx].quantity == 0) {
            while (book->best_ask_idx < MAX_LEVELS - 1 &&
                   book->asks[book->best_ask_idx].quantity == 0)
                book->best_ask_idx++;
        }
    }
}

// Sanity check: best bid price must be strictly less than best ask price
// when both sides have live orders. Fires in debug builds only.
static void check_invariant(const OrderBook* book) {
#ifndef NDEBUG
    if (!book->initialized) return;
    bool has_bid = book->bids[book->best_bid_idx].quantity > 0;
    bool has_ask = book->asks[book->best_ask_idx].quantity > 0;
    if (has_bid && has_ask) {
        assert(book->best_bid_idx < book->best_ask_idx &&
               "INVARIANT VIOLATED: best_bid_idx >= best_ask_idx");
    }
#else
    (void)book;
#endif
}

// ── Public handlers ───────────────────────────────────────────────────────────

void handle_add_order(MarketState* ms, const AddOrder& msg) {
    OrderBook* book = ms->books[msg.locate];
    if (!book) return;
    bool is_bid = (msg.side == 'B');
    book_add(book, msg.price, msg.shares, is_bid);
    ms->order_map[msg.order_ref] = {msg.order_ref, msg.price, msg.shares,
                                     msg.locate, msg.side, 0};
    check_invariant(book);
}

void handle_add_order_mpid(MarketState* ms, const AddOrderMPID& msg) {
    // MPID field is attribution only — book logic identical to AddOrder
    OrderBook* book = ms->books[msg.locate];
    if (!book) return;
    bool is_bid = (msg.side == 'B');
    book_add(book, msg.price, msg.shares, is_bid);
    ms->order_map[msg.order_ref] = {msg.order_ref, msg.price, msg.shares,
                                     msg.locate, msg.side, 0};
    check_invariant(book);
}

void handle_order_delete(MarketState* ms, const OrderDelete& msg) {
    auto it = ms->order_map.find(msg.order_ref);
    if (it == ms->order_map.end()) return;
    const Order& o = it->second;
    OrderBook* book = ms->books[o.locate];
    if (book) {
        book_remove(book, o.price, o.quantity, o.side == 'B');
        check_invariant(book);
    }
    ms->order_map.erase(it);
}

void handle_order_cancel(MarketState* ms, const OrderCancel& msg) {
    auto it = ms->order_map.find(msg.order_ref);
    if (it == ms->order_map.end()) return;
    Order& o = it->second;
    OrderBook* book = ms->books[o.locate];
    uint32_t remove = std::min(msg.cancelled_shares, o.quantity);
    o.quantity -= remove;
    if (book && o.price >= book->base_price) {
        uint32_t idx = o.price - book->base_price;
        if (idx < MAX_LEVELS) {
            if (o.side == 'B') book->bids[idx].quantity -= remove;
            else                book->asks[idx].quantity -= remove;
        }
    }
    if (o.quantity == 0) {
        if (book && o.price >= book->base_price) {
            uint32_t idx = o.price - book->base_price;
            if (idx < MAX_LEVELS) {
                bool is_bid = o.side == 'B';
                if (is_bid) {
                    book->bids[idx].order_count--;
                    if (idx == book->best_bid_idx && book->bids[idx].quantity == 0)
                        while (book->best_bid_idx > 0 &&
                               book->bids[book->best_bid_idx].quantity == 0)
                            book->best_bid_idx--;
                } else {
                    book->asks[idx].order_count--;
                    if (idx == book->best_ask_idx && book->asks[idx].quantity == 0)
                        while (book->best_ask_idx < MAX_LEVELS - 1 &&
                               book->asks[book->best_ask_idx].quantity == 0)
                            book->best_ask_idx++;
                }
                check_invariant(book);
            }
        }
        ms->order_map.erase(it);
    }
}

void handle_order_replace(MarketState* ms, const OrderReplace& msg) {
    auto it = ms->order_map.find(msg.orig_order_ref);
    if (it == ms->order_map.end()) return;
    Order old_o = it->second;
    ms->order_map.erase(it);

    OrderBook* book = ms->books[old_o.locate];
    if (!book) return;

    // Remove old
    book_remove(book, old_o.price, old_o.quantity, old_o.side == 'B');

    // Add new
    book_add(book, msg.new_price, msg.new_shares, old_o.side == 'B');
    ms->order_map[msg.new_order_ref] = {msg.new_order_ref, msg.new_price,
                                         msg.new_shares, old_o.locate, old_o.side, 0};
    check_invariant(book);
}

void handle_order_executed(MarketState* ms, const OrderExecuted& msg) {
    auto it = ms->order_map.find(msg.order_ref);
    if (it == ms->order_map.end()) return;
    Order& o = it->second;
    OrderBook* book = ms->books[o.locate];
    uint32_t remove = std::min(msg.executed_shares, o.quantity);
    o.quantity -= remove;
    if (book && o.price >= book->base_price) {
        bool is_bid = o.side == 'B';
        uint32_t idx = o.price - book->base_price;
        if (idx < MAX_LEVELS) {
            if (is_bid) book->bids[idx].quantity -= remove;
            else        book->asks[idx].quantity -= remove;

            if (o.quantity == 0) {
                if (is_bid) {
                    book->bids[idx].order_count--;
                    if (idx == book->best_bid_idx && book->bids[idx].quantity == 0)
                        while (book->best_bid_idx > 0 &&
                               book->bids[book->best_bid_idx].quantity == 0)
                            book->best_bid_idx--;
                } else {
                    book->asks[idx].order_count--;
                    if (idx == book->best_ask_idx && book->asks[idx].quantity == 0)
                        while (book->best_ask_idx < MAX_LEVELS - 1 &&
                               book->asks[book->best_ask_idx].quantity == 0)
                            book->best_ask_idx++;
                }
                check_invariant(book);
            }
        }
    }
    if (o.quantity == 0) ms->order_map.erase(it);
}

void handle_order_executed_price(MarketState* ms, const OrderExecutedPrice& msg) {
    // Execution price field is for reporting only — book update identical to OrderExecuted
    OrderExecuted equiv{};
    equiv.order_ref       = msg.order_ref;
    equiv.executed_shares = msg.executed_shares;
    equiv.locate          = msg.locate;
    equiv.timestamp       = msg.timestamp;
    equiv.match_number    = msg.match_number;
    handle_order_executed(ms, equiv);
}
