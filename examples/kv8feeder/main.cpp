////////////////////////////////////////////////////////////////////////////////
// kv8log/examples/kv8feeder/main.cpp
//
// kv8feeder -- continuous live Telemetry producer for demo and pipeline tests.
//
// Starts three threads, each publishing one counter to Kafka via kv8log macros.
//
//   Numbers/Counter  (100 Hz,     0..1023)  -- wrapping integer
//   Numbers/Wald    (10000 Hz, -4000..4000) -- Gaussian random walk
//   Scope/Phases   (100000 Hz,   -5..5)     -- piecewise-constant cyclogram
//
// All rates are overridable via --rate.counter=, --rate.wald=, --rate.phases=.
// Connection parameters /KV8.* are picked up automatically from argv or env.
//
// Timing notes:
//   Counter @ 100 Hz (10 ms interval): sleeps + 200 us spin -- near-zero CPU.
//   Wald  @ 10kHz (100 us interval): pure spin -- one full CPU core.
//   Phases @ 100kHz (10 us nominal, random): pure spin -- one full CPU core.
//
// Stop with Ctrl-C.
////////////////////////////////////////////////////////////////////////////////

#ifndef KV8_LOG_ENABLE
#  define KV8_LOG_ENABLE
#endif
#include "kv8log/KV8_Log.h"

#include "Generators.h"
#include "UdtFeeds.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <intrin.h>   // _mm_pause
#endif

// ── Stop flag ────────────────────────────────────────────────────────────────
static std::atomic<bool> g_stop{false};

#ifdef _WIN32
static BOOL WINAPI CtrlHandler(DWORD) { g_stop.store(true); return TRUE; }
#else
static void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }
#endif

// ── Precision wait ───────────────────────────────────────────────────────────
// Sleeps to within 200 µs of the deadline then spin-loops precisely.
// For intervals <= 200 µs the entire wait is a spin-loop.
// This eliminates the ~1 ms OS timer quantisation that causes bursting at
// high sample rates (Wald @ 10 kHz, Phases @ 100 kHz).
static void tight_wait(std::chrono::steady_clock::time_point deadline)
{
    using Clock = std::chrono::steady_clock;
    using us    = std::chrono::microseconds;
    constexpr us kSpinMargin{200};
    auto sleep_tp = deadline - kSpinMargin;
    if (Clock::now() < sleep_tp)
        std::this_thread::sleep_until(sleep_tp);
    while (Clock::now() < deadline)
    {
#if defined(_MSC_VER)
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause" ::: "memory");
#endif
    }
}

// ── Thread: Numbers/Counter ──────────────────────────────────────────────────
// Uniform emission at the configured Hz rate.
// Waits first, then records -- the KV8_TEL_ADD timestamp is taken at the
// correct nominal time rather than at an arbitrary pre-sleep moment.
static void run_counter_feed(int hz, const std::atomic<bool>& stop)
{
    KV8_TEL(counter_feed, "Numbers/Counter", 0.0, 1023.0);
    WrappingCounter gen(1023.0);

    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::nanoseconds(1000000000LL / hz);
    auto next = Clock::now();

    while (!stop.load(std::memory_order_relaxed)) {
        next += interval;
        tight_wait(next);
        KV8_TEL_ADD(counter_feed, gen.Next());
    }
}

// ── Thread: Numbers/Wald ────────────────────────────────────────────────────
// Uniform emission at the configured Hz rate (spin-loop at 10 kHz default).
static void run_wald_feed(int hz, const std::atomic<bool>& stop)
{
    KV8_TEL(wald_feed, "Numbers/Wald", -4000.0, 4000.0);
    GaussWalk gen(-4000.0, 4000.0, 20.0);

    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::nanoseconds(1000000000LL / hz);
    auto next = Clock::now();

    while (!stop.load(std::memory_order_relaxed)) {
        next += interval;
        tight_wait(next);
        KV8_TEL_ADD(wald_feed, gen.Next());
    }
}

// ── Thread: Scope/Phases ────────────────────────────────────────────────────
// Each sample is emitted after a randomly varying wait drawn from a truncated
// exponential distribution with mean = nominal_ns, range [nominal/10, nominal*10].
//
// Why truncated exponential:
//   - Mean == nominal_ns  =>  long-run average rate == configured Hz.
//   - 99.9 % of samples fall within [0.1x, 10x] of the nominal interval.
//   - No clustering (no "batch" effect): each sample is individually timed.
//
// The value itself follows a piecewise-constant cyclogram (PhaseCyclogram).
// hold_ticks_mean is calibrated so each phase lasts ~10 ms on average.
static void run_phases_feed(int hz, const std::atomic<bool>& stop)
{
    KV8_TEL(phases_feed, "Scope/Phases", -5.0, 5.0);

    const uint64_t nominal_ns = 1000000000ULL / (uint64_t)hz;
    const uint64_t min_ns     = nominal_ns / 10;   // 0.1 x nominal
    const uint64_t max_ns     = nominal_ns * 10;   // 10 x nominal

    // Each phase holds for ~10 ms on average.
    // With mean tick duration = nominal_ns, hold_ticks = 10 ms / nominal_ns.
    const double hold_ticks_mean = 10000000.0 / (double)nominal_ns;
    PhaseCyclogram gen(-5.0, 5.0, hold_ticks_mean);

    // Truncated exponential: mean = nominal_ns, clamped to [min_ns, max_ns].
    std::mt19937_64 rng(std::random_device{}());
    const double lambda = 1.0 / (double)nominal_ns;
    std::exponential_distribution<double> exp_dist(lambda);

    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();

    while (!stop.load(std::memory_order_relaxed)) {
        const uint64_t raw_ns  = static_cast<uint64_t>(exp_dist(rng));
        const uint64_t wait_ns = (raw_ns < min_ns) ? min_ns
                               : (raw_ns > max_ns) ? max_ns
                               : raw_ns;
        next += std::chrono::nanoseconds(wait_ns);
        tight_wait(next);
        KV8_TEL_ADD(phases_feed, gen.Next());
    }
}

