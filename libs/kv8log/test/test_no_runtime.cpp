////////////////////////////////////////////////////////////////////////////////
// kv8log/test/test_no_runtime.cpp
//
// Verifies that the kv8log lazy-loading mechanism degrades gracefully when
// the runtime shared library is absent.
//
// Strategy
// --------
//   Set KV8_DISABLE_RUNTIME=1 in the process environment BEFORE the first
//   kv8log macro expansion.  Runtime::InitOnce() detects this and returns
//   without calling dlopen/LoadLibrary, leaving the entire Vtable as
//   all-nullptr.  Every subsequent macro invocation hits the null-pointer
//   fast-path without touching the heap, mutexes, or file system.
//
// Hot-path cost analysis (no runtime, warm path after first call)
// ---------------------------------------------------------------
//   KV8_TEL_ADD:
//     1. call_once(m_reg_once) -- flag already set, single relaxed load.
//     2. if (!m_fn_add) return; -- one null-pointer branch.
//     Net: ~2-5 ns.  Equivalent to a null-pointer check.
//
//   KV8_LOG_INFO("literal"):
//     1. s_kv8_site_.load(relaxed) -- returns sentinel 1 (non-zero).
//     2. Runtime::Log(): Fn() call_once (fast) + if (!fn.log) return.
//     Net: ~3-6 ns.  Equivalent to a null-pointer check.
//
//   KV8_LOGF_INFO("fmt %d", x):
//     Always calls snprintf into a 4096-byte stack buffer, regardless of
//     whether the runtime is present.  The buffer is only dispatched if the
//     runtime exists (it is not); however, snprintf itself is unconditional.
//     Net: snprintf cost (~50-500 ns) + ~3 ns library path.
//     This is NOT equivalent to a null-pointer check -- it is the expected
//     trade-off when KV8_LOG_ENABLE is defined and formatted logging is used.
//     Applications that must avoid snprintf overhead in the no-runtime case
//     should use KV8_LOG_INFO("literal") instead of KV8_LOGF_INFO.
//
// Tests
// -----
//   1. Correctness  -- KV8_TEL_ADD, KV8_LOG_*, KV8_LOGF_* do not crash.
//   2. Cost bound   -- KV8_TEL_ADD and KV8_LOG_INFO complete within 2000 ns
//                      per call on average (rules out mutex, heap, syscall).
//   3. Thread safety -- concurrent first-touch on shared call sites is safe.
//
// No Kafka broker, no external files, no shared libraries required.
////////////////////////////////////////////////////////////////////////////////

#ifndef KV8_LOG_ENABLE
#  define KV8_LOG_ENABLE
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <cstdlib>   // _putenv
#endif

#include "kv8log/KV8_Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void disable_runtime()
{
    // Update both the Win32 environment block and the CRT copy so that
    // GetEnvironmentVariableA and std::getenv both see the value.
#ifdef _WIN32
    SetEnvironmentVariableA("KV8_DISABLE_RUNTIME", "1");
    _putenv("KV8_DISABLE_RUNTIME=1");
#else
    setenv("KV8_DISABLE_RUNTIME", "1", /*overwrite=*/1);
#endif
}

static long long now_ns()
{
    using clk = std::chrono::steady_clock;
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
               clk::now().time_since_epoch()).count();
}

// Per-call budget for macros that must behave like a null-pointer check.
// 2000 ns is generous enough for MSVC Debug builds on slow CI machines while
// still ruling out any mutex acquire, heap allocation, or syscall.
static const long long kBudgetNsPerCall = 2000LL;

#define EXPECT(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1); \
        } \
    } while (0)

// ---------------------------------------------------------------------------
// Test 1: KV8_TEL_ADD cost
// ---------------------------------------------------------------------------

