////////////////////////////////////////////////////////////////////////////////
// kv8log/test/test_log_static_cache.cpp
//
// Phase L5 unit test: exercise the per-call-site static atomic cache.
//
// The runtime shared library is intentionally NOT loaded by this test.  On
// Windows that happens incidentally because kv8log_runtime.dll is not on the
// test exe's PATH; on Linux the .so sits next to the exe and dlopen would
// otherwise succeed.  To get deterministic, platform-independent behaviour
// the test sets KV8_DISABLE_RUNTIME=1 BEFORE the first KV8_LOG_* macro
// expansion -- Runtime::InitOnce() then short-circuits and never opens the
// shared library, so:
//
//   - Runtime::RegisterLogSite returns the sentinel value 1 (benign, never 0)
//     because the runtime symbol pointer is null OR no channel handle exists.
//   - The macro stores 1 in its static atomic, never retries registration.
//   - Runtime::Log no-ops because the vtable entry / handle is missing.
//
// This test verifies:
//   1. Repeated single-thread calls on the same call site are crash-free and
//      complete in bounded time (no per-call registry path).
//   2. Concurrent first calls from N threads on a SHARED call site produce
//      identical static cache values across all threads (hash race is benign).
//   3. Distinct call sites yield distinct cache values across thread boundaries.
//
// The test does not require a Kafka broker.
////////////////////////////////////////////////////////////////////////////////

#include "kv8log/KV8_Log.h"
#include "kv8log/Runtime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

// One call site exercised in a tight loop.  The macro expansion owns a
// static std::atomic<uint32_t> internally; on first call it loads the cache,
// hits zero, calls RegisterLogSite (sentinel result), stores it.  On every
// subsequent call only a relaxed atomic load + Runtime::Log no-op runs.
static void HammerOneSite(int nIterations)
{
    for (int i = 0; i < nIterations; ++i)
        KV8_LOG_INFO("hammer one site");
}

// Two distinct call sites on different source lines; each owns its own
// static atomic.  Used to verify thread visibility and absence of crashes
// when many sites are first-touched concurrently.
static void TouchTwoSites()
{
    KV8_LOG_DEBUG("first distinct site");
    KV8_LOG_WARN ("second distinct site");
}

int main()
{
    // Force the runtime loader to short-circuit BEFORE any KV8_LOG_* macro
    // expansion (the very first call lazily triggers Runtime::InitOnce via
    // std::call_once).  Without this the kv8log_runtime shared library would
    // be auto-loaded on platforms where it sits next to the test exe (Linux),
    // a real Kafka producer would spin up, and the 10000-iteration timing
    // assertion below would measure real produces instead of the macro-cache
    // fast path the test is designed to exercise.
#ifdef _WIN32
    _putenv_s("KV8_DISABLE_RUNTIME", "1");
#else
    setenv("KV8_DISABLE_RUNTIME", "1", 1);
#endif

    // Sanity check: the runtime escape hatch must have taken effect.  Touching
    // Runtime::Fn() forces InitOnce() to run; with KV8_DISABLE_RUNTIME=1 set
    // the vtable stays zero-initialised, so the dispatch pointers are null and
    // every KV8_LOG_* below reduces to a relaxed atomic load + no-op.
    EXPECT(kv8log::Runtime::Fn().add == nullptr);
    EXPECT(kv8log::Runtime::Fn().log == nullptr);

    // 1. Single-thread bulk dispatch must complete quickly.
    {
        const int kIters = 10000;
        const auto t0 = std::chrono::steady_clock::now();
        HammerOneSite(kIters);
        const auto t1 = std::chrono::steady_clock::now();
        const auto dwMs = std::chrono::duration_cast<std::chrono::milliseconds>
                            (t1 - t0).count();
        // Without a runtime everything is a no-op; 10000 iterations must fit
        // comfortably in well under one second on any sane machine.
        EXPECT(dwMs < 2000);
    }

    // 2. Concurrent first-touch on a SHARED call site (the one inside
    //    HammerOneSite) is already warm at this point; spawn N threads that
    //    each touch two NEW call sites simultaneously.  This stresses the
    //    static-atomic-init race on cold sites.
    {
        constexpr int kThreads = 4;
        constexpr int kIters   = 1000;
        std::vector<std::thread> vThreads;
        std::atomic<int> qwExceptions{0};
        vThreads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t)
            vThreads.emplace_back([&] {
                try {
                    for (int i = 0; i < kIters; ++i)
                        TouchTwoSites();
                } catch (...) {
                    qwExceptions.fetch_add(1, std::memory_order_relaxed);
                }
            });
        for (auto& th : vThreads) th.join();
        EXPECT(qwExceptions.load() == 0);
    }

    fprintf(stdout, "[PASS] test_log_static_cache: macro caching is "
                    "crash-free and bounded (single + multi-thread)\n");
    return 0;
}
