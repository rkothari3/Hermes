#include <gtest/gtest.h>
#include <cstring>
#include "itch_parser.hpp"

// ── Task 4: AddOrder ('A', 36 bytes) ────────────────────────────────────────
// Layout: [0]=type [1-2]=locate [3-4]=tracking [5-10]=timestamp(48-bit)
//         [11-18]=order_ref(64-bit) [19]=side [20-23]=shares [24-31]=stock [32-35]=price
TEST(ParserTest, AddOrder_FieldsDecoded) {
    uint8_t body[36] = {};
    body[0]  = 'A';
    body[1] = 0x00; body[2] = 0x2A;  // locate = 42
    // tracking [3-4] = 0 (already zeroed)
    // timestamp = 1 ns
    body[5]=0x00; body[6]=0x00; body[7]=0x00; body[8]=0x00; body[9]=0x00; body[10]=0x01;
    // order_ref = 999 = 0x00000000000003E7
    body[11]=0x00; body[12]=0x00; body[13]=0x00; body[14]=0x00;
    body[15]=0x00; body[16]=0x00; body[17]=0x03; body[18]=0xE7;
    body[19] = 'B';  // side
    // shares = 500 = 0x000001F4
    body[20]=0x00; body[21]=0x00; body[22]=0x01; body[23]=0xF4;
    memcpy(&body[24], "AAPL    ", 8);  // stock (space-padded)
    // price = 1000000 = 0x000F4240
    body[32]=0x00; body[33]=0x0F; body[34]=0x42; body[35]=0x40;

    AddOrder result{};
    MessageHandlers h{};
    h.ctx = &result;
    h.on_add_order = [](const AddOrder& msg, void* ctx) {
        *static_cast<AddOrder*>(ctx) = msg;
    };
    parse_message(body, sizeof(body), h);

    EXPECT_EQ(result.locate,    42u);
    EXPECT_EQ(result.timestamp, 1u);
    EXPECT_EQ(result.order_ref, 999u);
    EXPECT_EQ(result.side,      (uint8_t)'B');
    EXPECT_EQ(result.shares,    500u);
    EXPECT_EQ(result.price,     1000000u);
    EXPECT_STREQ(result.stock,  "AAPL");
}

// ── Task 5: OrderDelete ('D', 19 bytes) ─────────────────────────────────────
// Layout: [0]=type [1-2]=locate [3-4]=tracking [5-10]=timestamp [11-18]=order_ref
TEST(ParserTest, OrderDelete_FieldsDecoded) {
    uint8_t body[19] = {};
    body[0] = 'D';
    body[1] = 0x00; body[2] = 0x07;  // locate = 7
    body[5]=0x00; body[6]=0x00; body[7]=0x00; body[8]=0x00; body[9]=0x00; body[10]=0x02;
    // order_ref = 12345 = 0x0000000000003039
    body[11]=0x00; body[12]=0x00; body[13]=0x00; body[14]=0x00;
    body[15]=0x00; body[16]=0x00; body[17]=0x30; body[18]=0x39;

    OrderDelete result{};
    MessageHandlers h{};
    h.ctx = &result;
    h.on_order_delete = [](const OrderDelete& msg, void* ctx) {
        *static_cast<OrderDelete*>(ctx) = msg;
    };
    parse_message(body, sizeof(body), h);

    EXPECT_EQ(result.locate,    7u);
    EXPECT_EQ(result.timestamp, 2u);
    EXPECT_EQ(result.order_ref, 12345u);
}

// ── Task 6: OrderReplace ('U', 35 bytes) ────────────────────────────────────
// Layout: [0]=type [1-2]=locate [3-4]=tracking [5-10]=timestamp
//         [11-18]=orig_order_ref [19-26]=new_order_ref [27-30]=shares [31-34]=price
TEST(ParserTest, OrderReplace_FieldsDecoded) {
    uint8_t body[35] = {};
    body[0] = 'U';
    body[1] = 0x00; body[2] = 0x05;  // locate = 5
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x03;  // ts=3
    // orig_order_ref = 100 (last byte only, rest zero)
    body[18] = 0x64;
    // new_order_ref = 200
    body[26] = 0xC8;
    // new_shares = 300 = 0x0000012C
    body[27]=0x00; body[28]=0x00; body[29]=0x01; body[30]=0x2C;
    // new_price = 500000 = 0x0007A120
    body[31]=0x00; body[32]=0x07; body[33]=0xA1; body[34]=0x20;

    OrderReplace result{};
    MessageHandlers h{};
    h.ctx = &result;
    h.on_order_replace = [](const OrderReplace& msg, void* ctx) {
        *static_cast<OrderReplace*>(ctx) = msg;
    };
    parse_message(body, sizeof(body), h);

    EXPECT_EQ(result.locate,         5u);
    EXPECT_EQ(result.timestamp,      3u);
    EXPECT_EQ(result.orig_order_ref, 100u);
    EXPECT_EQ(result.new_order_ref,  200u);
    EXPECT_EQ(result.new_shares,     300u);
    EXPECT_EQ(result.new_price,      500000u);
}

