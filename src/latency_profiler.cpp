// src/latency_profiler.cpp
#include "latency_profiler.hpp"
#include <ctime>
#include <cstring>
#include <pthread.h>
#include <climits>

double calibrate_rdtsc_ghz() {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t r0 = rdtsc_start();

    struct timespec sleep_req = {0, 100'000'000};  // 100 ms
    nanosleep(&sleep_req, nullptr);

    uint64_t r1 = rdtsc_end();
    clock_gettime(CLOCK_MONOTONIC, &t1);

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
    uint64_t target = static_cast<uint64_t>(total * pct / 100.0);
    uint64_t cum = 0;
    for (uint32_t i = 0; i < PROFILER_HIST_BUCKETS; ++i) {
        cum += buckets[i];
        if (cum > target) return i;
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
        auto fmt_ns = [](uint64_t v) -> const char* {
            static char buf[32];
            if (v == UINT64_MAX) snprintf(buf, sizeof(buf), " >2000 ns");
            else                 snprintf(buf, sizeof(buf), "%7llu ns", (unsigned long long)v);
            return buf;
        };
        printf("%-20s %9s %9s %9s %12llu\n",
               name,
               fmt_ns(s.percentile_ns(50.0)),
               fmt_ns(s.percentile_ns(99.0)),
               fmt_ns(s.percentile_ns(99.9)),
               (unsigned long long)s.total);
    };

    print_row("Book Update",    book);
    print_row("Signal Compute", signal);
    print_row("Full Callback",  total);

    printf("\nTSC rate: %.3f GHz (calibrated via CLOCK_MONOTONIC)\n", ghz);

    // Book Update histogram (10 ns bins, up to 200 ns)
    printf("\nBook Update latency histogram (10 ns bins):\n");
    bool printed_any = false;
    for (uint32_t bin = 0; bin < 200; bin += 10) {
        uint64_t count = 0;
        for (uint32_t i = bin; i < bin + 10 && i < PROFILER_HIST_BUCKETS; ++i)
            count += book.buckets[i];
        if (count == 0) continue;
        printed_any = true;
        double pct = 100.0 * count / book.total;
        printf("  [%3u-%3u ns] %10llu  (%.2f%%)\n",
               bin, bin + 9,
               (unsigned long long)count, pct);
    }
    if (!printed_any)
        printf("  (no samples in 0-199 ns range)\n");
    if (book.overflow > 0)
        printf("  [>=2000 ns]  %10llu  (%.2f%%)\n",
               (unsigned long long)book.overflow,
               100.0 * book.overflow / book.total);
}
