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

// Rebase one side of the book so that 'price' fits within the array.
// For asks: slide window DOWN when price < ask_base (pre-market → regular session).
// For bids: slide window UP when price > bid_base + MAX_LEVELS - 1 (price drifted up).
// Entries that shift off the far end are cleared; future removes for those
// stale orders will be silently ignored by the out-of-range guard in book_remove.
// Returns true if the entire array was cleared (all prior entries are stale).
static bool rebase_side(Level* levels, uint32_t& base, uint32_t& best_idx,
                         uint32_t price, bool slide_down) {
    uint32_t new_base;
    if (slide_down) {
        // New base: put price at index MAX_LEVELS/4 (room below for further drops)
        new_base = (price >= MAX_LEVELS / 4) ? price - MAX_LEVELS / 4 : 0;
        uint32_t shift = base - new_base;   // positive: entries shift UP in array
        if (shift >= MAX_LEVELS) {
            std::fill(levels, levels + MAX_LEVELS, Level{});
            best_idx = MAX_LEVELS - 1;      // ask default (empty book)
            base = new_base;
            return true;                    // was fully cleared
        } else {
            memmove(levels + shift, levels, (MAX_LEVELS - shift) * sizeof(Level));
            std::fill(levels, levels + shift, Level{});
            best_idx = std::min(best_idx + shift, MAX_LEVELS - 1u);
        }
    } else {
        // Slide UP: put price at index MAX_LEVELS*3/4 (room above for further rises)
        new_base = (price >= MAX_LEVELS * 3 / 4) ? price - MAX_LEVELS * 3 / 4 : 0;
        uint32_t shift = new_base - base;   // positive: entries shift DOWN in array
        if (shift >= MAX_LEVELS) {
            std::fill(levels, levels + MAX_LEVELS, Level{});
            best_idx = 0;                   // bid default (empty book)
            base = new_base;
            return true;                    // was fully cleared
        } else {
            memmove(levels, levels + shift, (MAX_LEVELS - shift) * sizeof(Level));
            std::fill(levels + (MAX_LEVELS - shift), levels + MAX_LEVELS, Level{});
            best_idx = (best_idx >= shift) ? best_idx - shift : 0u;
        }
    }
    base = new_base;
    return false;   // partial shift, entries preserved
}

