// src/main.cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "itch_parser.hpp"
#include "order_book.hpp"
#include "signal_engine.hpp"
#include "latency_profiler.hpp"

// ── Snapshot printer ──────────────────────────────────────────────────────────

static void print_snapshot(const MarketState* ms, const char* symbol,
                            uint16_t locate, uint64_t msg_count) {
    const OrderBook* book = ms->books[locate];
    if (!book || !book->initialized) return;

    bool has_bid = book->bid_initialized && book->bids[book->best_bid_idx].quantity > 0;
    bool has_ask = book->ask_initialized && book->asks[book->best_ask_idx].quantity > 0;
    if (!has_bid || !has_ask) return;

    uint32_t best_bid_price = book->bid_base + book->best_bid_idx;
    uint32_t best_ask_price = book->ask_base + book->best_ask_idx;
    if (best_bid_price >= best_ask_price) return;  // crossed/locked book, skip

    printf("\n[%llu msgs] %s (locate %u)\n",
           (unsigned long long)msg_count, symbol, locate);

    // Collect up to 5 ask levels above best ask (print highest first for display)
    uint32_t asks[5];
    uint32_t ask_count = 0;
    for (uint32_t i = book->best_ask_idx; i < MAX_LEVELS && ask_count < 5; ++i) {
        if (book->asks[i].quantity > 0) asks[ask_count++] = i;
    }
    for (int i = (int)ask_count - 1; i >= 0; --i) {
        uint32_t idx   = asks[i];
        uint32_t price = book->ask_base + idx;
        printf("  ASK  $%8.4f  x %6u\n",
               price / 10000.0, book->asks[idx].quantity);
    }

    printf("  --- spread: $%.4f ---\n",
           (double)(best_ask_price - best_bid_price) / 10000.0);

    // Print top 5 bid levels (best first = highest price first)
    uint32_t bid_count = 0;
    for (uint32_t i = book->best_bid_idx; bid_count < 5; --i) {
        if (book->bids[i].quantity > 0) {
            uint32_t price = book->bid_base + i;
            printf("  BID  $%8.4f  x %6u\n",
                   price / 10000.0, book->bids[i].quantity);
            ++bid_count;
        }
        if (i == 0) break;
    }
}

// ── Pipeline state ────────────────────────────────────────────────────────────

struct PipelineState {
    MarketState*    ms;
    uint64_t        msg_count = 0;
    uint16_t locate_aapl = 0, locate_msft = 0, locate_tsla = 0,
             locate_amzn = 0, locate_nvda = 0;
    bool     have_aapl = false, have_msft = false, have_tsla = false,
             have_amzn = false, have_nvda = false;
    FILE*           csv_fp   = nullptr;
    LatencyProfiler profiler;
};

static void maybe_write_signals(PipelineState* ps, uint16_t locate) {
    if (!ps->have_aapl || locate != ps->locate_aapl || !ps->csv_fp) return;
    const OrderBook* book = ps->ms->books[ps->locate_aapl];
    if (!book) return;
    Signals s = compute_signals(book);
    if (!s.valid) return;
    fprintf(ps->csv_fp, "%llu,%u,%u,%u,%.6f\n",
            (unsigned long long)ps->msg_count,
            s.spread, s.mid_price, s.microprice, (double)s.obi);
}

static void maybe_snapshot(PipelineState* ps) {
    if (ps->msg_count % 1'000'000 != 0) return;

    if (ps->have_aapl) print_snapshot(ps->ms, "AAPL", ps->locate_aapl, ps->msg_count);
    if (ps->have_msft) print_snapshot(ps->ms, "MSFT", ps->locate_msft, ps->msg_count);
    if (ps->have_tsla) print_snapshot(ps->ms, "TSLA", ps->locate_tsla, ps->msg_count);
    if (ps->have_amzn) print_snapshot(ps->ms, "AMZN", ps->locate_amzn, ps->msg_count);
    if (ps->have_nvda) print_snapshot(ps->ms, "NVDA", ps->locate_nvda, ps->msg_count);
    fflush(stdout);
}