static void test_tel_add()
{
    static const int N = 500000;

    // Declare the counter outside the loop -- the macro creates a
    // function-static Counter that must persist across iterations.
    KV8_TEL(ctr, "test/no_runtime_counter", 0.0, (double)N);

    // Warm-up: one call to fire all call_once paths.
    KV8_TEL_ADD(ctr, 0.0);

    long long t0 = now_ns();
    for (int i = 0; i < N; ++i)
        KV8_TEL_ADD(ctr, (double)i);
    long long elapsed = now_ns() - t0;

    double ns_per = (double)elapsed / N;
    fprintf(stdout, "[tel_add]   %d calls  %lld ns total  %.1f ns/call\n",
            N, elapsed, ns_per);

    // Must stay well under the null-pointer-check budget.
    EXPECT(ns_per < kBudgetNsPerCall);
    fprintf(stdout, "[tel_add]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: KV8_LOG_INFO cost (compile-time string, no snprintf)
// ---------------------------------------------------------------------------

static void test_log_info()
{
    static const int N = 500000;

    // Warm-up: one call to fire registration (returns sentinel 1) and store it.
    KV8_LOG_INFO("no_runtime_warmup");

    long long t0 = now_ns();
    for (int i = 0; i < N; ++i)
        KV8_LOG_INFO("no_runtime_hot");
    long long elapsed = now_ns() - t0;

    double ns_per = (double)elapsed / N;
    fprintf(stdout, "[log_info]  %d calls  %lld ns total  %.1f ns/call\n",
            N, elapsed, ns_per);

    EXPECT(ns_per < kBudgetNsPerCall);
    fprintf(stdout, "[log_info]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: KV8_LOGF_INFO correctness (snprintf always runs -- no timing assert)
// ---------------------------------------------------------------------------

static void test_logf_info()
{
    static const int N = 50000;

    long long t0 = now_ns();
    for (int i = 0; i < N; ++i)
        KV8_LOGF_INFO("no_runtime seq=%d", i);
    long long elapsed = now_ns() - t0;

    double ns_per = (double)elapsed / N;
    // Report only -- snprintf cost is outside the library path and is not
    // subject to the null-pointer-check budget.
    fprintf(stdout, "[logf_info] %d calls  %lld ns total  %.1f ns/call"
                    "  (snprintf overhead expected)\n",
            N, elapsed, ns_per);
    fprintf(stdout, "[logf_info] PASS (correctness only)\n");
}

// ---------------------------------------------------------------------------
// Test 4: all severity levels are crash-free with no runtime
// ---------------------------------------------------------------------------

static void test_all_levels()
{
    KV8_LOG_DEBUG("no_runtime debug");
    KV8_LOG_INFO("no_runtime info");
    KV8_LOG_WARN("no_runtime warn");
    KV8_LOG_ERROR("no_runtime error");
    KV8_LOG_FATAL("no_runtime fatal");

    KV8_LOGF_DEBUG("no_runtime debug %d", 1);
    KV8_LOGF_INFO("no_runtime info %d", 2);
    KV8_LOGF_WARN("no_runtime warn %d", 3);
    KV8_LOGF_ERROR("no_runtime error %d", 4);
    KV8_LOGF_FATAL("no_runtime fatal %d", 5);

    fprintf(stdout, "[all_levels] all 10 severity variants: PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: KV8_TEL_FLUSH and KV8_MONO_TO_NS are no-ops without runtime
// ---------------------------------------------------------------------------

static void test_flush_and_mono()
{
    KV8_TEL_FLUSH();

    // KV8_MONO_TO_NS must return 0 when the runtime is absent.
    uint64_t result = KV8_MONO_TO_NS(12345678ULL);
    EXPECT(result == 0);

    fprintf(stdout, "[flush_mono] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 6: thread safety -- concurrent first-touch on shared call sites
// ---------------------------------------------------------------------------

static std::atomic<int> g_ready{0};

static void thread_worker()
{
    while (!g_ready.load(std::memory_order_acquire)) {}

    // All threads share this one function-static Counter; call_once inside
    // the Counter and the Channel must tolerate concurrent first calls.
    KV8_TEL(thread_ctr, "test/thread_counter", 0.0, 10000.0);

    for (int i = 0; i < 5000; ++i) {
        KV8_TEL_ADD(thread_ctr, (double)i);
        KV8_LOG_DEBUG("thread_tick");
    }
}

static void test_thread_safety()
{
    static const int kThreads = 8;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(thread_worker);

    g_ready.store(1, std::memory_order_release);
    for (auto& t : threads) t.join();

    fprintf(stdout, "[threads]   %d threads x 5000 calls each: PASS\n", kThreads);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    // Must be called before ANY kv8log macro is expanded in this process.
    disable_runtime();

    fprintf(stdout, "=== test_no_runtime (KV8_DISABLE_RUNTIME=1) ===\n");

    test_tel_add();
    test_log_info();
    test_logf_info();
    test_all_levels();
    test_flush_and_mono();
    test_thread_safety();

    fprintf(stdout, "=== ALL PASS ===\n");
    return 0;
}
