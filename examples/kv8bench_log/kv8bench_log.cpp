////////////////////////////////////////////////////////////////////////////////
// kv8bench_log -- LOG hot-path throughput benchmark.
//
// Measures dispatch latency and throughput for KV8_LOGF_INFO from N producer
// threads over a fixed duration (or message count).  Mirrors kv8bench in
// CLI shape, per-second progress reporting and final report file layout.
//
// Usage:
//   kv8bench_log [options]
//
//   --brokers  <b>     Bootstrap brokers           [localhost:19092]
//   --duration <s>     Produce for this many seconds [10]
//   --count    <N>     Produce exactly N messages; overrides --duration
//   --threads  <N>     Producer thread count        [1]
//   --payload  <bytes> Log message payload size     [64]
//   --linger   <ms>    rdkafka linger.ms note       [5]
//   --batch    <n>     rdkafka batch.num.messages note [10000]
//   --report   <path>  Output report file
//   --user / --pass    SASL credentials
//   --no-cleanup       Keep topics after run
//   --help
////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#endif

#include <kv8log/KV8_Log.h>
#include <kv8/IKv8Consumer.h>

#include <kv8util/Kv8Timer.h>
#include <kv8util/Kv8Stats.h>
#include <kv8util/Kv8AppUtils.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

using namespace kv8util;

////////////////////////////////////////////////////////////////////////////////
// Hot-path emitter -- one template instantiation per thread index, so each
// thread owns its own static atomic site cache (no first-call contention).
////////////////////////////////////////////////////////////////////////////////

static constexpr int kMaxThreads = 32;

template <int Idx>
static void HotPathLoop(uint64_t                   nIters,
                        double                     dDurationSec,
                        const char                *sPayload,
                        int                        nSampleRate,
                        std::vector<double>       &vLatNs,   // per-thread, sampled
                        std::atomic<uint64_t>     &qwTotalCalls,
                        const std::atomic<bool>   &bStop)
{
    // Warm-up to ensure the static atomic is registered.
    for (int i = 0; i < 64; ++i)
        KV8_LOGF_INFO("kv8bench_log warmup t=%d i=%d s=%s", Idx, i, sPayload);

    constexpr uint64_t kReportBatch = 1024;
    uint64_t nLocal = 0;
    uint64_t nLastReport = 0;
    auto report = [&]() {
        const uint64_t nDelta = nLocal - nLastReport;
        if (nDelta) {
            qwTotalCalls.fetch_add(nDelta, std::memory_order_relaxed);
            nLastReport = nLocal;
        }
    };
    if (nIters)
    {
        for (uint64_t i = 0; i < nIters && !bStop.load(std::memory_order_relaxed); ++i)
        {
            const bool bSample = (nSampleRate > 0) && ((i % nSampleRate) == 0);
            if (bSample)
            {
                const uint64_t qwT0 = TimerNow();
                KV8_LOGF_INFO("kv8bench_log t=%d i=%llu s=%s",
                              Idx, (unsigned long long)i, sPayload);
                const uint64_t qwT1 = TimerNow();
                vLatNs.push_back(TicksToNs(qwT1 - qwT0));
            }
            else
            {
                KV8_LOGF_INFO("kv8bench_log t=%d i=%llu s=%s",
                              Idx, (unsigned long long)i, sPayload);
            }
            ++nLocal;
            if ((nLocal & (kReportBatch - 1)) == 0) report();
        }
    }
    else
    {
        const auto tStart   = std::chrono::steady_clock::now();
        const auto tDeadline = tStart +
            std::chrono::microseconds(static_cast<int64_t>(dDurationSec * 1e6));
        uint64_t i = 0;
        while (!bStop.load(std::memory_order_relaxed))
        {
            if ((i & 0xFFF) == 0 && std::chrono::steady_clock::now() >= tDeadline)
                break;
            const bool bSample = (nSampleRate > 0) && ((i % nSampleRate) == 0);
            if (bSample)
            {
                const uint64_t qwT0 = TimerNow();
                KV8_LOGF_INFO("kv8bench_log t=%d i=%llu s=%s",
                              Idx, (unsigned long long)i, sPayload);
                const uint64_t qwT1 = TimerNow();
                vLatNs.push_back(TicksToNs(qwT1 - qwT0));
            }
            else
            {
                KV8_LOGF_INFO("kv8bench_log t=%d i=%llu s=%s",
                              Idx, (unsigned long long)i, sPayload);
            }
            ++nLocal;
            ++i;
            if ((nLocal & (kReportBatch - 1)) == 0) report();
        }
    }
    report();
}

using ThreadFn = void (*)(uint64_t,
                          double,
                          const char *,
                          int,
                          std::vector<double> &,
                          std::atomic<uint64_t> &,
                          const std::atomic<bool> &);