int main(int argc, char* argv[]) {
    pin_to_core(0);   // pin to CPU 0 (P-core) before any work

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <itch_file>\n", argv[0]);
        return 1;
    }

    PipelineState ps{};
    ps.ms = create_market_state();
    ps.profiler.ghz = calibrate_rdtsc_ghz();
    fprintf(stderr, "TSC calibrated: %.3f GHz\n", ps.profiler.ghz);

    MessageHandlers h{};
    h.ctx = &ps;

    h.on_stock_directory = [](const StockDirectory& msg, void* ctx) {
        auto* ps = (PipelineState*)ctx;
        handle_stock_directory(ps->ms, msg);
        if (strcmp(msg.stock, "AAPL") == 0) { ps->locate_aapl = msg.locate; ps->have_aapl = true; }
        if (strcmp(msg.stock, "MSFT") == 0) { ps->locate_msft = msg.locate; ps->have_msft = true; }
        if (strcmp(msg.stock, "TSLA") == 0) { ps->locate_tsla = msg.locate; ps->have_tsla = true; }
        if (strcmp(msg.stock, "AMZN") == 0) { ps->locate_amzn = msg.locate; ps->have_amzn = true; }
        if (strcmp(msg.stock, "NVDA") == 0) { ps->locate_nvda = msg.locate; ps->have_nvda = true; }
    };

    h.on_add_order = [](const AddOrder& msg, void* ctx) {
        auto* ps = (PipelineState*)ctx;
        uint64_t t0 = rdtsc_start();
        handle_add_order(ps->ms, msg);
        uint64_t t1 = rdtsc_end();
        ++ps->msg_count; maybe_snapshot(ps);
        maybe_write_signals(ps, msg.locate);
        uint64_t t2 = rdtsc_end();
        ps->profiler.book.record(t1 - t0, ps->profiler.ghz);
        ps->profiler.signal.record(t2 - t1, ps->profiler.ghz);
        ps->profiler.total.record(t2 - t0, ps->profiler.ghz);
    };
    h.on_add_order_mpid = [](const AddOrderMPID& msg, void* ctx) {
        auto* ps = (PipelineState*)ctx;
        uint64_t t0 = rdtsc_start();
        handle_add_order_mpid(ps->ms, msg);
        uint64_t t1 = rdtsc_end();
        ++ps->msg_count; maybe_snapshot(ps);
        maybe_write_signals(ps, msg.locate);
        uint64_t t2 = rdtsc_end();
        ps->profiler.book.record(t1 - t0, ps->profiler.ghz);
        ps->profiler.signal.record(t2 - t1, ps->profiler.ghz);
        ps->profiler.total.record(t2 - t0, ps->profiler.ghz);
    };
    h.on_order_delete = [](const OrderDelete& msg, void* ctx) {
        auto* ps = (PipelineState*)ctx;
        uint64_t t0 = rdtsc_start();
        handle_order_delete(ps->ms, msg);
        uint64_t t1 = rdtsc_end();
        ++ps->msg_count; maybe_snapshot(ps);
        maybe_write_signals(ps, msg.locate);
        uint64_t t2 = rdtsc_end();
        ps->profiler.book.record(t1 - t0, ps->profiler.ghz);
        ps->profiler.signal.record(t2 - t1, ps->profiler.ghz);
        ps->profiler.total.record(t2 - t0, ps->profiler.ghz);
    };
    h.on_order_cancel = [](const OrderCancel& msg, void* ctx) {
        auto* ps = (PipelineState*)ctx;
        uint64_t t0 = rdtsc_start();
        handle_order_cancel(ps->ms, msg);
        uint64_t t1 = rdtsc_end();
        ++ps->msg_count; maybe_snapshot(ps);
        maybe_write_signals(ps, msg.locate);
        uint64_t t2 = rdtsc_end();
        ps->profiler.book.record(t1 - t0, ps->profiler.ghz);
        ps->profiler.signal.record(t2 - t1, ps->profiler.ghz);
        ps->profiler.total.record(t2 - t0, ps->profiler.ghz);
    };
    h.on_order_replace = [](const OrderReplace& msg, void* ctx) {
        auto* ps = (PipelineState*)ctx;
        uint64_t t0 = rdtsc_start();
        handle_order_replace(ps->ms, msg);
        uint64_t t1 = rdtsc_end();
        ++ps->msg_count; maybe_snapshot(ps);
        maybe_write_signals(ps, msg.locate);
        uint64_t t2 = rdtsc_end();
        ps->profiler.book.record(t1 - t0, ps->profiler.ghz);
        ps->profiler.signal.record(t2 - t1, ps->profiler.ghz);
        ps->profiler.total.record(t2 - t0, ps->profiler.ghz);
    };
    h.on_order_executed = [](const OrderExecuted& msg, void* ctx) {
        auto* ps = (PipelineState*)ctx;
        uint64_t t0 = rdtsc_start();
        handle_order_executed(ps->ms, msg);
        uint64_t t1 = rdtsc_end();
        ++ps->msg_count; maybe_snapshot(ps);
        maybe_write_signals(ps, msg.locate);
        uint64_t t2 = rdtsc_end();
        ps->profiler.book.record(t1 - t0, ps->profiler.ghz);
        ps->profiler.signal.record(t2 - t1, ps->profiler.ghz);
        ps->profiler.total.record(t2 - t0, ps->profiler.ghz);
    };
    h.on_order_executed_price = [](const OrderExecutedPrice& msg, void* ctx) {
        auto* ps = (PipelineState*)ctx;
        uint64_t t0 = rdtsc_start();
        handle_order_executed_price(ps->ms, msg);
        uint64_t t1 = rdtsc_end();
        ++ps->msg_count; maybe_snapshot(ps);
        maybe_write_signals(ps, msg.locate);
        uint64_t t2 = rdtsc_end();
        ps->profiler.book.record(t1 - t0, ps->profiler.ghz);
        ps->profiler.signal.record(t2 - t1, ps->profiler.ghz);
        ps->profiler.total.record(t2 - t0, ps->profiler.ghz);
    };

    ps.csv_fp = fopen("aapl_signals.csv", "w");
    if (!ps.csv_fp) { fprintf(stderr, "Cannot open aapl_signals.csv\n"); return 1; }
    fprintf(ps.csv_fp, "msg_count,spread,mid_price,microprice,obi\n");

    parse_file(argv[1], h);

    ps.profiler.print_report();

    printf("\nDone. %llu book-updating messages processed.\n",
           (unsigned long long)ps.profiler.total.total);
    printf("Active orders remaining in map: %zu\n", ps.ms->order_map.size());

    if (ps.csv_fp) fclose(ps.csv_fp);
    destroy_market_state(ps.ms);
    return 0;
}
