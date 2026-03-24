// kv8scope -- Kv8 Software Oscilloscope
// StatsEngine.h -- Full-duration running accumulators and P2 median engine.
//
// P4.1: StatsEngine maintains one FullDurationStats per counter, updated on
//       the consumer thread via Feed() with O(1) cost per sample.
//       The UI thread reads with relaxed atomic loads (benign tear acceptable
//       for display purposes).
//
// P4.2: P2Median implements the Jain & Chlamtac (1985) P-square algorithm
//       for online incremental median estimation without storing all samples.
//       One instance is embedded in WaveformRenderer::CounterData and is
//       reset + refed on each visible-window scan in QueryStats().

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>

// ---------------------------------------------------------------------------
// P2Median -- P-square algorithm for incremental quantile approximation.
//
// Maintains 5 markers that converge to the true median.
// Feed(x) ingests one sample.  Get() returns the current estimate.
// Reset() clears all state so the object can be reused for a new window.
// ---------------------------------------------------------------------------

class P2Median
{
public:
    P2Median() { Reset(); }

    void Reset()
    {
        m_n = 0;
        for (int i = 0; i < kM; ++i)
        {
            m_q[i]    = 0.0;
            m_pos[i]  = i + 1;
            m_ndes[i] = static_cast<double>(i + 1);
        }
    }

    // Feed one observation.
    void Feed(double x)
    {
        if (m_n < kM)
        {
            // Accumulate first kM samples; sort when full.
            m_q[m_n++] = x;
            if (m_n == kM)
            {
                // Insertion sort  to put markers in ascending order.
                for (int i = 1; i < kM; ++i)
                {
                    double v = m_q[i]; int j = i - 1;
                    while (j >= 0 && m_q[j] > v) { m_q[j + 1] = m_q[j]; --j; }
                    m_q[j + 1] = v;
                }
            }
            return;
        }

        // Increment desired positions (dn = [0, 0.25, 0.5, 0.75, 1.0] for p=0.5).
        static constexpr double kDn[kM] = { 0.0, 0.25, 0.50, 0.75, 1.0 };
        for (int i = 0; i < kM; ++i) m_ndes[i] += kDn[i];
        ++m_n;

        // Find the interval k where x falls.
        int k;
        if      (x < m_q[0]) { m_q[0] = x; k = 0; }
        else if (x < m_q[1]) { k = 0; }
        else if (x < m_q[2]) { k = 1; }
        else if (x < m_q[3]) { k = 2; }
        else if (x <= m_q[4]) { k = 3; }
        else                   { m_q[4] = x; k = 3; }

        // Increment actual positions of markers k+1 ... 4.
        for (int i = k + 1; i < kM; ++i) ++m_pos[i];

        // Adjust inner markers 1..3.
        for (int i = 1; i <= 3; ++i)
        {
            double d = m_ndes[i] - static_cast<double>(m_pos[i]);
            if ((d >= 1.0  && m_pos[i + 1] - m_pos[i] > 1) ||
                (d <= -1.0 && m_pos[i - 1] - m_pos[i] < -1))
            {
                int di = (d > 0.0) ? 1 : -1;
                double qnew = Parabolic(i, di);
                // Fall back to linear if parabolic overshoots the bracket.
                if (qnew <= m_q[i - 1] || qnew >= m_q[i + 1])
                    qnew = m_q[i] + static_cast<double>(di)
                                  * (m_q[i + di] - m_q[i])
                                  / static_cast<double>(m_pos[i + di] - m_pos[i]);
                m_q[i]   = qnew;
                m_pos[i] += di;
            }
        }
    }

    // Return the current median estimate.
    double Get() const
    {
        if (m_n == 0) return 0.0;
        if (m_n < kM)
        {
            // Exact median of the few initial unsorted samples (copy + sort).
            double tmp[kM];
            for (int i = 0; i < m_n; ++i) tmp[i] = m_q[i];
            for (int i = 1; i < m_n; ++i)
            {
                double v = tmp[i]; int j = i - 1;
                while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; --j; }
                tmp[j + 1] = v;
            }
            return tmp[m_n / 2];
        }
        return m_q[2]; // marker [2] is the half-quantile estimate
    }

    int Count() const { return m_n; }

