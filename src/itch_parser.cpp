// src/itch_parser.cpp
#include "itch_parser.hpp"
#include <cstdio>
#include <cstring>

// ── Byte-swap helpers (file-local) ───────────────────────────────────────────

static inline uint16_t read_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline uint32_t read_u32(const uint8_t* p) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static inline uint64_t read_u64(const uint8_t* p) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

// Timestamp is 6 bytes (48-bit) in ITCH — no bswap48 exists, build manually.
static inline uint64_t read_u48(const uint8_t* p) {
    return (uint64_t)p[0] << 40 | (uint64_t)p[1] << 32 | (uint64_t)p[2] << 24 |
           (uint64_t)p[3] << 16 | (uint64_t)p[4] << 8  | (uint64_t)p[5];
}

// Copy 8-byte stock field, null-terminate, strip trailing spaces.
static inline void read_stock(const uint8_t* p, char* out) {
    memcpy(out, p, 8);
    out[8] = '\0';
    for (int i = 7; i >= 0 && out[i] == ' '; --i) out[i] = '\0';
}

// ── parse_message ────────────────────────────────────────────────────────────

void parse_message(const uint8_t* body, size_t /*len*/, const MessageHandlers& h) {
    switch (body[0]) {
        case 'A': {
            if (!h.on_add_order) break;
            AddOrder msg{};
            msg.locate    = read_u16(body + 1);
            msg.timestamp = read_u48(body + 5);
            msg.order_ref = read_u64(body + 11);
            msg.side      = body[19];
            msg.shares    = read_u32(body + 20);
            read_stock(body + 24, msg.stock);
            msg.price     = read_u32(body + 32);
            h.on_add_order(msg, h.ctx);
            break;
        }
        case 'D': {
            if (!h.on_order_delete) break;
            OrderDelete msg{};
            msg.locate    = read_u16(body + 1);
            msg.timestamp = read_u48(body + 5);
            msg.order_ref = read_u64(body + 11);
            h.on_order_delete(msg, h.ctx);
            break;
        }
        case 'U': {
            if (!h.on_order_replace) break;
            OrderReplace msg{};
            msg.locate         = read_u16(body + 1);
            msg.timestamp      = read_u48(body + 5);
            msg.orig_order_ref = read_u64(body + 11);
            msg.new_order_ref  = read_u64(body + 19);
            msg.new_shares     = read_u32(body + 27);
            msg.new_price      = read_u32(body + 31);
            h.on_order_replace(msg, h.ctx);
            break;
        }
        case 'E': {
            if (!h.on_order_executed) break;
            OrderExecuted msg{};
            msg.locate          = read_u16(body + 1);
            msg.timestamp       = read_u48(body + 5);
            msg.order_ref       = read_u64(body + 11);
            msg.executed_shares = read_u32(body + 19);
            msg.match_number    = read_u64(body + 23);
            h.on_order_executed(msg, h.ctx);
            break;
        }
        case 'X': {
            if (!h.on_order_cancel) break;
            OrderCancel msg{};
            msg.locate           = read_u16(body + 1);
            msg.timestamp        = read_u48(body + 5);
            msg.order_ref        = read_u64(body + 11);
            msg.cancelled_shares = read_u32(body + 19);
            h.on_order_cancel(msg, h.ctx);
            break;
        }
        case 'R': {
            if (!h.on_stock_directory) break;
            StockDirectory msg{};
            msg.locate           = read_u16(body + 1);
            msg.timestamp        = read_u48(body + 5);
            read_stock(body + 11, msg.stock);
            msg.market_category  = (char)body[19];
            msg.financial_status = (char)body[20];
            msg.round_lot_size   = read_u32(body + 21);
            h.on_stock_directory(msg, h.ctx);
            break;
        }
        case 'F': {
            if (!h.on_add_order_mpid) break;
            AddOrderMPID msg{};
            msg.locate    = read_u16(body + 1);
            msg.timestamp = read_u48(body + 5);
            msg.order_ref = read_u64(body + 11);
            msg.side      = body[19];
            msg.shares    = read_u32(body + 20);
            read_stock(body + 24, msg.stock);
            msg.price     = read_u32(body + 32);
            memcpy(msg.mpid, body + 36, 4);
            msg.mpid[4]   = '\0';
            h.on_add_order_mpid(msg, h.ctx);
            break;
        }
        case 'C': {
            if (!h.on_order_executed_price) break;
            OrderExecutedPrice msg{};
            msg.locate          = read_u16(body + 1);
            msg.timestamp       = read_u48(body + 5);
            msg.order_ref       = read_u64(body + 11);
            msg.executed_shares = read_u32(body + 19);
            msg.match_number    = read_u64(body + 23);
            msg.printable       = (char)body[31];
            msg.execution_price = read_u32(body + 32);
            h.on_order_executed_price(msg, h.ctx);
            break;
        }
        case 'P': {
            if (!h.on_trade) break;
            Trade msg{};
            msg.locate       = read_u16(body + 1);
            msg.timestamp    = read_u48(body + 5);
            msg.order_ref    = read_u64(body + 11);
            msg.side         = body[19];
            msg.shares       = read_u32(body + 20);
            read_stock(body + 24, msg.stock);
            msg.price        = read_u32(body + 32);
            msg.match_number = read_u64(body + 36);
            h.on_trade(msg, h.ctx);
            break;
        }
        default: break;
    }
}

// ── parse_file ───────────────────────────────────────────────────────────────

void parse_file(const char* filepath, const MessageHandlers& h) {
    FILE* f = fopen(filepath, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open '%s'\n", filepath); return; }

    static char io_buf[4 * 1024 * 1024];
    setvbuf(f, io_buf, _IOFBF, sizeof(io_buf));

    uint8_t body[65535];
    while (true) {
        uint8_t len_bytes[2];
        if (fread(len_bytes, 1, 2, f) != 2) break;
        uint16_t msg_len = (uint16_t)((uint16_t)len_bytes[0] << 8 | len_bytes[1]);
        if (msg_len == 0) continue;
        if (fread(body, 1, msg_len, f) != (size_t)msg_len) break;
        parse_message(body, msg_len, h);
    }
    fclose(f);
}