// Add shares to one side of the book.
// Each side has its own base so pre-market wide spreads don't corrupt the other.
// Rebases on-the-fly when a price falls outside the current window (typically
// once per symbol when transitioning from pre-market to regular session).
static uint32_t book_add(OrderBook* book, uint32_t price, uint32_t shares, bool is_bid) {
    book->initialized = true;

    if (is_bid) {
        if (!book->bid_initialized) {
            book->bid_base = (price >= MAX_LEVELS / 2) ? price - MAX_LEVELS / 2 : 0;
            book->bid_initialized = true;
            if (!book->ask_initialized) book->base_price = book->bid_base;
        }
        if (price < book->bid_base) {
            if (rebase_side(book->bids, book->bid_base, book->best_bid_idx, price, true))
                book->bid_gen++;
        } else if (price - book->bid_base >= MAX_LEVELS) {
            if (rebase_side(book->bids, book->bid_base, book->best_bid_idx, price, false))
                book->bid_gen++;
        }

        uint32_t idx = price - book->bid_base;
        if (idx >= MAX_LEVELS) return MAX_LEVELS;
        book->bids[idx].quantity    += shares;
        book->bids[idx].order_count++;
        if (idx > book->best_bid_idx) book->best_bid_idx = idx;
        return idx;
    } else {
        if (!book->ask_initialized) {
            book->ask_base = (price >= MAX_LEVELS / 2) ? price - MAX_LEVELS / 2 : 0;
            book->ask_initialized = true;
            if (!book->bid_initialized) book->base_price = book->ask_base;
        }
        if (price < book->ask_base) {
            if (rebase_side(book->asks, book->ask_base, book->best_ask_idx, price, true))
                book->ask_gen++;
        } else if (price - book->ask_base >= MAX_LEVELS) {
            if (rebase_side(book->asks, book->ask_base, book->best_ask_idx, price, false))
                book->ask_gen++;
        }

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

        book->bids[idx].quantity    = (book->bids[idx].quantity    >= shares) ? book->bids[idx].quantity    - shares : 0u;
        book->bids[idx].order_count = (book->bids[idx].order_count >= 1)      ? book->bids[idx].order_count - 1      : 0u;
        if (idx == book->best_bid_idx && book->bids[idx].quantity == 0) {
            while (book->best_bid_idx > 0 &&
                   book->bids[book->best_bid_idx].quantity == 0)
                book->best_bid_idx--;
        }
    } else {
        if (!book->ask_initialized || price < book->ask_base) return;
        uint32_t idx = price - book->ask_base;
        if (idx >= MAX_LEVELS) return;

        book->asks[idx].quantity    = (book->asks[idx].quantity    >= shares) ? book->asks[idx].quantity    - shares : 0u;
        book->asks[idx].order_count = (book->asks[idx].order_count >= 1)      ? book->asks[idx].order_count - 1      : 0u;
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
    uint32_t idx = book_add(book, msg.price, msg.shares, is_bid);
    // Only track in order_map if placed in the book array. Store the current
    // generation so stale removes after a clear-rebase are detected and skipped.
    if (idx < MAX_LEVELS) {
        uint8_t gen = is_bid ? book->bid_gen : book->ask_gen;
        ms->order_map[msg.order_ref] = {msg.order_ref, msg.price, msg.shares,
                                         msg.locate, msg.side, gen};
    }
    check_invariant(book);
}

void handle_add_order_mpid(MarketState* ms, const AddOrderMPID& msg) {
    // MPID field is attribution only — book logic identical to AddOrder
    OrderBook* book = ms->books[msg.locate];
    if (!book) return;
    bool is_bid = (msg.side == 'B');
    uint32_t idx = book_add(book, msg.price, msg.shares, is_bid);
    if (idx < MAX_LEVELS) {
        uint8_t gen = is_bid ? book->bid_gen : book->ask_gen;
        ms->order_map[msg.order_ref] = {msg.order_ref, msg.price, msg.shares,
                                         msg.locate, msg.side, gen};
    }
    check_invariant(book);
}

void handle_order_delete(MarketState* ms, const OrderDelete& msg) {
    auto it = ms->order_map.find(msg.order_ref);
    if (it == ms->order_map.end()) return;
    const Order& o = it->second;
    OrderBook* book = ms->books[o.locate];
    if (book) {
        bool is_bid = (o.side == 'B');
        uint8_t cur_gen = is_bid ? book->bid_gen : book->ask_gen;
        if (o.gen == cur_gen) {   // skip if the book side was cleared after this order was added
            book_remove(book, o.price, o.quantity, is_bid);
            check_invariant(book);
        }
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
        uint8_t cur_gen = is_bid ? book->bid_gen : book->ask_gen;
        if (o.gen == cur_gen) {   // only touch book if order is from the current generation
            uint32_t base = is_bid ? book->bid_base : book->ask_base;
            bool init    = is_bid ? book->bid_initialized : book->ask_initialized;
            if (init && o.price >= base) {
                uint32_t idx = o.price - base;
                if (idx < MAX_LEVELS) {
                    if (is_bid) book->bids[idx].quantity = (book->bids[idx].quantity >= remove) ? book->bids[idx].quantity - remove : 0u;
                    else        book->asks[idx].quantity = (book->asks[idx].quantity >= remove) ? book->asks[idx].quantity - remove : 0u;
                }
            }
        }
    }

    if (o.quantity == 0) {
        if (book) {
            bool is_bid = o.side == 'B';
            uint8_t cur_gen = is_bid ? book->bid_gen : book->ask_gen;
            if (o.gen == cur_gen) {
                uint32_t base = is_bid ? book->bid_base : book->ask_base;
                bool init    = is_bid ? book->bid_initialized : book->ask_initialized;
                if (init && o.price >= base) {
                    uint32_t idx = o.price - base;
                    if (idx < MAX_LEVELS) {
                        if (is_bid) {
                            book->bids[idx].order_count = (book->bids[idx].order_count >= 1) ? book->bids[idx].order_count - 1 : 0u;
                            if (idx == book->best_bid_idx && book->bids[idx].quantity == 0)
                                while (book->best_bid_idx > 0 &&
                                       book->bids[book->best_bid_idx].quantity == 0)
                                    book->best_bid_idx--;
                        } else {
                            book->asks[idx].order_count = (book->asks[idx].order_count >= 1) ? book->asks[idx].order_count - 1 : 0u;
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

    // Remove old (gen check: skip book update if order is from a stale generation)
    bool is_bid = (old_o.side == 'B');
    uint8_t cur_gen = is_bid ? book->bid_gen : book->ask_gen;
    if (old_o.gen == cur_gen)
        book_remove(book, old_o.price, old_o.quantity, is_bid);

    // Add new (only track if placed in book)
    uint32_t idx = book_add(book, msg.new_price, msg.new_shares, is_bid);
    if (idx < MAX_LEVELS) {
        uint8_t new_gen = is_bid ? book->bid_gen : book->ask_gen;
        ms->order_map[msg.new_order_ref] = {msg.new_order_ref, msg.new_price,
                                             msg.new_shares, old_o.locate, old_o.side, new_gen};
    }
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
        uint8_t cur_gen = is_bid ? book->bid_gen : book->ask_gen;
        if (o.gen == cur_gen) {   // skip if book side was cleared after this order was added
            uint32_t base = is_bid ? book->bid_base : book->ask_base;
            bool init    = is_bid ? book->bid_initialized : book->ask_initialized;
            if (init && o.price >= base) {
                uint32_t idx = o.price - base;
                if (idx < MAX_LEVELS) {
                    if (is_bid) book->bids[idx].quantity = (book->bids[idx].quantity >= remove) ? book->bids[idx].quantity - remove : 0u;
                    else        book->asks[idx].quantity = (book->asks[idx].quantity >= remove) ? book->asks[idx].quantity - remove : 0u;

                    if (o.quantity == 0) {
                        if (is_bid) {
                            book->bids[idx].order_count = (book->bids[idx].order_count >= 1) ? book->bids[idx].order_count - 1 : 0u;
                            if (idx == book->best_bid_idx && book->bids[idx].quantity == 0)
                                while (book->best_bid_idx > 0 &&
                                       book->bids[book->best_bid_idx].quantity == 0)
                                    book->best_bid_idx--;
                        } else {
                            book->asks[idx].order_count = (book->asks[idx].order_count >= 1) ? book->asks[idx].order_count - 1 : 0u;
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
