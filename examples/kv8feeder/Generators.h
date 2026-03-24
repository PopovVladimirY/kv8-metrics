////////////////////////////////////////////////////////////////////////////////
// kv8log/examples/kv8feeder/Generators.h
//
// Three signal generators used by the kv8feeder example.
//
//   WrappingCounter  -- monotonically incrementing integer wrapping at 1023
//   GaussWalk        -- Gaussian random walk clamped to [min, max]
//   PhaseCyclogram   -- piecewise-constant level with random hold duration
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <random>
#include <cstdint>

// ── WrappingCounter ─────────────────────────────────────────────────────────
// Increments by 1 each call, wrapping from max back to 0.
// Used for "Numbers/Counter" -- the rolling value lets consumers verify that
// no samples are dropped.
class WrappingCounter
{
public:
    explicit WrappingCounter(double max = 1023.0)
        : m_max(max), m_value(0.0) {}

    double Next()
    {
        double v = m_value;
        m_value += 1.0;
        if (m_value > m_max) m_value = 0.0;
        return v;
    }

private:
    double m_max;
    double m_value;
};

// ── GaussWalk ───────────────────────────────────────────────────────────────
// Random walk with Gaussian steps clamped (reflected) to [min, max].
class GaussWalk
{
public:
    GaussWalk(double min, double max, double sigma = 20.0, unsigned seed = 42)
        : m_min(min), m_max(max), m_value(0.0)
        , m_rng(seed), m_dist(0.0, sigma) {}

    double Next()
    {
        double step  = m_dist(m_rng);
        double next  = m_value + step;
        // Reflect at boundaries.
        while (next > m_max || next < m_min) {
            if (next > m_max) next = 2.0 * m_max - next;
            if (next < m_min) next = 2.0 * m_min - next;
        }
        m_value = next;
        return m_value;
    }

private:
    double m_min, m_max, m_value;
    std::mt19937                          m_rng;
    std::normal_distribution<double>      m_dist;
};

// ── PhaseCyclogram ──────────────────────────────────────────────────────────
// Holds a constant level for a random number of ticks, then jumps to a new
// random level.  The caller decides what one "tick" means in wall-clock time.
//
// 'hold_ticks_mean' is the average number of Next() calls before a step.
// A geometric distribution is used so individual hold durations are
// exponentially distributed (memoryless).
class PhaseCyclogram
{
public:
    PhaseCyclogram(double min, double max,
                   double hold_ticks_mean = 1000.0,
                   unsigned seed = 99)
        : m_min(min), m_max(max)
        , m_rng(seed)
        , m_level_dist(min, max)
        , m_hold_dist(1.0 / hold_ticks_mean)
        , m_remaining(0)
    {
        StepLevel();
    }

    double Next()
    {
        if (m_remaining == 0) StepLevel();
        --m_remaining;
        return m_current;
    }

private:
    void StepLevel()
    {
        m_current   = m_level_dist(m_rng);
        m_remaining = static_cast<unsigned>(m_hold_dist(m_rng)) + 1;
    }

    double   m_min, m_max, m_current;
    unsigned m_remaining;
    std::mt19937                           m_rng;
    std::uniform_real_distribution<double> m_level_dist;
    std::geometric_distribution<unsigned>  m_hold_dist;
};
