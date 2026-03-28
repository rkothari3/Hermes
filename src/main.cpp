#include <cstdio>
#include <cstdint>
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

    h.on_add_order            = [](const AddOrder&,            void* ctx){ auto* c = (Counts*)ctx; c->add_order++;        c->total++; };
    h.on_add_order_mpid       = [](const AddOrderMPID&,        void* ctx){ auto* c = (Counts*)ctx; c->add_order_mpid++;   c->total++; };
    h.on_order_executed       = [](const OrderExecuted&,       void* ctx){ auto* c = (Counts*)ctx; c->order_executed++;   c->total++; };
    h.on_order_executed_price = [](const OrderExecutedPrice&,  void* ctx){ auto* c = (Counts*)ctx; c->order_exec_price++; c->total++; };
    h.on_order_cancel         = [](const OrderCancel&,         void* ctx){ auto* c = (Counts*)ctx; c->order_cancel++;     c->total++; };
    h.on_order_delete         = [](const OrderDelete&,         void* ctx){ auto* c = (Counts*)ctx; c->order_delete++;     c->total++; };
    h.on_order_replace        = [](const OrderReplace&,        void* ctx){ auto* c = (Counts*)ctx; c->order_replace++;    c->total++; };
    h.on_trade                = [](const Trade&,               void* ctx){ auto* c = (Counts*)ctx; c->trade++;            c->total++; };
    h.on_stock_directory      = [](const StockDirectory&,      void* ctx){ auto* c = (Counts*)ctx; c->stock_directory++;  c->total++; };

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
    printf("  ---------------------------------\n");
    printf("  Handled total:            %llu\n", (unsigned long long)c.total);
    return 0;
}