template <int N>
struct ThreadFnTable
{
    static void Fill(ThreadFn *p)
    {
        ThreadFnTable<N - 1>::Fill(p);
        p[N - 1] = &HotPathLoop<N - 1>;
    }
};
template <>
struct ThreadFnTable<0>
{
    static void Fill(ThreadFn *) {}
};

////////////////////////////////////////////////////////////////////////////////
// Helpers
////////////////////////////////////////////////////////////////////////////////

static std::string NowUtcStamp()
{
    char buf[32];
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    std::snprintf(buf, sizeof(buf), "%04u%02u%02uT%02u%02u%02uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
#else
    time_t t = time(nullptr);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    std::snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
#endif
    return buf;
}

static std::string DefaultChannelName()
{
    // Mirrors Runtime/DefaultChannel: exe basename with '/' -> '.'.
    return "kv8log.kv8bench_log";
}

////////////////////////////////////////////////////////////////////////////////
// main
////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    TimerInit();

    // ── Parse arguments ──────────────────────────────────────────────────────
    std::string sBrokers     = "localhost:19092";
    std::string sUser        = "kv8producer";
    std::string sPass        = "kv8secret";
    std::string sReport;
    double      dDurationSec = 10.0;
    uint64_t    nCountArg    = 0;       // 0 = duration mode
    int         nThreads     = 1;
    int         nPayload     = 64;
    int         nLingerMs    = 5;
    int         nBatch       = 10000;
    int         nSampleRate  = 100;
    bool        bCleanup     = true;

    for (int i = 1; i < argc; ++i)
    {
        auto match = [&](const char *f) { return std::strcmp(argv[i], f) == 0; };
        auto next  = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : nullptr; };

        if      (match("--brokers"))     { auto v = next(); if (v) sBrokers     = v; }
        else if (match("--duration"))    { auto v = next(); if (v) dDurationSec = atof(v); }
        else if (match("--count"))       { auto v = next(); if (v) nCountArg    = (uint64_t)strtoull(v, nullptr, 10); }
        else if (match("--threads"))     { auto v = next(); if (v) nThreads     = atoi(v); }
        else if (match("--payload"))     { auto v = next(); if (v) nPayload     = atoi(v); }
        else if (match("--linger"))      { auto v = next(); if (v) nLingerMs    = atoi(v); }
        else if (match("--batch"))       { auto v = next(); if (v) nBatch       = atoi(v); }
        else if (match("--sample-rate")) { auto v = next(); if (v) nSampleRate  = atoi(v); }
        else if (match("--report"))      { auto v = next(); if (v) sReport      = v; }
        else if (match("--user"))        { auto v = next(); if (v) sUser        = v; }
        else if (match("--pass"))        { auto v = next(); if (v) sPass        = v; }
        else if (match("--no-cleanup"))  { bCleanup = false; }
        else if (match("--help"))
        {
            std::fprintf(stdout,
                "kv8bench_log -- LOG hot-path throughput benchmark\n\n"
                "Usage: kv8bench_log [options]\n\n"
                "  --brokers  <b>     Bootstrap brokers          [localhost:19092]\n"
                "  --duration <s>     Produce for N seconds       [10]\n"
                "  --count    <N>     Produce exactly N msgs; overrides --duration\n"
                "  --threads  <N>     Producer thread count       [1]\n"
                "  --payload  <bytes> Log message payload size    [64]\n"
                "  --linger   <ms>    rdkafka linger.ms note      [5]\n"
                "  --batch    <n>     rdkafka batch.num.messages  [10000]\n"
                "  --sample-rate <n>  1-in-N msgs sampled         [100]\n"
                "  --report   <path>  Report file                 [kv8bench_log_<ts>.txt]\n"
                "  --user / --pass    SASL credentials\n"
                "  --no-cleanup       Keep topics after run\n");
            return 0;
        }
        else
        {
            std::fprintf(stderr, "[BENCH] unknown arg: %s\n", argv[i]);
            return 1;
        }
    }

    if (nThreads < 1 || nThreads > kMaxThreads)
    {
        std::fprintf(stderr, "[BENCH] --threads must be in [1..%d]\n", kMaxThreads);
        return 1;
    }
    if (nPayload < 1 || nPayload > 3500)
    {
        std::fprintf(stderr, "[BENCH] --payload must be in [1..3500]\n");
        return 1;
    }
    if (nSampleRate < 1) nSampleRate = 1;

    if (sReport.empty())
        sReport = "kv8bench_log_" + NowUtcStamp() + ".txt";

    // Build payload string.
    std::string sPayload(nPayload, 'x');

    // ── Pre-clean previous run's channel ─────────────────────────────────────
    const std::string sChannel = DefaultChannelName();
    {
        try
        {
            auto kCfg = BuildKv8Config(sBrokers, "sasl_plaintext", "PLAIN",
                                       sUser, sPass, "");
            auto cleaner = kv8::IKv8Consumer::Create(kCfg);
            cleaner->DeleteChannel(sChannel);
        }
        catch (...) {}
    }

    // ── Configure kv8log (must precede first emission) ───────────────────────
    KV8_LOG_CONFIGURE(sBrokers.c_str(), sChannel.c_str(),
                      sUser.c_str(), sPass.c_str());

    // ── Print banner ─────────────────────────────────────────────────────────
    std::printf("kv8bench_log -- LOG hot-path throughput benchmark\n");
    std::printf("  brokers : %s\n", sBrokers.c_str());
    std::printf("  channel : %s\n", sChannel.c_str());
    std::printf("  threads : %d\n", nThreads);
    std::printf("  payload : %d bytes\n", nPayload);
    if (nCountArg)
        std::printf("  count   : %llu msgs\n", (unsigned long long)nCountArg);
    else
        std::printf("  duration: %.3f s\n", dDurationSec);
    std::printf("  sample  : 1-in-%d for latency\n\n", nSampleRate);

    // ── Launch threads ───────────────────────────────────────────────────────
    ThreadFn fnTable[kMaxThreads] = {};
    ThreadFnTable<kMaxThreads>::Fill(fnTable);

    std::vector<std::vector<double>> vLatPerThread(nThreads);
    for (auto &v : vLatPerThread) v.reserve(1 << 16);

    std::atomic<uint64_t> qwTotalCalls{0};
    std::atomic<bool>     bStop{false};

    const uint64_t nItersPerThread =
        nCountArg ? (nCountArg / static_cast<uint64_t>(nThreads)) : 0;

    std::vector<std::thread> vThr;
    vThr.reserve(nThreads);

    const auto tStart = std::chrono::steady_clock::now();

    for (int t = 0; t < nThreads; ++t)
    {
        vThr.emplace_back(fnTable[t],
                          nItersPerThread,
                          dDurationSec,
                          sPayload.c_str(),
                          nSampleRate,
                          std::ref(vLatPerThread[t]),
                          std::ref(qwTotalCalls),
                          std::cref(bStop));
    }

    // ── Per-second progress rows ─────────────────────────────────────────────
    std::printf("  sec     calls/s        total_calls\n");
    uint64_t qwLastTotal = 0;
    int      nSecondsElapsed = 0;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        ++nSecondsElapsed;
        const uint64_t qwNow = qwTotalCalls.load(std::memory_order_relaxed);
        const uint64_t qwDelta = qwNow - qwLastTotal;
        std::printf("  %3d  %10llu  %18llu\n",
                    nSecondsElapsed,
                    (unsigned long long)qwDelta,
                    (unsigned long long)qwNow);
        qwLastTotal = qwNow;

        // Termination conditions.
        bool bAllDone = true;
        for (auto &th : vThr) { if (th.joinable() ? false : true) { bAllDone = false; break; } }
        // We can't probe joinable to detect "still running" -- threads are
        // joinable until joined.  Instead break when total calls plateau and
        // we have either passed the duration or count budget.
        if (nCountArg)
        {
            if (qwNow >= nCountArg) break;
        }
        else
        {
            if (nSecondsElapsed >= static_cast<int>(dDurationSec) + 2) break;
            if (nSecondsElapsed >= static_cast<int>(dDurationSec) && qwDelta == 0) break;
        }
    }

    bStop.store(true, std::memory_order_relaxed);
    for (auto &th : vThr) th.join();
    const auto tEnd = std::chrono::steady_clock::now();

    const double dElapsed = std::chrono::duration<double>(tEnd - tStart).count();

    // ── Flush kv8log so producer queue is drained ───────────────────────────
    std::printf("\n[BENCH] flushing kv8log...\n");
    KV8_TEL_FLUSH();

    // ── Aggregate latency ────────────────────────────────────────────────────
    std::vector<double> vAllLat;
    for (auto &v : vLatPerThread) vAllLat.insert(vAllLat.end(), v.begin(), v.end());
    Stats statLat = ComputeStats(vAllLat);

    const uint64_t qwTotal = qwTotalCalls.load();
    const double   dThru   = (dElapsed > 0.0) ? (qwTotal / dElapsed) : 0.0;

    // ── Acceptance thresholds (per LOG_PHASES.md L6.1) ──────────────────────
    const double dP50Limit  = 200.0;
    const double dP99Limit  = 1000.0;
    const double dThruLimit = 1.0e6;
    const bool   bPassP50  = statLat.dP50 < dP50Limit;
    const bool   bPassP99  = statLat.dP99 < dP99Limit;
    const bool   bPassThru = dThru > dThruLimit;
    const bool   bAllPass  = bPassP50 && bPassP99 && bPassThru;

    // ── Console summary ──────────────────────────────────────────────────────
    std::printf("\n=== Final summary ===\n");
    std::printf("Total calls:         %llu\n", (unsigned long long)qwTotal);
    std::printf("Duration (s):        %.3f\n", dElapsed);
    std::printf("Throughput (calls/s):%.0f\n", dThru);
    std::printf("Dispatch latency (sampled, n=%zu):\n", vAllLat.size());
    std::printf("  min:   %.0f ns\n",  statLat.dMin);
    std::printf("  p50:   %.0f ns\n",  statLat.dP50);
    std::printf("  p95:   %.0f ns\n",  statLat.dP95);
    std::printf("  p99:   %.0f ns\n",  statLat.dP99);
    std::printf("  p999:  %.0f ns\n",  statLat.dP999);
    std::printf("  max:   %.0f ns\n",  statLat.dMax);

    std::printf("\n=== Acceptance thresholds ===\n");
    std::printf("  p50 < %.0f ns:       %s  (%.0f ns)\n", dP50Limit,
                bPassP50  ? "PASS" : "FAIL", statLat.dP50);
    std::printf("  p99 < %.0f ns:      %s  (%.0f ns)\n", dP99Limit,
                bPassP99  ? "PASS" : "FAIL", statLat.dP99);
    std::printf("  throughput > %.1fM/s: %s  (%.2fM/s on %d threads)\n",
                dThruLimit / 1.0e6,
                bPassThru ? "PASS" : "FAIL",
                dThru / 1.0e6, nThreads);

    // ── Report file ──────────────────────────────────────────────────────────
    if (FILE *f = std::fopen(sReport.c_str(), "w"))
    {
        std::fprintf(f, "kv8bench_log -- LOG hot-path throughput benchmark\n");
        std::fprintf(f, "Date:     %s\n", NowUtcStamp().c_str());
        std::fprintf(f, "Brokers:  %s\n", sBrokers.c_str());
        std::fprintf(f, "Channel:  %s\n", sChannel.c_str());
        std::fprintf(f, "Threads:  %d\n", nThreads);
        std::fprintf(f, "Payload:  %d bytes\n", nPayload);
        std::fprintf(f, "Duration: %.3f s\n", dElapsed);
        std::fprintf(f, "Linger:   %d ms\n", nLingerMs);
        std::fprintf(f, "Batch:    %d\n", nBatch);
        std::fprintf(f, "\n=== Final summary ===\n");
        std::fprintf(f, "Total calls:          %llu\n", (unsigned long long)qwTotal);
        std::fprintf(f, "Throughput (calls/s): %.0f\n", dThru);
        PrintStatsBlock(f, "Dispatch latency", statLat, "ns",
                        static_cast<uint64_t>(vAllLat.size()));
        if (!vAllLat.empty()) PrintHistogram(f, vAllLat, "ns");
        std::fprintf(f, "\n=== Acceptance thresholds ===\n");
        std::fprintf(f, "  p50 < %.0f ns:       %s  (%.0f ns)\n", dP50Limit,
                     bPassP50  ? "PASS" : "FAIL", statLat.dP50);
        std::fprintf(f, "  p99 < %.0f ns:      %s  (%.0f ns)\n", dP99Limit,
                     bPassP99  ? "PASS" : "FAIL", statLat.dP99);
        std::fprintf(f, "  throughput > %.1fM/s: %s  (%.2fM/s on %d threads)\n",
                     dThruLimit / 1.0e6,
                     bPassThru ? "PASS" : "FAIL",
                     dThru / 1.0e6, nThreads);
        std::fclose(f);
        std::printf("\nReport written to %s\n", sReport.c_str());
    }
    else
    {
        std::fprintf(stderr, "[BENCH] failed to open report file: %s\n",
                     sReport.c_str());
    }

    // ── Cleanup ──────────────────────────────────────────────────────────────
    if (bCleanup)
    {
        try
        {
            auto kCfg = BuildKv8Config(sBrokers, "sasl_plaintext", "PLAIN",
                                       sUser, sPass, "");
            auto cleaner = kv8::IKv8Consumer::Create(kCfg);
            cleaner->DeleteChannel(sChannel);
            std::printf("[BENCH] channel %s deleted\n", sChannel.c_str());
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "[BENCH] cleanup failed: %s\n", e.what());
        }
    }

    return bAllPass ? 0 : 1;
}
