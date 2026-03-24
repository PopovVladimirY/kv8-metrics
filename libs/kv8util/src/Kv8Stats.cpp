////////////////////////////////////////////////////////////////////////////////
// Kv8Stats.cpp -- statistics computation and report-writing utilities
////////////////////////////////////////////////////////////////////////////////

#include <kv8util/Kv8Stats.h>

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <numeric>
#include <string>

namespace kv8util {

Stats ComputeStats(std::vector<double> &v)
{
    assert(!v.empty());
    std::sort(v.begin(), v.end());

    const size_t N = v.size();
    Stats s{};

    s.dMin = v[0];
    s.dMax = v[N - 1];

    // Mean
    double sum = std::accumulate(v.begin(), v.end(), 0.0);
    s.dMean = sum / (double)N;

    // Median (p50 exact)
    if (N % 2 == 0)
        s.dMedian = (v[N / 2 - 1] + v[N / 2]) * 0.5;
    else
        s.dMedian = v[N / 2];

    // Standard deviation
    double sq = 0.0;
    for (double x : v) { double d = x - s.dMean; sq += d * d; }
    s.dStdDev = std::sqrt(sq / (double)N);

    // Percentile helper: nearest-rank method
    auto pct = [&](double p) -> double {
        size_t idx = (size_t)std::ceil(p / 100.0 * (double)N);
        if (idx >= N) idx = N - 1;
        return v[idx];
    };

    s.dP50  = pct(50.0);
    s.dP75  = pct(75.0);
    s.dP90  = pct(90.0);
    s.dP95  = pct(95.0);
    s.dP99  = pct(99.0);
    s.dP999 = pct(99.9);

    return s;
}

void PrintStatsBlock(FILE       *f,
                     const char *title,
                     const Stats &s,
                     const char  *unit,
                     uint64_t    nCount)
{
    fprintf(f,
        "  %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10.3f %s\n"
        "  %-14s  %10" PRIu64 " messages\n\n",
        title,
        "min",    s.dMin,           unit,
        "p50",    s.dP50,           unit,
        "p75",    s.dP75,           unit,
        "median", s.dMedian,        unit,
        "mean",   s.dMean,          unit,
        "stddev", s.dStdDev,        unit,
        "p90",    s.dP90,           unit,
        "p95",    s.dP95,           unit,
        "p99",    s.dP99,           unit,
        "p99.9",  s.dP999,          unit,
        "max",    s.dMax,           unit,
        "range",  s.dMax - s.dMin,  unit,
        "N",      nCount
    );
}

void PrintHistogram(FILE                       *f,
                    const std::vector<double>  &sorted,
                    const char                 *unit,
                    int                         nBuckets)
{
    if (sorted.empty()) return;
    const double lo  = sorted.front();
    const double hi  = sorted.back();
    if (hi <= lo) { fprintf(f, "  (all values identical: %.3f %s)\n\n", lo, unit); return; }

    const double span     = hi - lo;
    const double bucketW  = span / (double)nBuckets;

    std::vector<size_t> counts((size_t)nBuckets, 0);
    for (double v : sorted)
    {
        int b = (int)((v - lo) / bucketW);
        if (b >= nBuckets) b = nBuckets - 1;
        counts[(size_t)b]++;
    }

    size_t peak = *std::max_element(counts.begin(), counts.end());
    const int   barWidth = 40;

    char colHeader[32];
    snprintf(colHeader, sizeof(colHeader), "Bucket (%s)", unit);
    fprintf(f, "  %-20s  %-40s  Count\n", colHeader, "Bar");
    fprintf(f, "  %s\n", std::string(78, '-').c_str());
    for (int b = 0; b < nBuckets; ++b)
    {
        double bLo = lo + b       * bucketW;
        double bHi = lo + (b + 1) * bucketW;
        int    bar = (peak > 0)
                     ? (int)((double)counts[(size_t)b] / (double)peak * barWidth)
                     : 0;
        fprintf(f, "  [%8.3f-%8.3f)  %s%s  %zu\n",
                bLo, bHi,
                std::string((size_t)bar, '#').c_str(),
                std::string((size_t)(barWidth - bar), ' ').c_str(),
                counts[(size_t)b]);
    }
    fprintf(f, "\n");
}

} // namespace kv8util
