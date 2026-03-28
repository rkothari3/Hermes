// include/itch_parser.hpp
#pragma once
#include <cstdint>
#include <cstddef>

// ── Message structs ──────────────────────────────────────────────────────────
// All integer fields are decoded from big-endian at parse time.
// stock[9]: 8 bytes from wire, null-terminated + space-stripped by parser.

struct StockDirectory {
    uint64_t timestamp;
    uint32_t round_lot_size;
    uint16_t locate;
    char     stock[9];
    char     market_category;
    char     financial_status;
};

struct AddOrder {
    uint64_t timestamp;
    uint64_t order_ref;
    uint32_t price;
    uint32_t shares;
    uint16_t locate;
    char     stock[9];
    uint8_t  side;   // 'B' or 'S'
};

struct AddOrderMPID {
    uint64_t timestamp;
    uint64_t order_ref;
    uint32_t price;
    uint32_t shares;
    uint16_t locate;
    char     stock[9];
    char     mpid[5];  // 4-byte attribution + null terminator
    uint8_t  side;
};

struct OrderExecuted {
    uint64_t timestamp;
    uint64_t order_ref;
    uint64_t match_number;
    uint32_t executed_shares;
    uint16_t locate;
};

struct OrderExecutedPrice {
    uint64_t timestamp;
    uint64_t order_ref;
    uint64_t match_number;
    uint32_t executed_shares;
    uint32_t execution_price;
    uint16_t locate;
    char     printable;
};

struct OrderCancel {
    uint64_t timestamp;
    uint64_t order_ref;
    uint32_t cancelled_shares;
    uint16_t locate;
};

struct OrderDelete {
    uint64_t timestamp;
    uint64_t order_ref;
    uint16_t locate;
};

struct OrderReplace {
    uint64_t timestamp;
    uint64_t orig_order_ref;
    uint64_t new_order_ref;
    uint32_t new_shares;
    uint32_t new_price;
    uint16_t locate;
};

struct Trade {
    uint64_t timestamp;
    uint64_t order_ref;
    uint64_t match_number;
    uint32_t shares;
    uint32_t price;
    uint16_t locate;
    char     stock[9];
    uint8_t  side;
};

// ── Callback interface ───────────────────────────────────────────────────────
// Plain function pointers (not std::function) — zero allocation overhead.
// void* ctx is passed through every callback so the caller can reach their state
// without globals. Set unused handlers to nullptr; parser will skip them.

struct MessageHandlers {
    void (*on_stock_directory)     (const StockDirectory&,    void* ctx) = nullptr;
    void (*on_add_order)           (const AddOrder&,          void* ctx) = nullptr;
    void (*on_add_order_mpid)      (const AddOrderMPID&,      void* ctx) = nullptr;
    void (*on_order_executed)      (const OrderExecuted&,     void* ctx) = nullptr;
    void (*on_order_executed_price)(const OrderExecutedPrice&,void* ctx) = nullptr;
    void (*on_order_cancel)        (const OrderCancel&,       void* ctx) = nullptr;
    void (*on_order_delete)        (const OrderDelete&,       void* ctx) = nullptr;
    void (*on_order_replace)       (const OrderReplace&,      void* ctx) = nullptr;
    void (*on_trade)               (const Trade&,             void* ctx) = nullptr;
    void* ctx = nullptr;
};

// ── Public API ───────────────────────────────────────────────────────────────
// parse_message: parse one message body (body[0] is the type byte).
//   Used by unit tests — no file I/O needed.
// parse_file: open filepath, read all messages, dispatch via handlers.
void parse_message(const uint8_t* body, size_t len, const MessageHandlers& h);
void parse_file(const char* filepath, const MessageHandlers& h);