// ── Task 7: OrderExecuted ('E', 31 bytes) ───────────────────────────────────
// Layout: [0]=type [1-2]=locate [3-4]=tracking [5-10]=timestamp
//         [11-18]=order_ref [19-22]=executed_shares [23-30]=match_number
TEST(ParserTest, OrderExecuted_FieldsDecoded) {
    uint8_t body[31] = {};
    body[0] = 'E';
    body[1] = 0x00; body[2] = 0x03;  // locate = 3
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x04;  // ts=4
    // order_ref = 50
    body[18] = 0x32;
    // executed_shares = 100
    body[19]=0x00; body[20]=0x00; body[21]=0x00; body[22]=0x64;
    // match_number = 9999 = 0x000000000000270F
    body[29]=0x27; body[30]=0x0F;

    OrderExecuted result{};
    MessageHandlers h{};
    h.ctx = &result;
    h.on_order_executed = [](const OrderExecuted& msg, void* ctx) {
        *static_cast<OrderExecuted*>(ctx) = msg;
    };
    parse_message(body, sizeof(body), h);

    EXPECT_EQ(result.locate,          3u);
    EXPECT_EQ(result.timestamp,       4u);
    EXPECT_EQ(result.order_ref,       50u);
    EXPECT_EQ(result.executed_shares, 100u);
    EXPECT_EQ(result.match_number,    9999u);
}

// ── Task 8: OrderCancel ('X', 23 bytes) ─────────────────────────────────────
// Layout: [0]=type [1-2]=locate [3-4]=tracking [5-10]=timestamp
//         [11-18]=order_ref [19-22]=cancelled_shares
TEST(ParserTest, OrderCancel_FieldsDecoded) {
    uint8_t body[23] = {};
    body[0] = 'X';
    body[1] = 0x00; body[2] = 0x02;  // locate = 2
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x05;  // ts=5
    // order_ref = 77
    body[18] = 0x4D;
    // cancelled_shares = 50
    body[19]=0x00; body[20]=0x00; body[21]=0x00; body[22]=0x32;

    OrderCancel result{};
    MessageHandlers h{};
    h.ctx = &result;
    h.on_order_cancel = [](const OrderCancel& msg, void* ctx) {
        *static_cast<OrderCancel*>(ctx) = msg;
    };
    parse_message(body, sizeof(body), h);

    EXPECT_EQ(result.locate,           2u);
    EXPECT_EQ(result.timestamp,        5u);
    EXPECT_EQ(result.order_ref,        77u);
    EXPECT_EQ(result.cancelled_shares, 50u);
}

// ── Task 9: StockDirectory ('R', 39 bytes) ──────────────────────────────────
// Layout: [0]=type [1-2]=locate [3-4]=tracking [5-10]=timestamp
//         [11-18]=stock [19]=market_cat [20]=fin_status [21-24]=round_lot_size
TEST(ParserTest, StockDirectory_FieldsDecoded) {
    uint8_t body[39] = {};
    body[0] = 'R';
    body[1] = 0x00; body[2] = 0x01;  // locate = 1
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x06;  // ts=6
    memcpy(&body[11], "AAPL    ", 8);
    body[19] = 'Q';  // market_category = NASDAQ Global Select
    body[20] = 'N';  // financial_status = Normal
    // round_lot_size = 100 = 0x00000064
    body[21]=0x00; body[22]=0x00; body[23]=0x00; body[24]=0x64;

    StockDirectory result{};
    MessageHandlers h{};
    h.ctx = &result;
    h.on_stock_directory = [](const StockDirectory& msg, void* ctx) {
        *static_cast<StockDirectory*>(ctx) = msg;
    };
    parse_message(body, sizeof(body), h);

    EXPECT_EQ(result.locate,           1u);
    EXPECT_EQ(result.timestamp,        6u);
    EXPECT_STREQ(result.stock,         "AAPL");  // trailing spaces stripped
    EXPECT_EQ(result.market_category,  'Q');
    EXPECT_EQ(result.financial_status, 'N');
    EXPECT_EQ(result.round_lot_size,   100u);
}

