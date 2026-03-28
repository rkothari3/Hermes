// tests/test_order_book.cpp
#include <gtest/gtest.h>
#include <cstring>
#include "order_book.hpp"

// Helper: build a minimal AddOrder struct
static AddOrder make_add(uint16_t locate, uint64_t ref, uint32_t price,
                          uint32_t shares, uint8_t side) {
    AddOrder msg{};
    msg.locate    = locate;
    msg.order_ref = ref;
    msg.price     = price;
    msg.shares    = shares;
    msg.side      = side;
    memcpy(msg.stock, "TEST    ", 8);
    msg.stock[8] = '\0';
    return msg;
}

// Helper: build a minimal StockDirectory struct
static StockDirectory make_dir(uint16_t locate, const char* symbol) {
    StockDirectory msg{};
    msg.locate = locate;
    strncpy(msg.stock, symbol, 8);
    msg.stock[8] = '\0';
    return msg;
}

class OrderBookTest : public ::testing::Test {
protected:
    MarketState* ms;
    void SetUp()    override { ms = create_market_state(); }
    void TearDown() override { destroy_market_state(ms); }
};

// ── Task 4: AddOrder ──────────────────────────────────────────────────────────

TEST_F(OrderBookTest, AddOrder_AllocatesBookOnStockDirectory) {
    handle_stock_directory(ms, make_dir(1, "AAPL"));
    ASSERT_NE(ms->books[1], nullptr);
    EXPECT_STREQ(ms->books[1]->symbol, "AAPL");
}

TEST_F(OrderBookTest, AddOrder_UpdatesBestBid) {
    handle_stock_directory(ms, make_dir(1, "AAPL"));
    handle_add_order(ms, make_add(1, 42, 1000000, 500, 'B'));

    const OrderBook* book = ms->books[1];
    ASSERT_NE(book, nullptr);
    EXPECT_TRUE(book->initialized);

    uint32_t idx = 1000000 - book->base_price;
    EXPECT_EQ(book->bids[idx].quantity,    500u);
    EXPECT_EQ(book->bids[idx].order_count, 1u);
    EXPECT_EQ(book->best_bid_idx,          idx);
}

TEST_F(OrderBookTest, AddOrder_UpdatesBestAsk) {
    handle_stock_directory(ms, make_dir(1, "AAPL"));
    handle_add_order(ms, make_add(1, 43, 1000100, 300, 'S'));

    const OrderBook* book = ms->books[1];
    uint32_t idx = 1000100 - book->base_price;
    EXPECT_EQ(book->asks[idx].quantity,    300u);
    EXPECT_EQ(book->asks[idx].order_count, 1u);
    EXPECT_EQ(book->best_ask_idx,          idx);
}

TEST_F(OrderBookTest, AddOrder_MultipleOrders_BestBidIsHighest) {
    handle_stock_directory(ms, make_dir(2, "MSFT"));
    handle_add_order(ms, make_add(2, 1, 2000000, 100, 'B'));  // lower bid
    handle_add_order(ms, make_add(2, 2, 2000200, 200, 'B'));  // higher bid — better

    const OrderBook* book = ms->books[2];
    uint32_t idx_high = 2000200 - book->base_price;
    EXPECT_EQ(book->best_bid_idx, idx_high);
}

TEST_F(OrderBookTest, AddOrder_MultipleOrders_BestAskIsLowest) {
    handle_stock_directory(ms, make_dir(2, "MSFT"));
    handle_add_order(ms, make_add(2, 3, 2000500, 100, 'S'));  // higher ask
    handle_add_order(ms, make_add(2, 4, 2000300, 200, 'S'));  // lower ask — better

    const OrderBook* book = ms->books[2];
    uint32_t idx_low = 2000300 - book->base_price;
    EXPECT_EQ(book->best_ask_idx, idx_low);
}

// ── Task 5: OrderDelete ───────────────────────────────────────────────────────

TEST_F(OrderBookTest, OrderDelete_RemovesQuantityAndOrder) {
    handle_stock_directory(ms, make_dir(3, "TSLA"));
    handle_add_order(ms, make_add(3, 10, 3000000, 100, 'B'));
    const OrderBook* book = ms->books[3];
    uint32_t idx = 3000000 - book->base_price;

    OrderDelete del{};
    del.order_ref = 10;
    del.locate    = 3;
    handle_order_delete(ms, del);

    EXPECT_EQ(book->bids[idx].quantity,    0u);
    EXPECT_EQ(book->bids[idx].order_count, 0u);
    EXPECT_EQ(ms->order_map.count(10),     0u);
}

TEST_F(OrderBookTest, OrderDelete_BestBidRescansAfterDeletion) {
    handle_stock_directory(ms, make_dir(3, "TSLA"));
    handle_add_order(ms, make_add(3, 20, 3000000, 100, 'B'));   // lower bid
    handle_add_order(ms, make_add(3, 21, 3000200, 200, 'B'));   // higher bid — best
    const OrderBook* book = ms->books[3];
    uint32_t idx_low  = 3000000 - book->base_price;
    uint32_t idx_high = 3000200 - book->base_price;
    EXPECT_EQ(book->best_bid_idx, idx_high);

    OrderDelete del{};
    del.order_ref = 21;
    del.locate    = 3;
    handle_order_delete(ms, del);

    EXPECT_EQ(book->best_bid_idx, idx_low);
    EXPECT_EQ(book->bids[idx_low].quantity, 100u);
}

