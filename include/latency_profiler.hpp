// include/latency_profiler.hpp
#pragma once
#include <cstdint>
#include <cstdio>

// ── RDTSC ─────────────────────────────────────────────────────────────────────

inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Calibrate TSC frequency by comparing TSC delta to CLOCK_MONOTONIC over ~100ms.
// Returns GHz (cycles per nanosecond). Call once at startup, before hot loop.
double calibrate_rdtsc_ghz();

// Pin calling thread to the given logical CPU (0-based).
// Uses pthread_setaffinity_np. No-op if affinity setting fails (prints warning).
void pin_to_core(int cpu_id);

// ── Histogram sampler ─────────────────────────────────────────────────────────

// Fixed-memory histogram: 2000 buckets of 1 ns each (covers 0–1999 ns).
// Latencies ≥ 2000 ns go into the overflow counter.
static constexpr uint32_t PROFILER_HIST_BUCKETS = 2000;

struct StageSampler {
    uint64_t buckets[PROFILER_HIST_BUCKETS] = {};
    uint64_t overflow = 0;
    uint64_t total    = 0;

    // Record one sample given raw TSC cycle delta and the pre-calibrated GHz value.
    void record(uint64_t cycles, double ghz) {
        ++total;
        uint64_t ns = static_cast<uint64_t>(cycles / ghz);
        if (ns < PROFILER_HIST_BUCKETS) ++buckets[ns];
        else                            ++overflow;
    }

    // Return the value at the given percentile (0–100).
    // Returns PROFILER_HIST_BUCKETS if the percentile falls in the overflow bucket.
    uint64_t percentile_ns(double pct) const;
};

// ── LatencyProfiler ──────────────────────────────────────────────────────────

struct LatencyProfiler {
    StageSampler book;    // handle_*() duration
    StageSampler signal;  // compute_signals() duration
    StageSampler total;   // full callback duration (book + signal + overhead)
    double       ghz = 1.0;

    // Print benchmark table + per-stage histograms to stdout.
    void print_report() const;
};
