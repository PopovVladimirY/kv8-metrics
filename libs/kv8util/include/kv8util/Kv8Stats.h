////////////////////////////////////////////////////////////////////////////////
// kv8util/Kv8Stats.h -- statistics computation and report-writing utilities
//
// Provides:
//   Stats          -- percentile summary structure
//   ComputeStats() -- compute min/max/mean/median/stddev/percentiles
//   PrintStatsBlock()  -- write a formatted percentile table to FILE*
//   PrintHistogram()   -- write an ASCII histogram to FILE*
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

namespace kv8util {

/// Percentile summary computed by ComputeStats().
struct Stats
{
    double dMin    = 0.0;
    double dMax    = 0.0;
    double dMean   = 0.0;
    double dMedian = 0.0;
    double dStdDev = 0.0;
    double dP50    = 0.0;
    double dP75    = 0.0;
    double dP90    = 0.0;
    double dP95    = 0.0;
    double dP99    = 0.0;
    double dP999   = 0.0;
};

/// Compute percentile statistics from a vector of values.
/// The vector is sorted in-place.
Stats ComputeStats(std::vector<double> &v);

/// Write a formatted percentile table to @p f.
void PrintStatsBlock(FILE       *f,
                     const char *title,
                     const Stats &s,
                     const char  *unit,
                     uint64_t    nCount);

/// Write an ASCII histogram with @p nBuckets buckets to @p f.
/// @p sorted must already be sorted (e.g. after ComputeStats).
void PrintHistogram(FILE                       *f,
                    const std::vector<double>  &sorted,
                    const char                 *unit,
                    int                         nBuckets = 20);

} // namespace kv8util
