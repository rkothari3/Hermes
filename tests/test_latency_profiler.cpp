// tests/test_latency_profiler.cpp
#include <gtest/gtest.h>
#include "latency_profiler.hpp"

// ── StageSampler ──────────────────────────────────────────────────────────────

TEST(StageSamplerTest, EmptySampler_PercentileIsZero) {
    StageSampler s;
    EXPECT_EQ(s.percentile_ns(50.0), 0u);
    EXPECT_EQ(s.total, 0u);
}

TEST(StageSamplerTest, SingleSample_P50IsCorrect) {
    StageSampler s;
    // 1.0 GHz so cycles == ns directly; 50 cycles → 50 ns bucket
    s.record(50, 1.0);
    EXPECT_EQ(s.total, 1u);
    EXPECT_EQ(s.percentile_ns(50.0), 50u);
    EXPECT_EQ(s.percentile_ns(99.0), 50u);
}

TEST(StageSamplerTest, UniformDistribution_PercentilesCorrect) {
    StageSampler s;
    // 1000 samples: 0 ns, 1 ns, ..., 999 ns (ghz=1.0)
    for (uint64_t i = 0; i < 1000; ++i)
        s.record(i, 1.0);
    EXPECT_EQ(s.total, 1000u);
    // P50: target = round(1000*50/100) = 500; first bucket where cum>=500 is bucket 499
    EXPECT_EQ(s.percentile_ns(50.0), 499u);
    // P99: target = round(1000*99/100) = 990; first bucket where cum>=990 is bucket 989
    EXPECT_EQ(s.percentile_ns(99.0), 989u);
    // P99.9: target = round(1000*99.9/100) = round(999) = 999; first cum>=999 is bucket 998
    EXPECT_EQ(s.percentile_ns(99.9), 998u);
}

TEST(StageSamplerTest, OverflowSample_GoesToOverflowBucket) {
    StageSampler s;
    // 3000 cycles at 1.0 GHz = 3000 ns → overflow (>= PROFILER_HIST_BUCKETS)
    s.record(3000, 1.0);
    EXPECT_EQ(s.total,    1u);
    EXPECT_EQ(s.overflow, 1u);
    EXPECT_EQ(s.buckets[0], 0u);
    // percentile falls in overflow → returns UINT64_MAX
    EXPECT_EQ(s.percentile_ns(50.0), UINT64_MAX);
}

TEST(StageSamplerTest, GhzScaling_CyclesConvertedCorrectly) {
    StageSampler s;
    // At 2.0 GHz, 100 cycles = 50 ns
    s.record(100, 2.0);
    EXPECT_EQ(s.buckets[50], 1u);
    EXPECT_EQ(s.buckets[100], 0u);
}

TEST(StageSamplerTest, AllSamplesInOneBucket_PercentilesAllSame) {
    StageSampler s;
    for (int i = 0; i < 1000; ++i)
        s.record(42, 1.0);  // all 42 ns
    EXPECT_EQ(s.percentile_ns(1.0),  42u);
    EXPECT_EQ(s.percentile_ns(50.0), 42u);
    EXPECT_EQ(s.percentile_ns(99.9), 42u);
}

// ── calibrate_rdtsc_ghz ───────────────────────────────────────────────────────

TEST(CalibrationTest, CalibratedGhz_InReasonableRange) {
    // Calibration takes ~100ms — only run once per test binary execution
    double ghz = calibrate_rdtsc_ghz();
    // Any plausible modern CPU: 1.0 – 6.0 GHz
    EXPECT_GT(ghz, 1.0);
    EXPECT_LT(ghz, 6.0);
}