// ── main ────────────────────────────────────────────────────────────────────
int main(int argc, char** argv)
{
    int  hz_counter = 100;
    int  hz_wald    = 10000;
    int  hz_phases  = 100000;
    int  duration   = 0;       // 0 = run forever
    bool bUdt       = false;

    for (int i = 1; i < argc; ++i) {
        // kv8log auto-reads /KV8.* args; we only parse our own flags here.
        if (std::sscanf(argv[i], "--rate.counter=%d", &hz_counter) == 1) continue;
        if (std::sscanf(argv[i], "--rate.wald=%d",    &hz_wald)    == 1) continue;
        if (std::sscanf(argv[i], "--rate.phases=%d",  &hz_phases)  == 1) continue;
        if (std::sscanf(argv[i], "--duration=%d",     &duration)   == 1) continue;
        if (strcmp(argv[i], "--udt") == 0) { bUdt = true; continue; }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stdout,
                "kv8feeder -- continuous live Telemetry producer\n"
                "\n"
                "Connection (auto-detected from argv/env, all optional):\n"
                "  /KV8.brokers=HOST:PORT    (default: localhost:19092)\n"
                "  /KV8.channel=NAME         (default: kv8log/kv8feeder)\n"
                "  /KV8.user=USER            (default: kv8producer)\n"
                "  /KV8.pass=PASS            (default: kv8secret)\n"
                "\n"
                "Sample rates:\n"
                "  --rate.counter=HZ         Numbers/Counter   (default: 100)\n"
                "  --rate.wald=HZ            Numbers/Wald      (default: 10000)\n"
                "  --rate.phases=HZ          Scope/Phases      (default: 100000)\n"
                "\n"
                "UDT feeds:\n"
                "  --udt                     Start UDT example feeds:\n"
                "                              Environment/WeatherStation @ 1 Hz\n"
                "                              Aerial/Platform           @ 1000 Hz\n"
                "\n"
                "Control:\n"
                "  --duration=SECONDS        Stop after N seconds (0 = forever)\n"
                "\n"
                "CPU note: Wald (10 kHz) and Phases (100 kHz) use spin-loops for\n"
                "precise timing and will consume one full CPU core each.\n"
                "\n"
                "Build without -DKV8_LOG_ENABLE to produce a fully silent binary.\n");
            return 0;
        }
    }

    // Clamp rates to a sane range.
    if (hz_counter < 1)  hz_counter = 1;
    if (hz_wald    < 1)  hz_wald    = 1;
    if (hz_phases  < 1)  hz_phases  = 1;

#ifdef _WIN32
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
#else
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
#endif

    fprintf(stdout,
        "[kv8feeder] starting\n"
        "[kv8feeder]   Numbers/Counter  @ %d Hz  (uniform, sleep+spin)\n"
        "[kv8feeder]   Numbers/Wald     @ %d Hz  (uniform, spin)\n"
        "[kv8feeder]   Scope/Phases     @ %d Hz  (exponential random, spin)\n"
        "[kv8feeder]   duration         : %s\n",
        hz_counter, hz_wald, hz_phases,
        duration ? "finite" : "until Ctrl-C");
    if (bUdt)
        fprintf(stdout,
            "[kv8feeder]   WeatherStation   @ 1 Hz    (UDT, sleep+spin)\n"
            "[kv8feeder]   Aerial/Navigation + Aerial/Attitude + Aerial/Motors  @ 1000 Hz (UDT+TS, spin)\n");

    std::thread t1(run_counter_feed, hz_counter, std::cref(g_stop));
    std::thread t2(run_wald_feed,    hz_wald,    std::cref(g_stop));
    std::thread t3(run_phases_feed,  hz_phases,  std::cref(g_stop));
    std::thread t4, t5;
    if (bUdt) {
        t4 = std::thread(RunWeatherStation, std::cref(g_stop));
        t5 = std::thread(RunAerialPlatform, std::cref(g_stop));
    }

    if (duration > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(duration));
        g_stop.store(true, std::memory_order_relaxed);
    } else {
        while (!g_stop.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    t1.join();
    t2.join();
    t3.join();
    if (t4.joinable()) t4.join();
    if (t5.joinable()) t5.join();

    fprintf(stdout, "[kv8feeder] flushing ...\n");
    KV8_TEL_FLUSH();
    fprintf(stdout, "[kv8feeder] done.\n");
    return 0;
}

