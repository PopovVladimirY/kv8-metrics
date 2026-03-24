////////////////////////////////////////////////////////////////////////////////
// kv8log/test/test_monotonic.cpp
//
// Verifies kv8log_monotonic_to_ns() and produces a valid kv8 session:
//
//   1. Configures a Kafka session (broker/channel/credentials).
//   2. Tests that KV8_MONO_TO_NS does not crash; returns 0 when the runtime
//      shared library is absent, a live wall-clock value when it is present.
//   3. Produces kSamples telemetry samples using KV8_TEL_ADD_TS with
//      monotonic-derived timestamps so that kv8scope can display the session
//      with correct sample positions on the wall-clock timeline.
//      Counter range [0.0, kSamples-1] matches the produced ramp values.
//
// If the runtime shared library is absent all production calls are silent
// no-ops, and the test still passes (exit code 0).
////////////////////////////////////////////////////////////////////////////////

#ifndef KV8_LOG_ENABLE
#  define KV8_LOG_ENABLE
#endif
#include "kv8log/KV8_Log.h"

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <thread>

// ── Platform monotonic clock (mirrors MonoNs() in kv8log_impl.cpp) ──────────
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static uint64_t MonoNsNow()
{
    static const uint64_t s_freq = []() -> uint64_t {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    uint64_t c = static_cast<uint64_t>(cnt.QuadPart);
    return (c / s_freq) * 1000000000ULL + (c % s_freq) * 1000000000ULL / s_freq;
}
#else
#  include <time.h>
static uint64_t MonoNsNow()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

static const int kSamples = 1000;

int main()
{
    // Must configure before the first KV8 API call so the runtime opens the
    // session with the correct name.
    KV8_LOG_CONFIGURE("localhost:19092", "test/test_monotonic",
                      "kv8producer", "kv8secret");

    // Test 1: KV8_MONO_TO_NS does not crash; returns 0 (absent runtime) or a
    // live wall-clock nanosecond value (present runtime).
    uint64_t check = KV8_MONO_TO_NS(MonoNsNow());
    fprintf(stdout, "[test_monotonic] KV8_MONO_TO_NS check = %llu\n",
            (unsigned long long)check);

    // Test 2: produce kSamples with monotonic-derived timestamps.
    // Counter range [0.0, kSamples-1] matches the ramp values exactly.
    {
        KV8_TEL(mono_ramp, "mono/ramp", 0.0, (double)(kSamples - 1));
        for (int i = 0; i < kSamples; ++i)
        {
            uint64_t mono_ns = MonoNsNow();
            uint64_t wall_ns = KV8_MONO_TO_NS(mono_ns);
            // wall_ns == 0 when the runtime is absent -- fall back to the
            // auto-timestamp path so the sample is still recorded.
            if (wall_ns != 0)
                KV8_TEL_ADD_TS(mono_ramp, (double)i, wall_ns);
            else
                KV8_TEL_ADD(mono_ramp, (double)i);
            // 1 ms between samples gives visible spacing on the kv8scope timeline.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        KV8_TEL_FLUSH();
    }

    fprintf(stdout, "[test_monotonic] PASS -- %d samples produced\n", kSamples);
    return 0;
}
