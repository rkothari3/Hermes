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

// Add shares to one side of the book.
// Bids and asks maintain independent base prices so wide pre-market spreads
// do not cause one side to fall outside the other side's window.
// Returns the index used, or MAX_LEVELS if the price is out of the array range.
static uint32_t book_add(OrderBook* book, uint32_t price, uint32_t shares, bool is_bid) {
    book->initialized = true;

    if (is_bid) {
        // Initialize bid base on first bid, centered so the first bid lands at
        // index MAX_LEVELS/2, giving equal headroom above and below.
        if (!book->bid_initialized) {
            book->bid_base = (price >= MAX_LEVELS / 2) ? price - MAX_LEVELS / 2 : 0;
            book->bid_initialized = true;
            // base_price tracks whichever side initialized first (back-compat for tests)
            if (!book->ask_initialized) book->base_price = book->bid_base;
        }
        if (price < book->bid_base) return MAX_LEVELS;
        uint32_t idx = price - book->bid_base;
        if (idx >= MAX_LEVELS) return MAX_LEVELS;

        book->bids[idx].quantity    += shares;
        book->bids[idx].order_count++;
        if (idx > book->best_bid_idx) book->best_bid_idx = idx;
        return idx;
    } else {
        // Initialize ask base on first ask.
        if (!book->ask_initialized) {
            book->ask_base = (price >= MAX_LEVELS / 2) ? price - MAX_LEVELS / 2 : 0;
            book->ask_initialized = true;
            // base_price tracks whichever side initialized first (back-compat for tests)
            if (!book->bid_initialized) book->base_price = book->ask_base;
        }
        if (price < book->ask_base) return MAX_LEVELS;
        uint32_t idx = price - book->ask_base;
        if (idx >= MAX_LEVELS) return MAX_LEVELS;

        book->asks[idx].quantity    += shares;
        book->asks[idx].order_count++;
        if (book->asks[book->best_ask_idx].quantity == 0 || idx < book->best_ask_idx)
            book->best_ask_idx = idx;
        return idx;
    }
}

// Remove shares from one side.
// Each side uses its own base price for index computation.
static void book_remove(OrderBook* book, uint32_t price, uint32_t shares, bool is_bid) {
    if (is_bid) {
        if (!book->bid_initialized || price < book->bid_base) return;
        uint32_t idx = price - book->bid_base;
        if (idx >= MAX_LEVELS) return;

        book->bids[idx].quantity -= shares;
        book->bids[idx].order_count--;
        if (idx == book->best_bid_idx && book->bids[idx].quantity == 0) {
            while (book->best_bid_idx > 0 &&
                   book->bids[book->best_bid_idx].quantity == 0)
                book->best_bid_idx--;
        }
    } else {
        if (!book->ask_initialized || price < book->ask_base) return;
        uint32_t idx = price - book->ask_base;
        if (idx >= MAX_LEVELS) return;

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
// when both sides have live orders.
// NOTE: real NASDAQ data has locked/crossed books in pre-open (e.g. bids
// entered before asks, or aggressive orders before the open auction).
// The assertion is therefore only enabled in unit-test builds where all
// input is well-formed continuous-session data (define CHECK_BOOK_INVARIANT
// at compile time to enable it).
static void check_invariant(const OrderBook* book) {
#ifdef CHECK_BOOK_INVARIANT
    if (!book->bid_initialized || !book->ask_initialized) return;
    bool has_bid = book->bids[book->best_bid_idx].quantity > 0;
    bool has_ask = book->asks[book->best_ask_idx].quantity > 0;
    if (has_bid && has_ask) {
        uint32_t best_bid_price = book->bid_base + book->best_bid_idx;
        uint32_t best_ask_price = book->ask_base + book->best_ask_idx;
        assert(best_bid_price < best_ask_price &&
               "INVARIANT VIOLATED: best_bid >= best_ask");
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

    if (book) {
        bool is_bid = o.side == 'B';
        uint32_t base = is_bid ? book->bid_base : book->ask_base;
        bool init    = is_bid ? book->bid_initialized : book->ask_initialized;
        if (init && o.price >= base) {
            uint32_t idx = o.price - base;
            if (idx < MAX_LEVELS) {
                if (is_bid) {
                    book->bids[idx].quantity -= remove;
                } else {
                    book->asks[idx].quantity -= remove;
                }
            }
        }
    }

    if (o.quantity == 0) {
        if (book) {
            bool is_bid = o.side == 'B';
            uint32_t base = is_bid ? book->bid_base : book->ask_base;
            bool init    = is_bid ? book->bid_initialized : book->ask_initialized;
            if (init && o.price >= base) {
                uint32_t idx = o.price - base;
                if (idx < MAX_LEVELS) {
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

    if (book) {
        bool is_bid = o.side == 'B';
        uint32_t base = is_bid ? book->bid_base : book->ask_base;
        bool init    = is_bid ? book->bid_initialized : book->ask_initialized;
        if (init && o.price >= base) {
            uint32_t idx = o.price - base;
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
