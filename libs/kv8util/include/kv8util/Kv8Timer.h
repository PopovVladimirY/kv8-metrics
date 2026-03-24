////////////////////////////////////////////////////////////////////////////////
// kv8util/Kv8Timer.h -- cross-platform high-resolution timer abstraction
//
// Provides nanosecond-resolution timing on both Windows (QPC) and POSIX
// (CLOCK_MONOTONIC).  All functions are static inline for zero-overhead
// inlining on the hot path.
//
// Usage:
//     kv8util::TimerInit();             // call once at startup
//     uint64_t t0 = kv8util::TimerNow();
//     // ... work ...
//     uint64_t t1 = kv8util::TimerNow();
//     double ns = kv8util::TicksToNs(t1 - t0);
//     int64_t wallMs = kv8util::QpcToWallMs(t0);
//
// Thread safety: TimerInit() must be called once before any other function.
// After initialisation all functions are safe to call from any thread.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#endif

#include <chrono>
#include <cstdint>

#ifndef _WIN32
#  include <ctime>
#endif

namespace kv8util {

////////////////////////////////////////////////////////////////////////////////
// Module-level calibration state (internal -- set by TimerInit)
////////////////////////////////////////////////////////////////////////////////

namespace detail {

#ifdef _WIN32
inline uint64_t g_qpcFreq    = 0; // ticks per second
inline uint64_t g_qpcBase    = 0; // QPC tick captured at TimerInit()
inline int64_t  g_wallMsBase = 0; // wall-clock ms captured at TimerInit()
#else
inline uint64_t g_qpcBase    = 0;
inline int64_t  g_wallMsBase = 0;
#endif

} // namespace detail

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

/// Calibrate the timer subsystem.  Must be called once before any other
/// timer function.  Captures the QPC-to-wall-clock offset so that
/// QpcToWallMs() can convert without a syscall per message.
static inline void TimerInit()
{
#ifdef _WIN32
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    detail::g_qpcFreq = (uint64_t)li.QuadPart;

    detail::g_wallMsBase = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
    QueryPerformanceCounter(&li);
    detail::g_qpcBase = (uint64_t)li.QuadPart;
#else
    detail::g_wallMsBase = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    detail::g_qpcBase = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/// Return the current high-resolution tick.
/// Windows: QPC counter value.  POSIX: CLOCK_MONOTONIC nanoseconds.
static inline uint64_t TimerNow()
{
#ifdef _WIN32
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (uint64_t)li.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/// Convert a tick delta to nanoseconds.
static inline double TicksToNs(uint64_t ticks)
{
#ifdef _WIN32
    return (double)ticks * 1e9 / (double)detail::g_qpcFreq;
#else
    return (double)ticks; // already nanoseconds
#endif
}

/// Convert an absolute QPC tick to wall-clock milliseconds (Unix epoch).
/// Uses the startup calibration -- pure integer arithmetic, no syscall.
static inline int64_t QpcToWallMs(uint64_t tick)
{
#ifdef _WIN32
    return detail::g_wallMsBase
         + (int64_t)((tick - detail::g_qpcBase) * 1000ULL / detail::g_qpcFreq);
#else
    return detail::g_wallMsBase
         + (int64_t)((tick - detail::g_qpcBase) / 1000000ULL);
#endif
}

/// Milliseconds since Unix epoch -- same timescale as Kafka broker timestamps.
/// Used for Producer->Broker and Broker->Consumer latency splits.
static inline int64_t WallMs()
{
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace kv8util
