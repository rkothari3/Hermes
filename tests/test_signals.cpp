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
    // Heavy bid vs tiny ask
    handle_stock_directory(ms, make_dir(1, "TEST"));
    handle_add_order(ms, make_add(1, 1, 1000000, 10000, 'B'));
    handle_add_order(ms, make_add(1, 2, 1000100, 1,     'S'));
    Signals s = compute_signals(ms->books[1]);
    ASSERT_TRUE(s.valid);
    // OBI = (10000 - 1) / (10000 + 1) ≈ 0.9998
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
