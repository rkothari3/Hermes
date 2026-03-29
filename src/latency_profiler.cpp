// src/latency_profiler.cpp
#include "latency_profiler.hpp"
#include <ctime>
#include <pthread.h>
#include <climits>

double calibrate_rdtsc_ghz() {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t r0 = rdtsc_start();

    // Busy-wait for 100ms — avoids WSL2 nanosleep inaccuracy
    do {
        clock_gettime(CLOCK_MONOTONIC, &t1);
    } while ((t1.tv_sec - t0.tv_sec) * 1000000000LL +
             (t1.tv_nsec - t0.tv_nsec) < 100000000LL);

    uint64_t r1 = rdtsc_end();

    double elapsed_ns = (t1.tv_sec  - t0.tv_sec)  * 1e9
                      + (t1.tv_nsec - t0.tv_nsec);
    return static_cast<double>(r1 - r0) / elapsed_ns;  // cycles/ns = GHz
}

void pin_to_core(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (rc != 0)
        fprintf(stderr, "Warning: could not pin to CPU %d (rc=%d)\n", cpu_id, rc);
}

uint64_t StageSampler::percentile_ns(double pct) const {
    if (total == 0) return 0;
    // ceil(total * pct / 100) gives the correct rank for the pct-th percentile
    uint64_t target = static_cast<uint64_t>(total * pct / 100.0 + 0.5);
    if (target == 0) target = 1;
    uint64_t cum = 0;
    for (uint32_t i = 0; i < PROFILER_HIST_BUCKETS; ++i) {
        cum += buckets[i];
        if (cum >= target) return i;
    }
    return UINT64_MAX;  // overflow bucket
}

void LatencyProfiler::print_report() const {
    printf("\n");
    printf("%-20s %8s %8s %8s %12s\n",
           "Stage", "P50", "P99", "P99.9", "Samples");
    printf("%-20s %8s %8s %8s %12s\n",
           "────────────────────",
           "────────", "────────", "────────", "────────────");

    auto print_row = [](const char* name, const StageSampler& s) {
        auto fmt_ns = [](uint64_t v, char* buf, size_t len) {
            if (v == UINT64_MAX) snprintf(buf, len, " >2000 ns");
            else                 snprintf(buf, len, "%7llu ns", (unsigned long long)v);
        };
        char p50[32], p99[32], p999[32];
        fmt_ns(s.percentile_ns(50.0),  p50,  sizeof(p50));
        fmt_ns(s.percentile_ns(99.0),  p99,  sizeof(p99));
        fmt_ns(s.percentile_ns(99.9),  p999, sizeof(p999));
        printf("%-20s %9s %9s %9s %12llu\n",
               name, p50, p99, p999,
               (unsigned long long)s.total);
    };

    print_row("Book Update",    book);
    print_row("Signal Compute", signal);
    print_row("Full Callback",  total);

    printf("\nTSC rate: %.3f GHz (calibrated via CLOCK_MONOTONIC)\n", ghz);

    // Book Update histogram (10 ns bins, full range)
    printf("\nBook Update latency histogram (10 ns bins):\n");
    bool printed_any = false;
    for (uint32_t bin = 0; bin < PROFILER_HIST_BUCKETS; bin += 10) {
        uint64_t count = 0;
        for (uint32_t i = bin; i < bin + 10 && i < PROFILER_HIST_BUCKETS; ++i)
            count += book.buckets[i];
        if (count == 0) continue;
        printed_any = true;
        double pct = 100.0 * count / book.total;
        printf("  [%4u-%4u ns] %10llu  (%.2f%%)\n",
               bin, bin + 9,
               (unsigned long long)count, pct);
    }
    if (!printed_any)
        printf("  (no samples)\n");
    if (book.overflow > 0)
        printf("  [>=2000 ns]   %10llu  (%.2f%%)\n",
               (unsigned long long)book.overflow,
               100.0 * book.overflow / book.total);
}