private:
    static constexpr int kM = 5;

    int    m_n;
    double m_q[kM];      // marker heights
    int    m_pos[kM];    // actual marker positions (integer, 1-based)
    double m_ndes[kM];   // desired marker positions (real)

    // Piecewise parabolic interpolation at marker i, direction di (+/-1).
    double Parabolic(int i, int di) const
    {
        double fdi = static_cast<double>(di);
        return m_q[i]
             + fdi / static_cast<double>(m_pos[i + 1] - m_pos[i - 1])
             * (  static_cast<double>(m_pos[i]     - m_pos[i - 1] + di)
                  * (m_q[i + 1] - m_q[i])
                  / static_cast<double>(m_pos[i + 1] - m_pos[i])
                + static_cast<double>(m_pos[i + 1] - m_pos[i]     - di)
                  * (m_q[i]     - m_q[i - 1])
                  / static_cast<double>(m_pos[i]     - m_pos[i - 1]) );
    }
};

// ---------------------------------------------------------------------------
// FullDurationStats -- per-counter cache-line-aligned running stats struct.
//
// Written by the consumer thread; read from the UI thread with relaxed
// ordering.  Cache-line aligned to prevent false sharing in the map.
// ---------------------------------------------------------------------------

struct alignas(64) FullDurationStats
{
    std::atomic<uint64_t> count  {0};
    std::atomic<double>   dMin   {std::numeric_limits<double>::max()};
    std::atomic<double>   dMax   {std::numeric_limits<double>::lowest()};
    // dSum and dSumSq are written only by the consumer thread (single writer).
    // UI thread reads with relaxed ordering -- benign tear is acceptable for
    // display-only use (workplan explicitly permits this).
    std::atomic<double>   dSum   {0.0};
    std::atomic<double>   dSumSq {0.0};
};

// ---------------------------------------------------------------------------
// StatsEngine
// ---------------------------------------------------------------------------

class StatsEngine
{
public:
    StatsEngine()  = default;
    ~StatsEngine() = default;

    // Non-copyable, non-movable.
    StatsEngine(const StatsEngine&)            = delete;
    StatsEngine& operator=(const StatsEngine&) = delete;
    StatsEngine(StatsEngine&&)                 = delete;
    StatsEngine& operator=(StatsEngine&&)      = delete;

    /// Register a counter.  Call from the main thread before the consumer
    /// thread starts.
    void RegisterCounter(uint32_t dwHash, uint16_t wID);

    /// Register a counter only if not already known.  Safe to call while
    /// the consumer thread is running (std::map node insert does not
    /// invalidate existing iterators).  Returns true if newly registered.
    bool RegisterCounterIfNew(uint32_t dwHash, uint16_t wID);

    /// Feed a new sample.  Called from the consumer thread (hot path).
    /// O(1) with at most one CAS loop per call for min/max.
    void Feed(uint32_t dwHash, uint16_t wID, double dValue);

    /// Query full-duration stats.  Safe from any thread (relaxed reads).
    /// Returns false if the counter is not registered or has no samples.
    bool GetFullStats(uint32_t   dwHash,
                      uint16_t   wID,
                      uint64_t&  nOut,
                      double&    dMinOut,
                      double&    dMaxOut,
                      double&    dAvgOut) const;

    /// Composite key -- exposed so callers can enumerate counters if needed.
    static uint64_t MakeKey(uint32_t dwHash, uint16_t wID)
    { return (static_cast<uint64_t>(dwHash) << 16) | wID; }

private:
    // CAS-loop helpers for atomic min/max on double.
    static void AtomicMin(std::atomic<double>& a, double v);
    static void AtomicMax(std::atomic<double>& a, double v);

    /// Per-counter stats.  std::map node-based storage ensures each
    /// FullDurationStats object is individually heap-allocated with its
    /// alignas(64) requirement respected.
    std::map<uint64_t, FullDurationStats> m_stats;
};
