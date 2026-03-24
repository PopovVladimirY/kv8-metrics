// kv8scope -- Kv8 Software Oscilloscope
// StatsEngine.cpp -- Full-duration running accumulator implementation.

#include "StatsEngine.h"

// ---------------------------------------------------------------------------
// RegisterCounter
// ---------------------------------------------------------------------------

void StatsEngine::RegisterCounter(uint32_t dwHash, uint16_t wID)
{
    m_stats.emplace(std::piecewise_construct,
                    std::forward_as_tuple(MakeKey(dwHash, wID)),
                    std::forward_as_tuple());
}

bool StatsEngine::RegisterCounterIfNew(uint32_t dwHash, uint16_t wID)
{
    uint64_t key = MakeKey(dwHash, wID);
    if (m_stats.find(key) != m_stats.end())
        return false;
    RegisterCounter(dwHash, wID);
    return true;
}

// ---------------------------------------------------------------------------
// Feed (hot path -- called from consumer thread)
// ---------------------------------------------------------------------------

void StatsEngine::Feed(uint32_t dwHash, uint16_t wID, double dValue)
{
    auto it = m_stats.find(MakeKey(dwHash, wID));
    if (it == m_stats.end())
        return;

    FullDurationStats& s = it->second;
    s.count.fetch_add(1, std::memory_order_relaxed);
    AtomicMin(s.dMin, dValue);
    AtomicMax(s.dMax, dValue);

    // dSum / dSumSq: single-writer (consumer thread), so load+compute+store
    // is safe -- there is no concurrent writer.
    double dSum = s.dSum.load(std::memory_order_relaxed) + dValue;
    s.dSum.store(dSum, std::memory_order_relaxed);
    double dSumSq = s.dSumSq.load(std::memory_order_relaxed) + dValue * dValue;
    s.dSumSq.store(dSumSq, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// GetFullStats (UI thread)
// ---------------------------------------------------------------------------

bool StatsEngine::GetFullStats(uint32_t  dwHash,
                               uint16_t  wID,
                               uint64_t& nOut,
                               double&   dMinOut,
                               double&   dMaxOut,
                               double&   dAvgOut) const
{
    auto it = m_stats.find(MakeKey(dwHash, wID));
    if (it == m_stats.end())
        return false;

    const FullDurationStats& s = it->second;
    nOut    = s.count.load(std::memory_order_relaxed);
    dMinOut = s.dMin.load(std::memory_order_relaxed);
    dMaxOut = s.dMax.load(std::memory_order_relaxed);
    double dSum = s.dSum.load(std::memory_order_relaxed);
    dAvgOut = (nOut > 0) ? (dSum / static_cast<double>(nOut)) : 0.0;
    return (nOut > 0);
}

// ---------------------------------------------------------------------------
// Atomic min / max helpers (CAS loops on double)
// ---------------------------------------------------------------------------

void StatsEngine::AtomicMin(std::atomic<double>& a, double v)
{
    double cur = a.load(std::memory_order_relaxed);
    while (v < cur &&
           !a.compare_exchange_weak(cur, v, std::memory_order_relaxed))
    {}
}

void StatsEngine::AtomicMax(std::atomic<double>& a, double v)
{
    double cur = a.load(std::memory_order_relaxed);
    while (v > cur &&
           !a.compare_exchange_weak(cur, v, std::memory_order_relaxed))
    {}
}
