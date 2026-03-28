# Phase 2 Parser Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build the full ITCH 5.0 parser with C++ structs, correct big-endian byte-swapping, callback interface, and Google Test unit tests verifying every message type field-by-field.

**Architecture:** Callback pattern via `MessageHandlers` struct with plain function pointers. `parse_message()` handles a single message body (used by tests). `parse_file()` wraps it with file I/O. All integer fields byte-swapped via `__builtin_bswap` or manual shifts.

**Tech Stack:** C++17, CMake 3.16+, Google Test v1.14.0 via FetchContent

---

### Task 1: Create project skeleton

**Files:**
- Create: `CMakeLists.txt`
- Create: `include/itch_parser.hpp` (empty)
- Create: `src/itch_parser.cpp` (empty)
- Create: `src/main.cpp` (stub)
- Create: `tests/test_parser.cpp` (empty)

**Step 1: Create directories**

```bash
cd /mnt/c/Users/rajg6/OneDrive/Desktop/Hermes/.worktrees/phase2-parser
mkdir -p include src tests
```

**Step 2: Create CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.16)
project(hermes CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -O2)

# Parser as a library shared between hermes binary and tests
add_library(hermes_lib src/itch_parser.cpp)
target_include_directories(hermes_lib PUBLIC include)

# Main binary
add_executable(hermes src/main.cpp)
target_link_libraries(hermes hermes_lib)