// ── Task 10: AddOrderMPID ('F', 40 bytes) ───────────────────────────────────
// Same as 'A' but with 4-byte MPID at [36-39]
TEST(ParserTest, AddOrderMPID_FieldsDecoded) {
    uint8_t body[40] = {};
    body[0] = 'F';
    body[1] = 0x00; body[2] = 0x0A;  // locate = 10
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x07;  // ts=7
    // order_ref = 42
    body[18] = 0x2A;
    body[19] = 'S';  // side
    // shares = 200 = 0x000000C8
    body[20]=0x00; body[21]=0x00; body[22]=0x00; body[23]=0xC8;
    memcpy(&body[24], "TSLA    ", 8);
    // price = 2000000 = 0x001E8480
    body[32]=0x00; body[33]=0x1E; body[34]=0x84; body[35]=0x80;
    memcpy(&body[36], "NSDQ", 4);  // MPID

    AddOrderMPID result{};
    MessageHandlers h{};
    h.ctx = &result;
    h.on_add_order_mpid = [](const AddOrderMPID& msg, void* ctx) {
        *static_cast<AddOrderMPID*>(ctx) = msg;
    };
    parse_message(body, sizeof(body), h);

    EXPECT_EQ(result.locate,    10u);
    EXPECT_EQ(result.timestamp, 7u);
    EXPECT_EQ(result.order_ref, 42u);
    EXPECT_EQ(result.side,      (uint8_t)'S');
    EXPECT_EQ(result.shares,    200u);
    EXPECT_EQ(result.price,     2000000u);
    EXPECT_STREQ(result.stock,  "TSLA");
    EXPECT_STREQ(result.mpid,   "NSDQ");
}

// ── Task 11: OrderExecutedPrice ('C', 36 bytes) ─────────────────────────────
// Layout: [0]=type [1-2]=locate [3-4]=tracking [5-10]=timestamp
//         [11-18]=order_ref [19-22]=executed_shares [23-30]=match_number
//         [31]=printable [32-35]=execution_price
TEST(ParserTest, OrderExecutedPrice_FieldsDecoded) {
    uint8_t body[36] = {};
    body[0] = 'C';
    body[1] = 0x00; body[2] = 0x08;  // locate = 8
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x08;  // ts=8
    body[18] = 0x58;  // order_ref = 88
    // executed_shares = 75 = 0x0000004B
    body[19]=0x00; body[20]=0x00; body[21]=0x00; body[22]=0x4B;
    // match_number = 111 = 0x000000000000006F
    body[30] = 0x6F;
    body[31] = 'Y';  // printable
    // execution_price = 750000 = 0x000B71B0
    body[32]=0x00; body[33]=0x0B; body[34]=0x71; body[35]=0xB0;

    OrderExecutedPrice result{};
    MessageHandlers h{};
    h.ctx = &result;
    h.on_order_executed_price = [](const OrderExecutedPrice& msg, void* ctx) {
        *static_cast<OrderExecutedPrice*>(ctx) = msg;
    };
    parse_message(body, sizeof(body), h);

    EXPECT_EQ(result.locate,          8u);
    EXPECT_EQ(result.timestamp,       8u);
    EXPECT_EQ(result.order_ref,       88u);
    EXPECT_EQ(result.executed_shares, 75u);
    EXPECT_EQ(result.match_number,    111u);
    EXPECT_EQ(result.printable,       'Y');
    EXPECT_EQ(result.execution_price, 750000u);
}

// ── Task 12: Trade ('P', 44 bytes) ──────────────────────────────────────────
// Layout: [0]=type [1-2]=locate [3-4]=tracking [5-10]=timestamp
//         [11-18]=order_ref [19]=side [20-23]=shares [24-31]=stock
//         [32-35]=price [36-43]=match_number
TEST(ParserTest, Trade_FieldsDecoded) {
    uint8_t body[44] = {};
    body[0] = 'P';
    body[1] = 0x00; body[2] = 0x09;  // locate = 9
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x09;  // ts=9
    // order_ref = 555 = 0x000000000000022B
    body[17]=0x02; body[18]=0x2B;
    body[19] = 'S';  // side
    // shares = 1000 = 0x000003E8
    body[20]=0x00; body[21]=0x00; body[22]=0x03; body[23]=0xE8;
    memcpy(&body[24], "TSLA    ", 8);
    // price = 2000000 = 0x001E8480
    body[32]=0x00; body[33]=0x1E; body[34]=0x84; body[35]=0x80;
    // match_number = 77777 = 0x0000000000012FD1
    // 8-byte big-endian field at body[36..43]; MSB at 36, LSB at 43.
    // 77777 = 0x12FD1 -> body[41]=0x01, body[42]=0x2F, body[43]=0xD1
    body[41]=0x01; body[42]=0x2F; body[43]=0xD1;

    Trade result{};
    MessageHandlers h{};
    h.ctx = &result;
    h.on_trade = [](const Trade& msg, void* ctx) {
        *static_cast<Trade*>(ctx) = msg;
    };
    parse_message(body, sizeof(body), h);

    EXPECT_EQ(result.locate,       9u);
    EXPECT_EQ(result.timestamp,    9u);
    EXPECT_EQ(result.order_ref,    555u);
    EXPECT_EQ(result.side,         (uint8_t)'S');
    EXPECT_EQ(result.shares,       1000u);
    EXPECT_STREQ(result.stock,     "TSLA");
    EXPECT_EQ(result.price,        2000000u);
    EXPECT_EQ(result.match_number, 77777u);
}
