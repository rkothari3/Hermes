// tests/test_integration.cpp
// Run separately — reads the full ITCH file (~8-12 GB, ~50s).
// Build with: cmake -DITCH_FILE=/path/to/file.NASDAQ_ITCH50 ...
// Run with:   ./build/hermes_integration_tests
#include <gtest/gtest.h>
#include "itch_parser.hpp"

#ifndef ITCH_FILE_PATH
#  error "Build with -DITCH_FILE=/path/to/file to enable integration tests"
#endif

TEST(IntegrationTest, FileCountsMatchPhase1) {
    const char* path = ITCH_FILE_PATH;

    struct Counts {
        uint64_t add_order=0, add_order_mpid=0, order_executed=0,
                 order_exec_price=0, order_cancel=0, order_delete=0,
                 order_replace=0, trade=0, stock_dir=0;
    } c{};

    MessageHandlers h{};
    h.ctx = &c;
    h.on_add_order            = [](const AddOrder&,            void* ctx){ ((Counts*)ctx)->add_order++; };
    h.on_add_order_mpid       = [](const AddOrderMPID&,        void* ctx){ ((Counts*)ctx)->add_order_mpid++; };
    h.on_order_executed       = [](const OrderExecuted&,       void* ctx){ ((Counts*)ctx)->order_executed++; };
    h.on_order_executed_price = [](const OrderExecutedPrice&,  void* ctx){ ((Counts*)ctx)->order_exec_price++; };
    h.on_order_cancel         = [](const OrderCancel&,         void* ctx){ ((Counts*)ctx)->order_cancel++; };
    h.on_order_delete         = [](const OrderDelete&,         void* ctx){ ((Counts*)ctx)->order_delete++; };
    h.on_order_replace        = [](const OrderReplace&,        void* ctx){ ((Counts*)ctx)->order_replace++; };
    h.on_trade                = [](const Trade&,               void* ctx){ ((Counts*)ctx)->trade++; };
    h.on_stock_directory      = [](const StockDirectory&,      void* ctx){ ((Counts*)ctx)->stock_dir++; };

    parse_file(path, h);

    EXPECT_EQ(c.add_order,        117145568ULL);
    EXPECT_EQ(c.add_order_mpid,     1485888ULL);
    EXPECT_EQ(c.order_executed,     5722824ULL);
    EXPECT_EQ(c.order_exec_price,     99917ULL);
    EXPECT_EQ(c.order_cancel,       2787676ULL);
    EXPECT_EQ(c.order_delete,     114360997ULL);
    EXPECT_EQ(c.order_replace,     21639067ULL);
    EXPECT_EQ(c.trade,              1218602ULL);
    EXPECT_EQ(c.stock_dir,             8906ULL);
}