# Google Test via FetchContent (downloads automatically, no manual install)
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Test binary
enable_testing()
add_executable(hermes_tests tests/test_parser.cpp)
target_link_libraries(hermes_tests hermes_lib GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(hermes_tests)
```

**Step 3: Create stub files**

`include/itch_parser.hpp` — leave empty for now (filled in Task 2).

`src/itch_parser.cpp`:
```cpp
#include "itch_parser.hpp"
```

`src/main.cpp`:
```cpp
#include <cstdio>
int main(int argc, char* argv[]) {
    if (argc != 2) { fprintf(stderr, "Usage: %s <itch_file>\n", argv[0]); return 1; }
    fprintf(stdout, "Hermes Phase 2 — parser stub. File: %s\n", argv[1]);
    return 0;
}
```

`tests/test_parser.cpp`:
```cpp
#include <gtest/gtest.h>
// Tests added in Tasks 4-12
```

**Step 4: Verify it configures and builds**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Expected: build succeeds, `hermes_tests` binary exists.

```bash
./build/hermes_tests
```

Expected: `[==========] 0 tests from 0 test suites ran.`

**Step 5: Commit**

```bash
git add CMakeLists.txt include/ src/ tests/
git commit -m "feat: add cmake skeleton with gtest, stub parser and main"
```

---

### Task 2: Define message structs and parser interface in header

**Files:**
- Modify: `include/itch_parser.hpp`

**Why these types:** All integer fields use fixed-width types (`uint32_t` etc.) so struct size is predictable. `stock[9]` = 8 ITCH bytes + null terminator we add ourselves. No `float` anywhere — integer arithmetic only in the hot path.

**Step 1: Write the header**

```cpp
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
```

**Step 2: Build to verify it compiles**

```bash
cmake --build build
```

Expected: zero errors, zero warnings.

**Step 3: Commit**

```bash
git add include/itch_parser.hpp
git commit -m "feat: define ITCH message structs and MessageHandlers interface"
```

---

### Task 3: Add byte-swap helpers and stub parse_message / parse_file

**Files:**
- Modify: `src/itch_parser.cpp`

**Why these helpers:** ITCH sends all integers big-endian. x86 is little-endian. We must swap every multi-byte integer. The 6-byte timestamp has no `__builtin_bswap48` so we build it manually with bit shifts. `read_u32` / `read_u64` use `memcpy` + `__builtin_bswap` which compiles to a single `bswap` instruction.

**Step 1: Write the implementation stub**

```cpp
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
        // Handlers implemented in Tasks 4–12
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
```

**Step 2: Build**

```bash
cmake --build build
```

Expected: zero errors.

**Step 3: Commit**

```bash
git add src/itch_parser.cpp
git commit -m "feat: add byte-swap helpers and parse_file skeleton"
```

---

### Task 4: TDD — AddOrder ('A', 36 bytes)

**Files:**
- Modify: `tests/test_parser.cpp`
- Modify: `src/itch_parser.cpp`

**ITCH byte layout for 'A':**
```
[0]     type = 'A'
[1-2]   Stock Locate     (uint16 BE)
[3-4]   Tracking Number  (uint16 BE, we ignore)
[5-10]  Timestamp        (uint48 BE)
[11-18] Order Ref        (uint64 BE)
[19]    Side             ('B' or 'S')
[20-23] Shares           (uint32 BE)
[24-31] Stock            (8 chars, space-padded)
[32-35] Price            (uint32 BE, 4 implied decimals)
```

**Step 1: Write the failing test**

Add to `tests/test_parser.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstring>
#include "itch_parser.hpp"

// Helper: run parse_message and capture the result via callback
template<typename T>
static T capture(void (*handler)(const T&, void*), uint8_t* body, size_t len) {
    T result{};
    MessageHandlers h{};
    h.ctx = &result;
    // We assign the handler field in each test (see below)
    (void)handler;
    parse_message(body, len, h);
    return result;
}

TEST(ParserTest, AddOrder_FieldsDecoded) {
    uint8_t body[36] = {};
    body[0]  = 'A';
    // locate = 42 = 0x002A
    body[1] = 0x00; body[2] = 0x2A;
    // tracking = 0
    body[3] = 0x00; body[4] = 0x00;
    // timestamp = 1 nanosecond
    body[5]=0x00; body[6]=0x00; body[7]=0x00; body[8]=0x00; body[9]=0x00; body[10]=0x01;
    // order_ref = 999 = 0x00000000000003E7
    body[11]=0x00; body[12]=0x00; body[13]=0x00; body[14]=0x00;
    body[15]=0x00; body[16]=0x00; body[17]=0x03; body[18]=0xE7;
    // side = 'B'
    body[19] = 'B';
    // shares = 500 = 0x000001F4
    body[20]=0x00; body[21]=0x00; body[22]=0x01; body[23]=0xF4;
    // stock = "AAPL    "
    memcpy(&body[24], "AAPL    ", 8);
    // price = 1000000 = 0x000F4240  ($100.0000)
    body[32]=0x00; body[33]=0x0F; body[34]=0x42; body[35]=0x40;

    AddOrder result{};
    bool called = false;
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
```

**Step 2: Build and verify test FAILS**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.AddOrder*"
```

Expected: `FAILED` — callback never called, result stays zero-initialized.

**Step 3: Implement AddOrder parsing in parse_message**

Add to the `switch` in `src/itch_parser.cpp`:

```cpp
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
```

**Step 4: Build and verify test PASSES**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.AddOrder*"
```

Expected: `PASSED`.

**Step 5: Commit**

```bash
git add src/itch_parser.cpp tests/test_parser.cpp
git commit -m "feat: parse AddOrder ('A') with TDD — test passing"
```

---

### Task 5: TDD — OrderDelete ('D', 19 bytes)

**ITCH byte layout:**
```
[0]     type = 'D'
[1-2]   Stock Locate  (uint16 BE)
[3-4]   Tracking      (uint16 BE, ignored)
[5-10]  Timestamp     (uint48 BE)
[11-18] Order Ref     (uint64 BE)
```

**Step 1: Write the failing test**

Add to `tests/test_parser.cpp`:

```cpp
TEST(ParserTest, OrderDelete_FieldsDecoded) {
    uint8_t body[19] = {};
    body[0] = 'D';
    body[1] = 0x00; body[2] = 0x07;  // locate = 7
    body[5]=0x00; body[6]=0x00; body[7]=0x00; body[8]=0x00; body[9]=0x00; body[10]=0x02; // ts=2
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
```

**Step 2: Run — verify FAILS**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.OrderDelete*"
```

**Step 3: Implement**

```cpp
case 'D': {
    if (!h.on_order_delete) break;
    OrderDelete msg{};
    msg.locate    = read_u16(body + 1);
    msg.timestamp = read_u48(body + 5);
    msg.order_ref = read_u64(body + 11);
    h.on_order_delete(msg, h.ctx);
    break;
}
```

**Step 4: Run — verify PASSES**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.OrderDelete*"
```

**Step 5: Commit**

```bash
git add src/itch_parser.cpp tests/test_parser.cpp
git commit -m "feat: parse OrderDelete ('D') with TDD — test passing"
```

---

### Task 6: TDD — OrderReplace ('U', 35 bytes)

**ITCH byte layout:**
```
[0]     type = 'U'
[1-2]   Stock Locate       (uint16 BE)
[3-4]   Tracking           (ignored)
[5-10]  Timestamp          (uint48 BE)
[11-18] Orig Order Ref     (uint64 BE)
[19-26] New Order Ref      (uint64 BE)
[27-30] Shares             (uint32 BE)
[31-34] Price              (uint32 BE)
```

**Step 1: Write failing test**

```cpp
TEST(ParserTest, OrderReplace_FieldsDecoded) {
    uint8_t body[35] = {};
    body[0] = 'U';
    body[1] = 0x00; body[2] = 0x05;  // locate = 5
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x03; // ts=3
    // orig_order_ref = 100 = 0x64
    body[18] = 0x64;
    // new_order_ref = 200 = 0xC8
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
```

**Step 2: Run — verify FAILS**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.OrderReplace*"
```

**Step 3: Implement**

```cpp
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
```

**Step 4: Run — verify PASSES**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.OrderReplace*"
```

**Step 5: Commit**

```bash
git add src/itch_parser.cpp tests/test_parser.cpp
git commit -m "feat: parse OrderReplace ('U') with TDD — test passing"
```

---

### Task 7: TDD — OrderExecuted ('E', 31 bytes)

**ITCH byte layout:**
```
[0]     type = 'E'
[1-2]   Stock Locate      (uint16 BE)
[3-4]   Tracking          (ignored)
[5-10]  Timestamp         (uint48 BE)
[11-18] Order Ref         (uint64 BE)
[19-22] Executed Shares   (uint32 BE)
[23-30] Match Number      (uint64 BE)
```

**Step 1: Write failing test**

```cpp
TEST(ParserTest, OrderExecuted_FieldsDecoded) {
    uint8_t body[31] = {};
    body[0] = 'E';
    body[1] = 0x00; body[2] = 0x03;  // locate = 3
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x04; // ts=4
    // order_ref = 50
    body[18] = 0x32;
    // executed_shares = 100
    body[19]=0x00; body[20]=0x00; body[21]=0x00; body[22]=0x64;
    // match_number = 9999 = 0x0000000000002 70F
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
```

**Step 2: Run — verify FAILS**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.OrderExecuted_*"
```

**Step 3: Implement**

```cpp
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
```

**Step 4: Run — verify PASSES**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.OrderExecuted_*"
```

**Step 5: Commit**

```bash
git add src/itch_parser.cpp tests/test_parser.cpp
git commit -m "feat: parse OrderExecuted ('E') with TDD — test passing"
```

---

### Task 8: TDD — OrderCancel ('X', 23 bytes)

**ITCH byte layout:**
```
[0]     type = 'X'
[1-2]   Stock Locate       (uint16 BE)
[3-4]   Tracking           (ignored)
[5-10]  Timestamp          (uint48 BE)
[11-18] Order Ref          (uint64 BE)
[19-22] Cancelled Shares   (uint32 BE)
```

**Step 1: Write failing test**

```cpp
TEST(ParserTest, OrderCancel_FieldsDecoded) {
    uint8_t body[23] = {};
    body[0] = 'X';
    body[1] = 0x00; body[2] = 0x02;  // locate = 2
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x05; // ts=5
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
```

**Step 2: Run — verify FAILS**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.OrderCancel*"
```

**Step 3: Implement**

```cpp
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
```

**Step 4: Run — verify PASSES**

```bash
cmake --build build && ./build/hermes_tests
```

**Step 5: Commit**

```bash
git add src/itch_parser.cpp tests/test_parser.cpp
git commit -m "feat: parse OrderCancel ('X') with TDD — test passing"
```

---

### Task 9: TDD — StockDirectory ('R', 39 bytes)

**ITCH byte layout (key fields only):**
```
[0]     type = 'R'
[1-2]   Stock Locate        (uint16 BE)
[3-4]   Tracking            (ignored)
[5-10]  Timestamp           (uint48 BE)
[11-18] Stock               (8 chars, space-padded)
[19]    Market Category     (char)
[20]    Financial Status    (char)
[21-24] Round Lot Size      (uint32 BE)
... (remaining fields not stored in struct)
```

**Step 1: Write failing test**

```cpp
TEST(ParserTest, StockDirectory_FieldsDecoded) {
    uint8_t body[39] = {};
    body[0] = 'R';
    body[1] = 0x00; body[2] = 0x01;   // locate = 1
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x06; // ts=6
    memcpy(&body[11], "AAPL    ", 8);  // stock (space-padded)
    body[19] = 'Q';                    // market_category = NASDAQ Global Select
    body[20] = 'N';                    // financial_status = Normal
    // round_lot_size = 100 = 0x00000064
    body[21]=0x00; body[22]=0x00; body[23]=0x00; body[24]=0x64;

    StockDirectory result{};
    MessageHandlers h{};
    h.ctx = &result;
    h.on_stock_directory = [](const StockDirectory& msg, void* ctx) {
        *static_cast<StockDirectory*>(ctx) = msg;
    };
    parse_message(body, sizeof(body), h);

    EXPECT_EQ(result.locate,          1u);
    EXPECT_EQ(result.timestamp,       6u);
    EXPECT_STREQ(result.stock,        "AAPL");  // spaces stripped
    EXPECT_EQ(result.market_category, 'Q');
    EXPECT_EQ(result.financial_status,'N');
    EXPECT_EQ(result.round_lot_size,  100u);
}
```

**Step 2: Run — verify FAILS**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.StockDirectory*"
```

**Step 3: Implement**

```cpp
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
```

**Step 4: Run — verify PASSES**

```bash
cmake --build build && ./build/hermes_tests
```

**Step 5: Commit**

```bash
git add src/itch_parser.cpp tests/test_parser.cpp
git commit -m "feat: parse StockDirectory ('R') with TDD — test passing"
```

---

### Task 10: TDD — AddOrderMPID ('F', 40 bytes)

**ITCH byte layout:** Same as 'A' (36 bytes) plus 4-byte MPID at `[36-39]`.

**Step 1: Write failing test**

```cpp
TEST(ParserTest, AddOrderMPID_FieldsDecoded) {
    uint8_t body[40] = {};
    body[0] = 'F';
    body[1] = 0x00; body[2] = 0x0A;   // locate = 10
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x07; // ts=7
    // order_ref = 42
    body[18] = 0x2A;
    body[19] = 'S';                    // side = 'S' (ask)
    // shares = 200 = 0x000000C8
    body[20]=0x00; body[21]=0x00; body[22]=0x00; body[23]=0xC8;
    memcpy(&body[24], "TSLA    ", 8);
    // price = 2000000 = 0x001E8480
    body[32]=0x00; body[33]=0x1E; body[34]=0x84; body[35]=0x80;
    memcpy(&body[36], "NSDQ", 4);      // MPID

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
```

**Step 2: Run — verify FAILS**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.AddOrderMPID*"
```

**Step 3: Implement**

```cpp
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
```

**Step 4: Run — verify PASSES**

```bash
cmake --build build && ./build/hermes_tests
```

**Step 5: Commit**

```bash
git add src/itch_parser.cpp tests/test_parser.cpp
git commit -m "feat: parse AddOrderMPID ('F') with TDD — test passing"
```

---

### Task 11: TDD — OrderExecutedPrice ('C', 36 bytes)

**ITCH byte layout:**
```
[0]     type = 'C'
[1-2]   Stock Locate      (uint16 BE)
[3-4]   Tracking          (ignored)
[5-10]  Timestamp         (uint48 BE)
[11-18] Order Ref         (uint64 BE)
[19-22] Executed Shares   (uint32 BE)
[23-30] Match Number      (uint64 BE)
[31]    Printable         (char, 'Y' or 'N')
[32-35] Execution Price   (uint32 BE)
```

**Step 1: Write failing test**

```cpp
TEST(ParserTest, OrderExecutedPrice_FieldsDecoded) {
    uint8_t body[36] = {};
    body[0] = 'C';
    body[1] = 0x00; body[2] = 0x08;   // locate = 8
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x08; // ts=8
    body[18] = 0x58;                   // order_ref = 88
    body[19]=0x00; body[20]=0x00; body[21]=0x00; body[22]=0x4B; // executed_shares=75
    body[30] = 0x6F;                   // match_number = 111
    body[31] = 'Y';                    // printable
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
```

**Step 2: Run — verify FAILS**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.OrderExecutedPrice*"
```

**Step 3: Implement**

```cpp
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
```

**Step 4: Run — verify PASSES**

```bash
cmake --build build && ./build/hermes_tests
```

**Step 5: Commit**

```bash
git add src/itch_parser.cpp tests/test_parser.cpp
git commit -m "feat: parse OrderExecutedPrice ('C') with TDD — test passing"
```

---

### Task 12: TDD — Trade ('P', 44 bytes)

**ITCH byte layout:**
```
[0]     type = 'P'
[1-2]   Stock Locate   (uint16 BE)
[3-4]   Tracking       (ignored)
[5-10]  Timestamp      (uint48 BE)
[11-18] Order Ref      (uint64 BE)
[19]    Side           ('B' or 'S')
[20-23] Shares         (uint32 BE)
[24-31] Stock          (8 chars, space-padded)
[32-35] Price          (uint32 BE)
[36-43] Match Number   (uint64 BE)
```

**Step 1: Write failing test**

```cpp
TEST(ParserTest, Trade_FieldsDecoded) {
    uint8_t body[44] = {};
    body[0] = 'P';
    body[1] = 0x00; body[2] = 0x09;   // locate = 9
    body[5]=0; body[6]=0; body[7]=0; body[8]=0; body[9]=0; body[10]=0x09; // ts=9
    // order_ref = 555 = 0x022B
    body[17]=0x02; body[18]=0x2B;
    body[19] = 'S';                    // side
    // shares = 1000 = 0x000003E8
    body[20]=0x00; body[21]=0x00; body[22]=0x03; body[23]=0xE8;
    memcpy(&body[24], "TSLA    ", 8);
    // price = 2000000 = 0x001E8480
    body[32]=0x00; body[33]=0x1E; body[34]=0x84; body[35]=0x80;
    // match_number = 77777 = 0x00000000 00012FD1
    body[39]=0x01; body[40]=0x2F; body[41]=0xD1;

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
```

**Step 2: Run — verify FAILS**

```bash
cmake --build build && ./build/hermes_tests --gtest_filter="ParserTest.Trade*"
```

**Step 3: Implement**

```cpp
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
```

**Step 4: Run — ALL tests should pass now**

```bash
cmake --build build && ./build/hermes_tests
```

Expected: all 9 tests pass.

**Step 5: Commit**

```bash
git add src/itch_parser.cpp tests/test_parser.cpp
git commit -m "feat: parse Trade ('P') with TDD — all 9 parser tests passing"
```

---

### Task 13: Wire up main.cpp with count output

**Goal:** Run the parser over the real ITCH file and print per-type message counts — like Phase 1 but now going through the full struct parser.

**Files:**
- Modify: `src/main.cpp`

**Step 1: Rewrite main.cpp**

```cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "itch_parser.hpp"

struct Counts {
    uint64_t add_order        = 0;
    uint64_t add_order_mpid   = 0;
    uint64_t order_executed   = 0;
    uint64_t order_exec_price = 0;
    uint64_t order_cancel     = 0;
    uint64_t order_delete     = 0;
    uint64_t order_replace    = 0;
    uint64_t trade            = 0;
    uint64_t stock_directory  = 0;
    uint64_t total            = 0;
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <itch_file>\n", argv[0]);
        return 1;
    }

    Counts c{};
    MessageHandlers h{};
    h.ctx = &c;

    h.on_add_order        = [](const AddOrder&,           void* ctx){ auto* c = (Counts*)ctx; c->add_order++;        c->total++; };
    h.on_add_order_mpid   = [](const AddOrderMPID&,       void* ctx){ auto* c = (Counts*)ctx; c->add_order_mpid++;   c->total++; };
    h.on_order_executed   = [](const OrderExecuted&,      void* ctx){ auto* c = (Counts*)ctx; c->order_executed++;   c->total++; };
    h.on_order_executed_price = [](const OrderExecutedPrice&, void* ctx){ auto* c = (Counts*)ctx; c->order_exec_price++; c->total++; };
    h.on_order_cancel     = [](const OrderCancel&,        void* ctx){ auto* c = (Counts*)ctx; c->order_cancel++;     c->total++; };
    h.on_order_delete     = [](const OrderDelete&,        void* ctx){ auto* c = (Counts*)ctx; c->order_delete++;     c->total++; };
    h.on_order_replace    = [](const OrderReplace&,       void* ctx){ auto* c = (Counts*)ctx; c->order_replace++;    c->total++; };
    h.on_trade            = [](const Trade&,              void* ctx){ auto* c = (Counts*)ctx; c->trade++;            c->total++; };
    h.on_stock_directory  = [](const StockDirectory&,     void* ctx){ auto* c = (Counts*)ctx; c->stock_directory++;  c->total++; };

    parse_file(argv[1], h);

    printf("Parsed message counts:\n");
    printf("  Add Order (A):            %llu\n", (unsigned long long)c.add_order);
    printf("  Add Order MPID (F):       %llu\n", (unsigned long long)c.add_order_mpid);
    printf("  Order Executed (E):       %llu\n", (unsigned long long)c.order_executed);
    printf("  Order Exec w/Price (C):   %llu\n", (unsigned long long)c.order_exec_price);
    printf("  Order Cancel (X):         %llu\n", (unsigned long long)c.order_cancel);
    printf("  Order Delete (D):         %llu\n", (unsigned long long)c.order_delete);
    printf("  Order Replace (U):        %llu\n", (unsigned long long)c.order_replace);
    printf("  Trade (P):                %llu\n", (unsigned long long)c.trade);
    printf("  Stock Directory (R):      %llu\n", (unsigned long long)c.stock_directory);
    printf("  ─────────────────────────────────\n");
    printf("  Handled total:            %llu\n", (unsigned long long)c.total);
    return 0;
}
```

**Step 2: Build and run**

```bash
cmake --build build
./build/hermes /mnt/c/Users/rajg6/OneDrive/Desktop/Hermes/12302019.NASDAQ_ITCH50
```

**Step 3: Verify counts match Phase 1 output**

Cross-check against Phase 1 results:
- Add Order (A): should be **117,145,568**
- Add Order MPID (F): should be **1,485,888**
- Order Executed (E): should be **5,722,824**
- Order Cancel (X): should be **2,787,676**
- Order Delete (D): should be **114,360,997**
- Order Replace (U): should be **21,639,067**
- Trade (P): should be **1,218,602**
- Stock Directory (R): should be **8,906**

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: wire main.cpp to parse_file, verify counts match phase 1"
```

---

### Task 14: Integration test — verify count totals match Phase 1

**Files:**
- Modify: `tests/test_parser.cpp`

**Step 1: Add integration test**

```cpp
TEST(IntegrationTest, FileCountsMatchPhase1) {
    const char* path = "/mnt/c/Users/rajg6/OneDrive/Desktop/Hermes/12302019.NASDAQ_ITCH50";

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

    EXPECT_EQ(c.add_order,       117145568ULL);
    EXPECT_EQ(c.add_order_mpid,   1485888ULL);
    EXPECT_EQ(c.order_executed,   5722824ULL);
    EXPECT_EQ(c.order_exec_price,   99917ULL);
    EXPECT_EQ(c.order_cancel,     2787676ULL);
    EXPECT_EQ(c.order_delete,   114360997ULL);
    EXPECT_EQ(c.order_replace,   21639067ULL);
    EXPECT_EQ(c.trade,            1218602ULL);
    EXPECT_EQ(c.stock_dir,           8906ULL);
}
```

**Step 2: Run**

```bash
./build/hermes_tests --gtest_filter="IntegrationTest*"
```

Expected: PASSED. (This test takes ~15-30 seconds — it reads the full 10GB file.)

**Step 3: Commit**

```bash
git add tests/test_parser.cpp
git commit -m "test: add integration test verifying all counts match phase 1"
```

---

### Task 15: Merge to main

**Step 1: Run full test suite one final time**

```bash
cmake --build build && ./build/hermes_tests
```

Expected: all tests pass (unit + integration).

**Step 2: Merge**

```bash
cd /mnt/c/Users/rajg6/OneDrive/Desktop/Hermes
git merge phase2-parser
```

**Step 3: Update CLAUDE.md status**

Change the `## Current Status` section to:

```
**Phase 2 complete (2026-03-28).** Full parser with structs, byte-swapping, and unit tests.
- All 9 message types parsed and tested field-by-field
- Counts verified against Phase 1 output
- Google Test via FetchContent, CMake build system in place
- Worktree: phase2-parser branch merged to main

**Next: Phase 3** — Order book updater (array-indexed, cache-friendly).
```

**Step 4: Commit and clean up worktree**

```bash
git add CLAUDE.md
git commit -m "docs: mark phase 2 complete in CLAUDE.md"
git worktree remove .worktrees/phase2-parser
git branch -d phase2-parser
```