TEST_F(OrderBookTest, OrderDelete_UnknownRefIsNoOp) {
    handle_stock_directory(ms, make_dir(3, "TSLA"));
    OrderDelete del{};
    del.order_ref = 9999;
    EXPECT_NO_THROW(handle_order_delete(ms, del));
}

// ── Task 6: OrderCancel ───────────────────────────────────────────────────────

TEST_F(OrderBookTest, OrderCancel_ReducesQuantityPartially) {
    handle_stock_directory(ms, make_dir(4, "AMZN"));
    handle_add_order(ms, make_add(4, 30, 4000000, 100, 'B'));
    const OrderBook* book = ms->books[4];
    uint32_t idx = 4000000 - book->base_price;

    OrderCancel cancel{};
    cancel.order_ref        = 30;
    cancel.cancelled_shares = 40;
    handle_order_cancel(ms, cancel);

    EXPECT_EQ(book->bids[idx].quantity,      60u);  // 100 - 40
    EXPECT_EQ(ms->order_map.at(30).quantity, 60u);
    EXPECT_EQ(ms->order_map.count(30),        1u);  // still tracked
}

TEST_F(OrderBookTest, OrderCancel_FullCancelRemovesOrder) {
    handle_stock_directory(ms, make_dir(4, "AMZN"));
    handle_add_order(ms, make_add(4, 31, 4000000, 50, 'B'));
    const OrderBook* book = ms->books[4];
    uint32_t idx = 4000000 - book->base_price;

    OrderCancel cancel{};
    cancel.order_ref        = 31;
    cancel.cancelled_shares = 50;
    handle_order_cancel(ms, cancel);

    EXPECT_EQ(book->bids[idx].quantity, 0u);
    EXPECT_EQ(ms->order_map.count(31),  0u);
}

// ── Task 7: OrderReplace ─────────────────────────────────────────────────────

TEST_F(OrderBookTest, OrderReplace_OldRefGoneNewRefPresent) {
    handle_stock_directory(ms, make_dir(5, "NVDA"));
    handle_add_order(ms, make_add(5, 40, 5000000, 100, 'B'));

    OrderReplace rep{};
    rep.orig_order_ref = 40;
    rep.new_order_ref  = 41;
    rep.new_price      = 5000200;
    rep.new_shares     = 75;
    handle_order_replace(ms, rep);

    EXPECT_EQ(ms->order_map.count(40), 0u);
    EXPECT_EQ(ms->order_map.count(41), 1u);
    EXPECT_EQ(ms->order_map.at(41).price,    5000200u);
    EXPECT_EQ(ms->order_map.at(41).quantity, 75u);
}

TEST_F(OrderBookTest, OrderReplace_BookUpdatesOldAndNewLevel) {
    handle_stock_directory(ms, make_dir(5, "NVDA"));
    handle_add_order(ms, make_add(5, 50, 5000000, 100, 'B'));
    const OrderBook* book = ms->books[5];
    uint32_t old_idx = 5000000 - book->base_price;

    OrderReplace rep{};
    rep.orig_order_ref = 50;
    rep.new_order_ref  = 51;
    rep.new_price      = 5000400;
    rep.new_shares     = 60;
    handle_order_replace(ms, rep);

    uint32_t new_idx = 5000400 - book->base_price;
    EXPECT_EQ(book->bids[old_idx].quantity, 0u);
    EXPECT_EQ(book->bids[new_idx].quantity, 60u);
    EXPECT_EQ(book->best_bid_idx,           new_idx);
}

// ── Task 8: OrderExecuted ────────────────────────────────────────────────────

TEST_F(OrderBookTest, OrderExecuted_PartialReducesQuantity) {
    handle_stock_directory(ms, make_dir(1, "AAPL"));
    handle_add_order(ms, make_add(1, 60, 1000000, 100, 'S'));
    const OrderBook* book = ms->books[1];
    uint32_t idx = 1000000 - book->base_price;

    OrderExecuted exec{};
    exec.order_ref       = 60;
    exec.executed_shares = 30;
    handle_order_executed(ms, exec);

    EXPECT_EQ(book->asks[idx].quantity,      70u);
    EXPECT_EQ(ms->order_map.count(60),        1u);
    EXPECT_EQ(ms->order_map.at(60).quantity, 70u);
}

TEST_F(OrderBookTest, OrderExecuted_FullRemovesOrder) {
    handle_stock_directory(ms, make_dir(1, "AAPL"));
    handle_add_order(ms, make_add(1, 61, 1000000, 50, 'S'));
    const OrderBook* book = ms->books[1];
    uint32_t idx = 1000000 - book->base_price;

    OrderExecuted exec{};
    exec.order_ref       = 61;
    exec.executed_shares = 50;
    handle_order_executed(ms, exec);

    EXPECT_EQ(book->asks[idx].quantity, 0u);
    EXPECT_EQ(ms->order_map.count(61),  0u);
}

TEST_F(OrderBookTest, OrderExecuted_BestAskRescansAfterFull) {
    handle_stock_directory(ms, make_dir(1, "AAPL"));
    handle_add_order(ms, make_add(1, 70, 1000000, 100, 'S'));  // best ask (lower)
    handle_add_order(ms, make_add(1, 71, 1000100, 200, 'S'));  // worse ask (higher)
    const OrderBook* book = ms->books[1];
    uint32_t idx_low  = 1000000 - book->base_price;
    uint32_t idx_high = 1000100 - book->base_price;
    EXPECT_EQ(book->best_ask_idx, idx_low);

    OrderExecuted exec{};
    exec.order_ref       = 70;
    exec.executed_shares = 100;
    handle_order_executed(ms, exec);

    EXPECT_EQ(book->best_ask_idx, idx_high);
}
