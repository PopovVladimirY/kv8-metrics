////////////////////////////////////////////////////////////////////////////////
// kv8bench -- Kv8 producer + consumer latency benchmark
//
// Measures three independent latency distributions using two complementary
// clocks.  Producer and consumer run on the same host so both clocks are
// directly comparable without any synchronisation assumption.
//
//   1. Dispatch latency  (QPC / CLOCK_MONOTONIC, nanosecond resolution)
//      Time from the Produce() call entry to its return.
//      Measures the cost of enqueuing into the librdkafka send buffer.
//
//   2. Producer -> Broker latency  (wall clock, millisecond resolution)
//      Time from the producer's system-clock instant (embedded in each
//      message payload) to the Kafka broker timestamp attached to that
//      message (tsKafkaMs from rd_kafka_message_timestamp).
//      Includes: linger/batching delay, TCP send, broker storage.
//
//   3. Broker -> Consumer latency  (wall clock, millisecond resolution)
//      Time from the broker timestamp to the consumer's system-clock instant
//      at the moment the Subscribe/Poll callback fires.
//      Includes: consumer fetch latency, broker polling, callback dispatch.
//
//   4. End-to-end latency  (wall clock, ms) = P->B + B->C
//      Total latency from message enqueue to consumer delivery.
//
// The consumer runs CONCURRENTLY with the producer (separate thread using
// Subscribe/Poll).  This makes E2E latency per-message, not batch-after-flush.
// It also allows observing whether the consumer can keep up with the producer;
// the report contains per-second throughput rows so accumulated lag is visible.
//
// Duration vs count mode
// ----------------------
//   --duration <s>  : produce as fast as possible for this many seconds.
//                     Default is 10 s when neither flag is given.
//   --count <N>     : produce exactly N messages, then stop.
//                     If both are given, --count takes priority.
//
// Usage:
//   kv8bench [options]
//
//   --brokers  <b>     Bootstrap brokers           [localhost:19092]
//   --duration <s>     Produce for this many seconds [10]  (default mode)
//   --count    <N>     Produce exactly N messages; overrides --duration
//   --topic    <t>     Kafka topic (auto-generated if omitted)
//   --linger   <ms>    Producer linger.ms note in report [5]
//   --batch    <n>     batch.num.messages note in report  [10000]
//   --report   <path>  Output report file           [kv8bench_<ts>.txt]
//   --user / --pass    SASL credentials
//   --security-proto   plaintext|sasl_plaintext|sasl_ssl
//   --sasl-mechanism   PLAIN|SCRAM-SHA-256|SCRAM-SHA-512
//   --no-cleanup       Keep the Kafka topic after the run
//   --help
////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#endif

#include <kv8/IKv8Producer.h>
#include <kv8/IKv8Consumer.h>
#include "UdtSchemaParser.h"            // ParseUdtSchema -- UDT verify pass

#include <kv8util/Kv8Timer.h>
#include <kv8util/Kv8BenchMsg.h>
#include <kv8util/Kv8Stats.h>
#include <kv8util/Kv8TopicUtils.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace kv8;
using namespace kv8util;

// Timer, BenchMsg, Stats and TopicUtils are now provided by kv8util.

// GenerateTopicName is now provided by kv8util::GenerateTopicName.

// BenchMsg is now provided by kv8util::BenchMsg.

// Stats and ComputeStats are now provided by kv8util.

// PrintStatsBlock and PrintHistogram are now provided by kv8util.

// NowUTC is now provided by kv8util::NowUTC.

// ProgressRow is now provided by kv8util::ProgressRow.

////////////////////////////////////////////////////////////////////////////////
// UDT benchmark helpers
////////////////////////////////////////////////////////////////////////////////

// FNV-32 hash -- mirrors kv8log_impl.cpp (no external dependency needed here).
static uint32_t Fnv32Local(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

// Write one KafkaRegistryRecord to a Kafka topic.
// Mirrors WriteRegistryRecord() in kv8log_impl.cpp.
static void WriteRegistryRecordLocal(
    kv8::IKv8Producer*  p,
    const std::string&  regTopic,
    uint32_t            dwHash,
    uint16_t            wCounterID,
    uint16_t            wFlags,
    double              dbMin,
    double              dbMax,
    uint64_t            qwFreq,
    uint64_t            qwTimer,
    uint32_t            dwHi,
    uint32_t            dwLo,
    const std::string&  sName,
    const std::string&  sTopic)
{
    kv8::KafkaRegistryRecord rec{};
    rec.dwHash           = dwHash;
    rec.wCounterID       = wCounterID;
    rec.wFlags           = wFlags;
    rec.dbMin            = dbMin;
    rec.dbAlarmMin       = dbMin;
    rec.dbMax            = dbMax;
    rec.dbAlarmMax       = dbMax;
    rec.wNameLen         = (uint16_t)sName.size();
    rec.wTopicLen        = (uint16_t)sTopic.size();
    rec.wVersion         = kv8::KV8_REGISTRY_VERSION;
    rec.wPad             = 0;
    rec.qwTimerFrequency = qwFreq;
    rec.qwTimerValue     = qwTimer;
    rec.dwTimeHi         = dwHi;
    rec.dwTimeLo         = dwLo;

    std::vector<uint8_t> buf(sizeof(rec) + sName.size() + sTopic.size());
    memcpy(buf.data(),                              &rec,          sizeof(rec));
    memcpy(buf.data() + sizeof(rec),                sName.data(),  sName.size());
    memcpy(buf.data() + sizeof(rec) + sName.size(), sTopic.data(), sTopic.size());
    p->Produce(regTopic, buf.data(), buf.size(), nullptr, 0);
}

// Attitude sensor UDT payload.
// 9 x f64 (position/velocity/acceleration) + 1 x i64 (send wall-clock ms)
// + 1 x u32 (0-based sample counter for continuity verification).
// Total payload: 84 bytes -- well within KV8_UDT_MAX_PAYLOAD (240 bytes).
#pragma pack(push, 1)
struct BenchUdtMsg
{
    double   pos_x, pos_y, pos_z;   // position (m)
    double   vel_x, vel_y, vel_z;   // velocity (m/s)
    double   acc_x, acc_y, acc_z;   // acceleration (m/s^2)
    int64_t  tSendWallMs;           // wall-clock ms at send (for E2E latency)
    uint32_t nSeqCounter;           // 0-based sample index; verified post-bench
};
#pragma pack(pop)
static_assert(sizeof(BenchUdtMsg) == 84, "BenchUdtMsg size mismatch");

// Schema JSON for BenchAttitude.
// Field type tokens are those recognised by UdtSchemaParser (f64, i64, etc.).
// Single-quoted keys avoid escaping in C string literals.
static const char kUdtSchemaJson[] =
    "{'name':'BenchAttitude','fields':["
    "{'n':'pos_x','t':'f64'},"
    "{'n':'pos_y','t':'f64'},"
    "{'n':'pos_z','t':'f64'},"
    "{'n':'vel_x','t':'f64'},"
    "{'n':'vel_y','t':'f64'},"
    "{'n':'vel_z','t':'f64'},"
    "{'n':'acc_x','t':'f64'},"
    "{'n':'acc_y','t':'f64'},"
    "{'n':'acc_z','t':'f64'},"
    "{'n':'tSendWallMs','t':'i64'},"
    "{'n':'nSeqCounter','t':'u32'}"
    "]}";

////////////////////////////////////////////////////////////////////////////////
// main
////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    TimerInit();

    // ── Parse arguments ──────────────────────────────────────────────────────
    std::string sBrokers      = "localhost:19092";
    std::string sTopic;
    std::string sSecProto     = "sasl_plaintext";
    std::string sSaslMech     = "PLAIN";
    std::string sUser         = "kv8producer";
    std::string sPass         = "kv8secret";
    std::string sReport;
    uint64_t    nCountArg     = 1000000; // only used when bCountExplicit
    double      dDurationSec  = 10.0;   // default mode: 10 s
    int         nLingerMs     = 5;
    int         nBatch        = 10000;
    int         nPartitions   = 1;      // topic partitions -- each adds one independent fetch stream
    int         nSampleRate   = 100;    // 1-in-N messages sampled for latency (rest just counted)
    int64_t     nTempoMps     = 500000; // producer rate limit (msg/s); 0 = unlimited
    bool        bCleanup      = true;
    bool        bCountExplicit= false;  // true when --count is on the command line
    bool        bUdt          = false;   // --udt: UDT (attitude sensor) benchmark mode
    int64_t     nUdtRate      = 100000;  // --udt-rate: UDT samples/s [100000]
    bool        bUdtRateExplicit = false; // true when --udt-rate was given explicitly

    for (int i = 1; i < argc; ++i)
    {
        auto match = [&](const char *f) { return strcmp(argv[i], f) == 0; };
        auto next  = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : nullptr; };

        if      (match("--brokers"))        { auto v = next(); if (v) sBrokers     = v; }
        else if (match("--topic"))          { auto v = next(); if (v) sTopic       = v; }
        else if (match("--count"))          { auto v = next(); if (v) { nCountArg = (uint64_t)strtoull(v, nullptr, 10); bCountExplicit = true; } }
        else if (match("--duration"))       { auto v = next(); if (v) dDurationSec = atof(v); }
        else if (match("--linger"))         { auto v = next(); if (v) nLingerMs    = atoi(v); }
        else if (match("--batch"))          { auto v = next(); if (v) nBatch       = atoi(v); }
        else if (match("--partitions"))     { auto v = next(); if (v) nPartitions  = atoi(v); }
        else if (match("--sample-rate"))    { auto v = next(); if (v) nSampleRate  = atoi(v); }
        else if (match("--tempo"))           { auto v = next(); if (v) nTempoMps    = (int64_t)strtoull(v, nullptr, 10); }
        else if (match("--no-tempo"))        { nTempoMps = 0; }
        else if (match("--report"))         { auto v = next(); if (v) sReport      = v; }
        else if (match("--security-proto")) { auto v = next(); if (v) sSecProto    = v; }
        else if (match("--sasl-mechanism")) { auto v = next(); if (v) sSaslMech    = v; }
        else if (match("--user"))           { auto v = next(); if (v) sUser        = v; }
        else if (match("--pass"))           { auto v = next(); if (v) sPass        = v; }
        else if (match("--udt"))          { bUdt = true; }
        else if (match("--udt-rate"))     { auto v = next(); if (v) { nUdtRate = (int64_t)strtoull(v, nullptr, 10); bUdtRateExplicit = true; } }
        else if (match("--no-cleanup"))  { bCleanup = false; }
        else if (match("--help"))
        {
            fprintf(stdout,
                "kv8bench -- Kv8 producer + consumer latency benchmark\n\n"
                "Usage: kv8bench [options]\n\n"
                "  --brokers  <b>     Bootstrap brokers          [localhost:19092]\n"
                "  --duration <s>     Produce for N seconds       [10]  (default mode)\n"
                "  --count    <N>     Produce exactly N messages; overrides --duration\n"
                "  --topic    <t>     Kafka topic (auto-gen if omitted)\n"
                "  --linger   <ms>    Producer linger.ms note     [5]\n"
                "  --batch    <n>     batch.num.messages note      [10000]\n"
                "  --partitions <n>   Topic partition count        [4]\n"
                "  --sample-rate <n>  1-in-N msgs sampled for latency [100]\n"
                "  --tempo <n>        Rate limit: msg/s (scalar) or samples/s (UDT) [500000]\n"
                "  --no-tempo         Unlimited rate\n"
                "  --report   <path>  Report file                 [kv8bench_<ts>.txt]\n"
                "  --user / --pass    SASL credentials\n"
                "  --security-proto   plaintext|sasl_plaintext|sasl_ssl\n"
                "  --sasl-mechanism   PLAIN|SCRAM-SHA-256|SCRAM-SHA-512\n"
                "  --udt               UDT (attitude sensor) benchmark mode\n"
                "  --udt-rate <n>      Override UDT sample rate; takes priority over --tempo [100000]\n"
                "  --no-cleanup       Keep the Kafka topic after the run\n\n"
                "Consumer runs concurrently with producer (Subscribe/Poll thread).\n"
                "Per-second lag rows show whether the consumer keeps up.\n");
            return 0;
        }
        else
        {
            fprintf(stderr, "[BENCH] unknown arg: %s\n", argv[i]);
            return 1;
        }
    }

    // --tempo also controls UDT rate unless --udt-rate was given explicitly.
    if (!bUdtRateExplicit)
        nUdtRate = nTempoMps;

    // Decide mode: if --count was explicit it overrides duration
    const bool bCountMode = bCountExplicit;

    if (bCountMode && nCountArg == 0)
    { fprintf(stderr, "[BENCH] --count must be > 0\n"); return 1; }
    if (!bCountMode && dDurationSec <= 0.0)
    { fprintf(stderr, "[BENCH] --duration must be > 0\n"); return 1; }
    if (nSampleRate < 1)
    { fprintf(stderr, "[BENCH] --sample-rate must be >= 1\n"); return 1; }

    // Auto-generate names
    if (sTopic.empty())  sTopic  = GenerateTopicName("kv8bench");
    if (sReport.empty())
    {
        char buf[32];
#ifdef _WIN32
        SYSTEMTIME st; GetSystemTime(&st);
        snprintf(buf, sizeof(buf), "%04u%02u%02uT%02u%02u%02uZ",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
#else
        time_t t = time(nullptr);
        strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", gmtime(&t));
#endif
        sReport = "kv8bench_" + std::string(buf) + ".txt";
    }

    if (bCountMode)
        fprintf(stdout, "[BENCH] Mode    : count %" PRIu64 " messages\n", nCountArg);
    else
        fprintf(stdout, "[BENCH] Mode    : duration %.1f s\n", dDurationSec);
    if (!bUdt)
    {
        if (nTempoMps > 0)
            fprintf(stdout, "[BENCH] Tempo   : %" PRId64 " msg/s\n", nTempoMps);
        else
            fprintf(stdout, "[BENCH] Tempo   : unlimited\n");
    }
    fprintf(stdout, "[BENCH] Topic   : %s\n", sTopic.c_str());
    fprintf(stdout, "[BENCH] Report  : %s\n", sReport.c_str());

    // ── Build Kv8Config ──────────────────────────────────────────────────────
    Kv8Config kCfg;
    kCfg.sBrokers       = sBrokers;
    kCfg.sSecurityProto = sSecProto;
    kCfg.sSaslMechanism = sSaslMech;
    kCfg.sUser          = sUser;
    kCfg.sPass          = sPass;

    if (!bUdt)
    {
        // ── Create topic explicitly before subscribing ────────────────────────────
        // Doing this upfront (a) lets us control the partition count, which
        // multiplies consumer fetch throughput directly: librdkafka opens one
        // independent fetch pipeline per partition; (b) avoids the metadata-refresh
        // delay that occurs when a consumer subscribes to a topic that does not yet
        // exist (librdkafka retries on a slow background interval).
        fprintf(stdout, "[BENCH] Creating topic '%s' with %d partition(s)...\n",
                sTopic.c_str(), nPartitions);
        auto adm = IKv8Consumer::Create(kCfg);
        if (!adm)
        { fprintf(stderr, "[BENCH] consumer create failed (admin pre-flight)\n"); return 1; }

        // Abort if topic already exists and has data (stale run guard).
        auto counts = adm->GetTopicMessageCounts({sTopic}, 8000);
        auto it = counts.find(sTopic);
        if (it != counts.end() && it->second > 0)
        {
            fprintf(stderr,
                "[BENCH] ERROR: topic '%s' already has %" PRId64 " message(s).\n"
                "[BENCH]        Use --topic <new-name> or delete the topic first.\n",
                sTopic.c_str(), it->second);
            return 2;
        }

        if (!adm->CreateTopic(sTopic, nPartitions, 1, 8000))
        {
            fprintf(stderr, "[BENCH] ERROR: could not create topic '%s'\n", sTopic.c_str());
            return 1;
        }
        fprintf(stdout, "[BENCH] Topic ready.\n");

        // Let broker propagate metadata to all nodes before the consumer subscribes.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // ── UDT benchmark mode ─────────────────────────────────────────────────────
    // Self-contained execution path: runs the full UDT benchmark and returns.
    // Skipped entirely when --udt is not specified.
    if (bUdt)
    {
        // ── Topic name derivation ──────────────────────────────────────────────
        // Channel "bench" is fixed; session ID reuses the auto-generated sTopic.
        // Registry topic: bench._registry
        // Group topic:    bench.<sessionID>.d.<Fnv32(channel)hex8>
        // UDT data topic: bench.<sessionID>.u.<Fnv32(schema)hex8>.<feedID hex4>
        const std::string sChannel      = "bench";
        const std::string sSessionID    = sTopic;
        const uint32_t    dwChanHash    = Fnv32Local(sChannel);
        const uint32_t    dwSchemaHash  = Fnv32Local(std::string(kUdtSchemaJson));
        const uint16_t    wFeedId       = 0;

        char szCH[16], szSH[16], szFI[16];
        snprintf(szCH, sizeof(szCH), "%08X", dwChanHash);
        snprintf(szSH, sizeof(szSH), "%08X", dwSchemaHash);
        snprintf(szFI, sizeof(szFI), "%04X", (unsigned)wFeedId);

        const std::string sRegTopic   = sChannel + "._registry";
        const std::string sGroupTopic = sChannel + "." + sSessionID + ".d." + szCH;
        const std::string sUdtTopic   = sChannel + "." + sSessionID + ".u." + szSH + "." + szFI;

        fprintf(stdout, "[UDT] UDT benchmark mode (BenchAttitude sensor)\n");
        fprintf(stdout, "[UDT] Channel   : %s\n", sChannel.c_str());
        fprintf(stdout, "[UDT] Session   : %s\n", sSessionID.c_str());
        fprintf(stdout, "[UDT] Schema    : BenchAttitude  hash=%08X\n", dwSchemaHash);
        fprintf(stdout, "[UDT] Data topic: %s\n", sUdtTopic.c_str());
        fprintf(stdout, "[UDT] Rate      : %" PRId64 " samples/s\n", nUdtRate);

        // ── Create producer ────────────────────────────────────────────────────
        auto udtProducer = IKv8Producer::Create(kCfg);
        if (!udtProducer)
        {
            fprintf(stderr, "[UDT] ERROR: producer create failed\n");
            return 1;
        }

        // ── Create UDT data topic ──────────────────────────────────────────────
        {
            fprintf(stdout, "[UDT] Creating UDT topic '%s' with %d partition(s)...\n",
                    sUdtTopic.c_str(), nPartitions);
            auto adm = IKv8Consumer::Create(kCfg);
            if (!adm)
            {
                fprintf(stderr, "[UDT] ERROR: admin consumer create failed\n");
                return 1;
            }
            auto counts2 = adm->GetTopicMessageCounts({sUdtTopic}, 8000);
            auto it2 = counts2.find(sUdtTopic);
            if (it2 != counts2.end() && it2->second > 0)
            {
                fprintf(stderr,
                    "[UDT] ERROR: topic '%s' already has %" PRId64 " message(s).\n"
                    "[UDT]        Use --topic <new-name> or delete the topic first.\n",
                    sUdtTopic.c_str(), it2->second);
                return 2;
            }
            if (!adm->CreateTopic(sUdtTopic, nPartitions, 1, 8000))
            {
                fprintf(stderr, "[UDT] ERROR: could not create topic '%s'\n",
                        sUdtTopic.c_str());
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            fprintf(stdout, "[UDT] Topic ready.\n");
        }

        // ── Write registry records so kv8scope can discover this session ───────
        {
            // Timer anchors for the GROUP record (same approach as kv8log_open).
            uint64_t qwFreq = 0, qwTimer = 0;
            uint32_t dwHi   = 0, dwLo    = 0;
#ifdef _WIN32
            LARGE_INTEGER liF; QueryPerformanceFrequency(&liF); qwFreq = (uint64_t)liF.QuadPart;
            LARGE_INTEGER liN; QueryPerformanceCounter(&liN);   qwTimer = (uint64_t)liN.QuadPart;
            FILETIME ft; GetSystemTimeAsFileTime(&ft);
            dwHi = ft.dwHighDateTime;
            dwLo = ft.dwLowDateTime;
#else
            struct timespec tsM; clock_gettime(CLOCK_MONOTONIC, &tsM);
            qwFreq  = 1000000000ULL;
            qwTimer = (uint64_t)tsM.tv_sec * 1000000000ULL + (uint64_t)tsM.tv_nsec;
            struct timespec tsR; clock_gettime(CLOCK_REALTIME, &tsR);
            uint64_t ft100 = ((uint64_t)tsR.tv_sec * 1000000000ULL
                              + (uint64_t)tsR.tv_nsec) / 100ULL
                             + 116444736000000000ULL;
            dwHi = (uint32_t)(ft100 >> 32);
            dwLo = (uint32_t)(ft100 & 0xFFFFFFFFu);
#endif
            // GROUP record: timer anchors for the session.
            WriteRegistryRecordLocal(udtProducer.get(), sRegTopic,
                dwChanHash, kv8::KV8_CID_GROUP, 0,
                0.0, 0.0, qwFreq, qwTimer, dwHi, dwLo,
                sChannel, sGroupTopic);

            // SCHEMA record: carries full JSON so kv8scope can decode payloads.
            WriteRegistryRecordLocal(udtProducer.get(), sRegTopic,
                dwSchemaHash, kv8::KV8_CID_SCHEMA, 0,
                0.0, 0.0, 0, 0, 0, 0,
                "BenchAttitude", std::string(kUdtSchemaJson));

            // FEED record: links the feed name and data topic to the schema.
            WriteRegistryRecordLocal(udtProducer.get(), sRegTopic,
                dwSchemaHash, wFeedId,
                static_cast<uint16_t>(kv8::KV8_FLAG_UDT | 1u),
                0.0, 0.0, 0, 0, 0, 0,
                "attitude", sUdtTopic);

            udtProducer->Flush(5000);
            fprintf(stdout, "[UDT] Registry records written to '%s'.\n", sRegTopic.c_str());
        }

        // ── Shared state ───────────────────────────────────────────────────────
        std::atomic<int64_t> gUdtSent{0};
        std::atomic<int64_t> gUdtRecv{0};
        std::atomic<bool>    gUdtStop{false};
        std::atomic<bool>    gUdtReset{false};

        // Latency sample array (E2E = consumer wall ms - producer wall ms).
        const size_t nUdtReserve = bCountMode
            ? (size_t)nCountArg  / (size_t)nSampleRate + 1
            : (size_t)(dDurationSec * (double)nUdtRate) / (size_t)nSampleRate + 1;
        auto aUdtE2eMs = std::unique_ptr<double[]>(new double[nUdtReserve]);
        std::atomic<size_t> nUdtSamples{0};

        std::vector<ProgressRow> vUdtProgress;

        // ── Consumer thread ────────────────────────────────────────────────────
        std::thread udtConsumer([&]()
        {
            auto cons = IKv8Consumer::Create(kCfg);
            if (!cons)
            {
                fprintf(stderr, "[UDT] consumer thread: create failed\n");
                return;
            }
            cons->Subscribe(sUdtTopic);

            size_t  nLocalRecv    = 0;
            size_t  nLocalSamples = 0;
            double *pE2           = aUdtE2eMs.get();

            while (!gUdtStop.load())
            {
                if (gUdtReset.load(std::memory_order_relaxed))
                {
                    nLocalRecv    = 0;
                    nLocalSamples = 0;
                    gUdtReset.store(false, std::memory_order_relaxed);
                }

                const int64_t tBatchMs = WallMs();
                cons->PollBatch(50000, 5,
                    [&](std::string_view /*topic*/,
                        const void *pPayload, size_t cbPayload,
                        int64_t /*tsKafkaMs*/)
                    {
                        const size_t kMin =
                            sizeof(kv8::Kv8UDTSample) + sizeof(BenchUdtMsg);
                        if (cbPayload < kMin) return;
                        ++nLocalRecv;
                        if ((nLocalRecv % (size_t)nSampleRate) != 0) return;
                        const auto* msg = reinterpret_cast<const BenchUdtMsg*>(
                            static_cast<const uint8_t*>(pPayload)
                            + sizeof(kv8::Kv8UDTSample));
                        // Skip warmup sentinel (tSendWallMs == 0).
                        if (msg->tSendWallMs == 0) return;
                        const double e2eMs = (double)(tBatchMs - msg->tSendWallMs);
                        if (e2eMs >= 0.0 && e2eMs < 60000.0
                            && nLocalSamples < nUdtReserve)
                        {
                            pE2[nLocalSamples++] = e2eMs;
                        }
                    });
                gUdtRecv.store((int64_t)nLocalRecv, std::memory_order_relaxed);
            }

            // Final drain: up to 2 s, stop after 3 consecutive empty polls.
            {
                const auto tFinalMax = std::chrono::steady_clock::now()
                                     + std::chrono::seconds(2);
                int nEmpty = 0;
                for (;;)
                {
                    const int got = cons->PollBatch(50000, 100,
                        [&](std::string_view, const void*, size_t cb, int64_t)
                        {
                            const size_t kMin =
                                sizeof(kv8::Kv8UDTSample) + sizeof(BenchUdtMsg);
                            if (cb >= kMin) ++nLocalRecv;
                        });
                    if (got > 0)
                    {
                        nEmpty = 0;
                        gUdtRecv.store((int64_t)nLocalRecv,
                                       std::memory_order_relaxed);
                    }
                    else if (++nEmpty >= 3) break;
                    if (std::chrono::steady_clock::now() >= tFinalMax) break;
                }
            }

            nUdtSamples.store(nLocalSamples, std::memory_order_relaxed);
            gUdtRecv.store((int64_t)nLocalRecv, std::memory_order_relaxed);
            cons->Stop();
        });

        // Give the consumer time to subscribe before the warmup message.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // ── Warmup: wait for consumer partition assignment ─────────────────────
        {
            fprintf(stdout, "[UDT] Warming up consumer (waiting for partition assign)...\n");
            const uint16_t kWarmSize =
                static_cast<uint16_t>(sizeof(kv8::Kv8UDTSample) + sizeof(BenchUdtMsg));
            uint8_t wbuf[sizeof(kv8::Kv8UDTSample) + sizeof(BenchUdtMsg)] = {};
            kv8::Kv8UDTSample whdr{};
            whdr.sCommonRaw.dwBits = 2u
                | (kv8::KV8_TEL_TYPE_UDT << 5)
                | ((uint32_t)kWarmSize << 10);
            whdr.wFeedID = wFeedId;
            whdr.wSeqN   = 0xFFFF; // sentinel -- tSendWallMs in payload is 0
            memcpy(wbuf, &whdr, sizeof(whdr));
            // BenchUdtMsg bytes are all zero (tSendWallMs == 0 signals warmup).

            while (!udtProducer->Produce(sUdtTopic, wbuf, kWarmSize))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            udtProducer->Flush(5000);

            const auto tWarmMax = std::chrono::steady_clock::now()
                                + std::chrono::seconds(30);
            while (gUdtRecv.load(std::memory_order_relaxed) == 0)
            {
                if (std::chrono::steady_clock::now() >= tWarmMax)
                {
                    fprintf(stderr, "[UDT] WARNING: consumer warmup timeout\n");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            // Reset counters: warmup message not counted in the benchmark.
            gUdtSent.store(0, std::memory_order_relaxed);
            gUdtRecv.store(0, std::memory_order_relaxed);
            gUdtReset.store(true, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            fprintf(stdout, "[UDT] Consumer ready.\n");
        }

        // ── Producer loop ──────────────────────────────────────────────────────
        fprintf(stdout, "[UDT] Producing");
        if (bCountMode)
            fprintf(stdout, " %" PRIu64 " UDT samples", nCountArg);
        else
            fprintf(stdout, " for %.1f s", dDurationSec);
        fprintf(stdout, " @ %" PRId64 " samples/s ...\n", nUdtRate);
        fprintf(stdout, "[UDT] %6s  %12s  %12s  %12s  %10s  %10s\n",
                "t(s)", "sent", "recv", "lag", "send Mps", "recv Mps");
        fprintf(stdout, "[UDT] %s\n", std::string(72, '-').c_str());

        const auto tUdtStart    = std::chrono::steady_clock::now();
        const auto tUdtDeadline = tUdtStart
            + std::chrono::milliseconds((int64_t)(dDurationSec * 1000.0));

        // Token-bucket rate limiter (same pattern as scalar benchmark).
        const int64_t nUdtTokPerMs = (nUdtRate > 0) ? (nUdtRate / 1000LL) : 0;
        auto    tUdtWin    = tUdtStart;
        int64_t nUdtWinTok = (nUdtTokPerMs > 0) ? nUdtTokPerMs : 0;

        auto    tUdtLastProg  = tUdtStart;
        int64_t nUdtSentLast  = 0;
        int64_t nUdtRecvLast  = 0;
        int     nUdtElapsed   = 0;
        int64_t nUdtQueueFull = 0;

        uint16_t udtsSeq = 1; // seq 0 was warmup
        double   phase   = 0.0;
        const double dStep = 0.001;
        uint32_t nUdtPayloadSeq = 0; // 0-based payload counter written to nSeqCounter

        const uint16_t kMsgSize =
            static_cast<uint16_t>(sizeof(kv8::Kv8UDTSample) + sizeof(BenchUdtMsg));
        uint8_t buf[sizeof(kv8::Kv8UDTSample) + sizeof(BenchUdtMsg)];

        for (;;)
        {
            if (bCountMode)
            {
                if (gUdtSent.load(std::memory_order_relaxed) >= (int64_t)nCountArg)
                    break;
            }
            else
            {
                if (std::chrono::steady_clock::now() >= tUdtDeadline) break;
            }

            // Token-bucket rate limiter.
            if (nUdtTokPerMs > 0)
            {
                if (nUdtWinTok <= 0)
                {
                    auto tNext = tUdtWin + std::chrono::milliseconds(1);
                    while (std::chrono::steady_clock::now() < tNext)
                        ; // busy-wait for sub-ms precision
                    tUdtWin    = tNext;
                    nUdtWinTok = nUdtTokPerMs;
                }
                --nUdtWinTok;
            }

            // Build BenchUdtMsg (sinusoidal attitude sensor data).
            BenchUdtMsg msg{};
            msg.pos_x = 100.0 * cos(phase);
            msg.pos_y = 100.0 * sin(phase);
            msg.pos_z = 10.0  * sin(2.0 * phase);
            msg.vel_x = -sin(phase);
            msg.vel_y =  cos(phase);
            msg.vel_z = 0.2 * cos(2.0 * phase);
            msg.acc_x = -cos(phase);
            msg.acc_y = -sin(phase);
            msg.acc_z = -0.4 * sin(2.0 * phase);
            msg.tSendWallMs  = WallMs();
            msg.nSeqCounter  = nUdtPayloadSeq;
            phase += dStep;
            if (phase > 6.2831853) phase -= 6.2831853;

            // Assemble Kv8UDTSample header + BenchUdtMsg payload.
            kv8::Kv8UDTSample hdr{};
            hdr.sCommonRaw.dwBits =
                2u | (kv8::KV8_TEL_TYPE_UDT << 5) | ((uint32_t)kMsgSize << 10);
            hdr.wFeedID = wFeedId;
            hdr.wSeqN   = udtsSeq++;
            hdr.qwTimer = 0;
            memcpy(buf,                             &hdr, sizeof(hdr));
            memcpy(buf + sizeof(kv8::Kv8UDTSample), &msg, sizeof(msg));

            const bool ok = udtProducer->Produce(sUdtTopic, buf, kMsgSize,
                                                 &hdr.wFeedID, sizeof(hdr.wFeedID));
            if (ok)
            {
                gUdtSent.fetch_add(1, std::memory_order_relaxed);
                ++nUdtPayloadSeq;
            }
            else
            {
                ++nUdtQueueFull;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }

            // Progress every ~1 s.
            const auto tNow = std::chrono::steady_clock::now();
            if (tNow - tUdtLastProg >= std::chrono::milliseconds(1000))
            {
                ++nUdtElapsed;
                const int64_t curSent = gUdtSent.load(std::memory_order_relaxed);
                const int64_t curRecv = gUdtRecv.load(std::memory_order_relaxed);
                const double  sMps    = (double)(curSent - nUdtSentLast) / 1e6;
                const double  rMps    = (double)(curRecv - nUdtRecvLast) / 1e6;
                fprintf(stdout,
                    "[UDT] %6d  %12" PRId64 "  %12" PRId64
                    "  %12" PRId64 "  %10.3f  %10.3f\n",
                    nUdtElapsed, curSent, curRecv,
                    curSent - curRecv, sMps, rMps);

                ProgressRow pr{};
                pr.tSec        = nUdtElapsed;
                pr.nSent       = curSent;
                pr.nRecv       = curRecv;
                pr.nQueueFull  = nUdtQueueFull;
                pr.sendRateMps = sMps;
                pr.recvRateMps = rMps;
                vUdtProgress.push_back(pr);

                nUdtSentLast = curSent;
                nUdtRecvLast = curRecv;
                tUdtLastProg = tNow;
            }
        }

        const auto tUdtEnd = std::chrono::steady_clock::now();
        const double dUdtTotalMs =
            (double)std::chrono::duration_cast<std::chrono::microseconds>(
                tUdtEnd - tUdtStart).count() / 1000.0;

        const int64_t nUdtTotalSent = gUdtSent.load();
        fprintf(stdout, "[UDT] %s\n", std::string(72, '-').c_str());
        fprintf(stdout, "[UDT] Produced %" PRId64 " samples in %.1f ms, flushing...\n",
                nUdtTotalSent, dUdtTotalMs);
        udtProducer->Flush(60000);
        gUdtStop.store(true);

        // Wait for consumer to drain.
        {
            const auto tDrainMax  = std::chrono::steady_clock::now()
                                  + std::chrono::seconds(30);
            int64_t nRecvLast     = gUdtRecv.load(std::memory_order_relaxed);
            auto    tStaleSince   = std::chrono::steady_clock::now();

            while (true)
            {
                const int64_t curRecv = gUdtRecv.load(std::memory_order_relaxed);
                const auto    tNow    = std::chrono::steady_clock::now();

                if (curRecv >= nUdtTotalSent)
                {
                    fprintf(stdout, "[UDT] Consumer fully caught up.\n");
                    break;
                }
                if (curRecv > nRecvLast) { nRecvLast = curRecv; tStaleSince = tNow; }
                else
                {
                    const int64_t stallMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            tNow - tStaleSince).count();
                    if (stallMs >= 3000)
                    {
                        fprintf(stdout, "[UDT] Consumer stalled (lag=%" PRId64 ").\n",
                                nUdtTotalSent - curRecv);
                        break;
                    }
                }
                if (tNow >= tDrainMax)
                {
                    fprintf(stdout, "[UDT] Drain deadline reached.\n");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }

        udtConsumer.join();
        const int64_t nUdtTotalRecv = gUdtRecv.load();
        fprintf(stdout, "[UDT] Done. sent=%" PRId64 "  recv=%" PRId64 "\n",
                nUdtTotalSent, nUdtTotalRecv);

        // ── Post-session Kafka verification pass ───────────────────────────────
        // An independent consumer re-reads the UDT topic from offset 0 to:
        //   (a) count how many benchmark samples reached the broker,
        //   (b) verify all expected schema fields are present (payload size check),
        //   (c) decode nSeqCounter and verify sequential continuity.
        int64_t nUdtVerifyRecv    = 0;  // benchmark samples seen (warmup excluded)
        int64_t nUdtVerifyWarmup  = 0;  // warmup sentinel messages skipped
        int64_t nUdtSizeBad        = 0;  // samples with wrong payload size
        int64_t nUdtSeqOutOfOrder  = 0;  // non-monotonic steps (real decode errors)
        int64_t nUdtSeqGaps        = 0;  // total skipped counter values (= message loss)
        int64_t nUdtSeqFirstBadIdx = -1; // vector index of first out-of-order step
        uint32_t nUdtSeqFirstBadPrev = 0; // counter value before first bad step
        uint32_t nUdtSeqFirstBadGot  = 0; // counter value at first bad step
        bool    bUdtVerifyRan      = false;
        std::vector<int64_t>  vUdtVerifyMs;    // tSendWallMs (for regularity analysis)
        std::vector<uint32_t> vUdtSeqCounters; // nSeqCounter value per sample

        {
            fprintf(stdout, "\n[UDT] %s\n", std::string(72, '=').c_str());
            fprintf(stdout, "[UDT] Post-session Kafka verify\n");
            fprintf(stdout, "[UDT] %s\n", std::string(72, '-').c_str());

            // -- Schema field inventory ----------------------------------------
            // Parse kUdtSchemaJson; confirm fields and payload size vs sizeof(BenchUdtMsg).
            {
                static const char* const kFTypeNames[] =
                    { "i8","u8","i16","u16","i32","u32","i64","u64","f32","f64","rat" };
                ResolvedSchema rs;
                std::string schErr;
                std::map<std::string, ResolvedSchema> noKnown;
                fprintf(stdout, "[UDT] Schema field inventory:\n");
                if (!ParseUdtSchema(kUdtSchemaJson, noKnown, rs, schErr))
                {
                    fprintf(stderr, "[UDT]   ERROR parsing kUdtSchemaJson: %s\n",
                            schErr.c_str());
                }
                else
                {
                    const bool bSzOk = (rs.payload_size == sizeof(BenchUdtMsg));
                    fprintf(stdout, "[UDT]   Name        : %s\n", rs.schema_name.c_str());
                    fprintf(stdout, "[UDT]   Fields      : %zu\n", rs.fields.size());
                    fprintf(stdout, "[UDT]   Payload     : %u bytes  (%s)\n",
                            rs.payload_size,
                            bSzOk ? "matches sizeof(BenchUdtMsg)"
                                  : "MISMATCH with sizeof(BenchUdtMsg)");
                    fprintf(stdout, "[UDT]   %3s  %-20s  %-6s  %6s\n",
                            "#", "Field", "Type", "Offset");
                    fprintf(stdout, "[UDT]   %s\n", std::string(42, '-').c_str());
                    for (size_t fi = 0; fi < rs.fields.size(); ++fi)
                    {
                        const auto& f = rs.fields[fi];
                        const char* tn = ((int)f.type < 11)
                            ? kFTypeNames[(int)f.type] : "?";
                        fprintf(stdout, "[UDT]   %3zu  %-20s  %-6s  %3u\n",
                                fi, f.name.c_str(), tn, (unsigned)f.offset);
                    }
                    if (!bSzOk)
                        fprintf(stderr,
                            "[UDT]   ERROR: schema payload_size=%u, "
                            "sizeof(BenchUdtMsg)=%zu\n",
                            rs.payload_size, sizeof(BenchUdtMsg));
                }
            }
            fprintf(stdout, "[UDT] %s\n", std::string(72, '-').c_str());

            // -- Message-by-message verify -------------------------------------
            fprintf(stdout, "[UDT] Re-reading '%s' from offset 0 (timeout 30 s)...\n",
                    sUdtTopic.c_str());

            auto verifier = IKv8Consumer::Create(kCfg);
            if (!verifier)
            {
                fprintf(stderr, "[UDT] WARNING: could not create verification consumer.\n");
            }
            else
            {
                bUdtVerifyRan = true;
                const size_t kExactMsg =
                    sizeof(kv8::Kv8UDTSample) + sizeof(BenchUdtMsg);
                vUdtVerifyMs.reserve((size_t)nUdtTotalSent);
                vUdtSeqCounters.reserve((size_t)nUdtTotalSent);

                verifier->ConsumeTopicFromBeginning(sUdtTopic, 30000,
                    [&](const void *pPayload, size_t cbPayload, int64_t /*ts*/)
                    {
                        if (cbPayload < kExactMsg) return; // too small -- skip
                        // Identify warmup sentinels by payload content: tSendWallMs == 0.
                        // Do NOT gate on wSeqN == 0xFFFF: wSeqN is a rolling uint16_t
                        // that aliases to 65535 naturally every 65536 messages (at
                        // positions 65534, 131070, 196606, ...), which would silently
                        // discard real data samples as false warmup hits.
                        // The real-time consumer correctly uses tSendWallMs == 0.
                        const auto* msg = reinterpret_cast<const BenchUdtMsg*>(
                            static_cast<const uint8_t*>(pPayload)
                            + sizeof(kv8::Kv8UDTSample));
                        if (msg->tSendWallMs == 0) { ++nUdtVerifyWarmup; return; }
                        ++nUdtVerifyRecv;
                        if (cbPayload != kExactMsg) { ++nUdtSizeBad; return; }
                        vUdtVerifyMs.push_back(msg->tSendWallMs);
                        vUdtSeqCounters.push_back(msg->nSeqCounter);
                    });

                // Counter continuity: values must be strictly monotonically
                // increasing. Gaps are expected (= lost messages in Kafka).
                // Only out-of-order or duplicate counter values are real errors.
                if (!vUdtSeqCounters.empty())
                {
                    for (size_t ci = 1; ci < vUdtSeqCounters.size(); ++ci)
                    {
                        const uint32_t prev = vUdtSeqCounters[ci - 1];
                        const uint32_t cur  = vUdtSeqCounters[ci];
                        if (cur <= prev)
                        {
                            if (nUdtSeqFirstBadIdx < 0)
                            {
                                nUdtSeqFirstBadIdx  = (int64_t)ci;
                                nUdtSeqFirstBadPrev = prev;
                                nUdtSeqFirstBadGot  = cur;
                            }
                            ++nUdtSeqOutOfOrder;
                        }
                        else
                        {
                            nUdtSeqGaps += (int64_t)(cur - prev - 1);
                        }
                    }
                }

                const int64_t nLost     = nUdtTotalSent - nUdtVerifyRecv;
                const double  dLossRate = (nUdtTotalSent > 0)
                    ? (double)nLost / (double)nUdtTotalSent * 100.0 : 0.0;
                const bool bPassed = (nLost == 0 && nUdtSizeBad == 0
                                      && nUdtSeqOutOfOrder == 0);

                fprintf(stdout, "[UDT] Samples produced    : %" PRId64 "\n",
                        nUdtTotalSent);
                fprintf(stdout, "[UDT] Samples in Kafka    : %" PRId64
                                "  (+ %" PRId64 " warmup sentinel(s))\n",
                        nUdtVerifyRecv, nUdtVerifyWarmup);
                fprintf(stdout, "[UDT] Size mismatches     : %" PRId64 "\n",
                        nUdtSizeBad);
                fprintf(stdout, "[UDT] Missing             : %" PRId64, nLost);
                if (nLost > 0)
                    fprintf(stdout, "  (%.4f%% loss)", dLossRate);
                fprintf(stdout, "\n");
                fprintf(stdout, "[UDT] Counter continuity (nSeqCounter):\n");
                if (!vUdtSeqCounters.empty())
                {
                    fprintf(stdout,
                        "[UDT]   First decoded    : %" PRIu32 "\n"
                        "[UDT]   Last decoded     : %" PRIu32 "\n"
                        "[UDT]   Gaps (lost msgs) : %" PRId64 "\n",
                        vUdtSeqCounters.front(),
                        vUdtSeqCounters.back(),
                        nUdtSeqGaps);
                    if (nUdtSeqOutOfOrder == 0)
                        fprintf(stdout,
                            "[UDT]   Out-of-order     : 0"
                            "  -- sequence is monotonically increasing\n");
                    else
                        fprintf(stdout,
                            "[UDT]   Out-of-order     : %" PRId64
                            "  (first at index %" PRId64
                            ": prev=%" PRIu32 " got=%" PRIu32 ")\n",
                            nUdtSeqOutOfOrder, nUdtSeqFirstBadIdx,
                            nUdtSeqFirstBadPrev, nUdtSeqFirstBadGot);
                }
                else
                {
                    fprintf(stdout, "[UDT]   No samples decoded.\n");
                }
                fprintf(stdout, "[UDT] >>> %s\n",
                        bPassed ? "PASSED -- fields, continuity, count all correct."
                                : "FAILED -- see details above.");
                fprintf(stdout, "[UDT] %s\n", std::string(72, '=').c_str());
            }
        }

        // ── Timestamp regularity analysis (from verify pass) ──────────────────
        std::vector<double> vUdtIntervalMs;  // inter-sample intervals (ms)
        int64_t nUdtNonMono   = 0;
        int64_t nUdtAnomalies = 0;
        bool    bUdtTsRan     = false;

        if (bUdtVerifyRan && vUdtVerifyMs.size() > 1)
        {
            const double dExpectedMs = (nUdtRate > 0)
                ? 1000.0 / (double)nUdtRate : 0.0;

            fprintf(stdout, "\n[UDT] %s\n", std::string(72, '=').c_str());
            fprintf(stdout, "[UDT] UDT payload timestamp regularity\n");
            fprintf(stdout, "[UDT] %s\n", std::string(72, '-').c_str());
            fprintf(stdout, "[UDT] Expected interval : %.4f ms  (%d samples/s)\n",
                    dExpectedMs, (int)nUdtRate);

            bUdtTsRan = true;
            vUdtIntervalMs.reserve(vUdtVerifyMs.size() - 1);

            for (size_t i = 1; i < vUdtVerifyMs.size(); ++i)
            {
                const int64_t dt = vUdtVerifyMs[i] - vUdtVerifyMs[i - 1];
                if (dt < 0) { ++nUdtNonMono; vUdtIntervalMs.push_back(-(double)(-dt)); }
                else         vUdtIntervalMs.push_back((double)dt);
            }

            Stats sTs = ComputeStats(vUdtIntervalMs);

            // Anomaly threshold: intervals > max(mean*50, 5*expected ms, 5 ms).
            const double dThr = [&]() {
                double t = sTs.dMean * 50.0;
                if (dExpectedMs * 5.0 > t) t = dExpectedMs * 5.0;
                if (5.0 > t) t = 5.0;
                return t;
            }();
            for (double v : vUdtIntervalMs)
                if (v > dThr) ++nUdtAnomalies;

            const bool bTsOk = (nUdtNonMono == 0 && nUdtAnomalies == 0);

            fprintf(stdout,
                "[UDT] Consecutive pairs  : %zu\n"
                "[UDT] Min interval       : %10.4f ms\n"
                "[UDT] Median interval    : %10.4f ms\n"
                "[UDT] Mean interval      : %10.4f ms\n"
                "[UDT] Max interval       : %10.4f ms\n"
                "[UDT] StdDev             : %10.4f ms\n"
                "[UDT] Non-monotonic      : %" PRId64 "\n"
                "[UDT] Anomaly threshold  : %10.4f ms\n"
                "[UDT] Anomalies          : %" PRId64 "\n",
                vUdtIntervalMs.size(),
                sTs.dMin, sTs.dMedian, sTs.dMean, sTs.dMax, sTs.dStdDev,
                nUdtNonMono, dThr, nUdtAnomalies);
            fprintf(stdout, "[UDT] >>> %s\n",
                    bTsOk ? "PASSED -- timestamps monotonic and regular."
                           : "ANOMALIES DETECTED -- see report for details.");
            fprintf(stdout, "[UDT] %s\n", std::string(72, '=').c_str());
        }

        // ── Stats + report ─────────────────────────────────────────────────────
        const size_t nSmp  = nUdtSamples.load();
        const double dTput = (dUdtTotalMs > 0.0)
                           ? (double)nUdtTotalSent / (dUdtTotalMs / 1000.0)
                           : 0.0;
        const double dAchievedRate = dTput; // samples actually produced per second
        const size_t kWireSize = sizeof(kv8::Kv8UDTSample) + sizeof(BenchUdtMsg);
        const double dWireMBps = dTput * (double)kWireSize / 1e6;
        const int64_t nUdtLost = nUdtTotalSent - (bUdtVerifyRan ? nUdtVerifyRecv
                                                                  : nUdtTotalRecv);
        const double dLossPct  = (nUdtTotalSent > 0)
            ? (double)(nUdtLost > 0 ? nUdtLost : 0) / (double)nUdtTotalSent * 100.0
            : 0.0;

        // ── Explicit stdout summary ───────────────────────────────────────────
        fprintf(stdout, "\n[UDT] %s\n", std::string(72, '=').c_str());
        fprintf(stdout, "[UDT]   kv8bench UDT -- Summary\n");
        fprintf(stdout, "[UDT] %s\n", std::string(72, '-').c_str());

        // Throughput
        fprintf(stdout,
            "[UDT]   Configured rate      : %12" PRId64 " samples/s\n"
            "[UDT]   Achieved send rate   : %12.0f samples/s\n"
            "[UDT]   Produce time         : %12.1f ms\n"
            "[UDT]   Wire size            : %12zu bytes/sample"
            "  (hdr %zu + payload %zu)\n"
            "[UDT]   Wire throughput      : %12.3f MB/s\n"
            "[UDT]   Header overhead      : %12.1f %%\n",
            nUdtRate,
            dAchievedRate,
            dUdtTotalMs,
            kWireSize,
            sizeof(kv8::Kv8UDTSample),
            sizeof(BenchUdtMsg),
            dWireMBps,
            100.0 * (double)sizeof(kv8::Kv8UDTSample) / (double)kWireSize);

        fprintf(stdout, "[UDT] %s\n", std::string(72, '-').c_str());

        // Delivery
        fprintf(stdout,
            "[UDT]   Samples produced     : %12" PRId64 "\n"
            "[UDT]   Samples in Kafka     : %12" PRId64 "  (%s)\n"
            "[UDT]   Samples lost         : %12" PRId64 "  (%.4f%%)\n"
            "[UDT]   Queue-full events    : %12" PRId64 "\n",
            nUdtTotalSent,
            bUdtVerifyRan ? nUdtVerifyRecv : nUdtTotalRecv,
            bUdtVerifyRan ? "verified via re-read" : "real-time counter",
            (nUdtLost > 0 ? nUdtLost : (int64_t)0),
            dLossPct,
            nUdtQueueFull);

        fprintf(stdout, "[UDT] %s\n", std::string(72, '-').c_str());

        // E2E latency
        if (nSmp == 0)
        {
            fprintf(stderr, "[UDT] WARNING: no latency samples collected -- "
                            "cannot report E2E latency.\n");
        }
        else
        {
            std::vector<double> vE2e(aUdtE2eMs.get(), aUdtE2eMs.get() + nSmp);
            Stats sE2e = ComputeStats(vE2e);

            fprintf(stdout,
                "[UDT]   E2E latency (producer wall -> consumer wall)\n"
                "[UDT]   %-24s  %10.4f ms\n"
                "[UDT]   %-24s  %10.4f ms\n"
                "[UDT]   %-24s  %10.4f ms\n"
                "[UDT]   %-24s  %10.4f ms\n"
                "[UDT]   %-24s  %10.4f ms\n"
                "[UDT]   %-24s  %10.4f ms\n"
                "[UDT]   %-24s  %10.4f ms\n"
                "[UDT]   %-24s  %10.4f ms\n"
                "[UDT]   %-24s  %10.4f ms\n"
                "[UDT]   %-24s  %10zu\n",
                "min",         sE2e.dMin,
                "p50 (median)", sE2e.dP50,
                "p75",         sE2e.dP75,
                "p90",         sE2e.dP90,
                "p95",         sE2e.dP95,
                "p99",         sE2e.dP99,
                "p99.9",       sE2e.dP999,
                "max",         sE2e.dMax,
                "stddev",      sE2e.dStdDev,
                "latency samples", nSmp);

            fprintf(stdout, "[UDT] %s\n", std::string(72, '-').c_str());

            // Verification and regularity pass-fail
            if (bUdtVerifyRan)
            {
                const bool bLossOk = (nUdtLost <= 0 && nUdtSizeBad == 0);
                fprintf(stdout, "[UDT]   Kafka verify         : %s\n",
                        bLossOk ? "PASSED" : "FAILED");
                fprintf(stdout, "[UDT]   Counter continuity   : %s\n",
                        nUdtSeqOutOfOrder == 0 ? "PASSED" : "FAILED");
            }
            if (bUdtTsRan)
            {
                fprintf(stdout, "[UDT]   Timestamp regularity : %s"
                                "  (pairs=%zu  non-mono=%" PRId64
                                "  anomalies=%" PRId64 ")\n",
                        (nUdtNonMono == 0 && nUdtAnomalies == 0)
                            ? "PASSED" : "ANOMALIES",
                        vUdtIntervalMs.size(), nUdtNonMono, nUdtAnomalies);
            }
            fprintf(stdout, "[UDT] %s\n", std::string(72, '=').c_str());

            // ── Report file ────────────────────────────────────────────────────
            FILE *fRep = fopen(sReport.c_str(), "w");
            if (!fRep) fRep = stdout;

            const std::string sep(70, '=');
            const std::string sepm(70, '-');

            fprintf(fRep,
                "%s\n"
                "  kv8bench UDT -- Throughput & Latency Report\n"
                "%s\n\n"
                "  Timestamp         : %s\n"
                "  Brokers           : %s\n"
                "  Channel           : %s\n"
                "  Session           : %s\n"
                "  UDT topic         : %s\n"
                "  Schema            : BenchAttitude\n"
                "  Fields            : pos_x/y/z (f64, m)  vel_x/y/z (f64, m/s)  "
                "acc_x/y/z (f64, m/s^2)  tSendWallMs (i64, ms)  nSeqCounter (u32)\n"
                "  Payload size      : %zu bytes  (hdr %zu + payload %zu)\n"
                "  Mode              : %s\n"
                "  Configured rate   : %" PRId64 " samples/s\n"
                "  Achieved rate     : %.0f samples/s\n"
                "  Produce time      : %.1f ms\n"
                "  Linger cfg        : %d ms\n"
                "  Batch cfg         : %d\n"
                "  Partitions        : %d\n"
                "  Latency samples   : %zu  (1-in-%d)\n\n"
                "  Clock notes:\n"
                "    E2E = tSendWallMs in payload vs consumer wall clock at"
                " batch arrival.\n"
                "    Both are system_clock milliseconds -- same epoch as"
                " Kafka broker timestamps.\n"
                "    Producer and consumer share the same host.\n\n",
                sep.c_str(), sep.c_str(),
                NowUTC().c_str(),
                sBrokers.c_str(),
                sChannel.c_str(),
                sSessionID.c_str(),
                sUdtTopic.c_str(),
                kWireSize,
                sizeof(kv8::Kv8UDTSample),
                sizeof(BenchUdtMsg),
                bCountMode ? "count" : "duration",
                nUdtRate,
                dAchievedRate,
                dUdtTotalMs,
                nLingerMs,
                nBatch,
                nPartitions,
                nSmp, nSampleRate);

            // [1] Per-second throughput and lag table
            fprintf(fRep,
                "%s\n  [1] Per-Second Throughput and Lag\n%s\n\n",
                sep.c_str(), sepm.c_str());
            fprintf(fRep, "  %6s  %12s  %12s  %12s  %10s  %10s\n",
                    "t(s)", "sent", "recv", "lag", "send Mps", "recv Mps");
            fprintf(fRep, "  %s\n", std::string(68, '-').c_str());
            for (const auto& pr : vUdtProgress)
                fprintf(fRep,
                    "  %6d  %12" PRId64 "  %12" PRId64 "  %12" PRId64
                    "  %10.3f  %10.3f\n",
                    pr.tSec, pr.nSent, pr.nRecv,
                    pr.nSent - pr.nRecv,
                    pr.sendRateMps, pr.recvRateMps);
            fprintf(fRep,
                "  %6s  %12" PRId64 "  %12" PRId64 "  %12" PRId64
                "  %10s  %10s\n\n",
                "final", nUdtTotalSent, nUdtTotalRecv,
                nUdtTotalSent - nUdtTotalRecv, "-", "-");

            // [2] E2E latency
            fprintf(fRep,
                "%s\n  [2] E2E Latency"
                "  (tSendWallMs in payload -> consumer wall clock at batch)\n"
                "%s\n\n",
                sep.c_str(), sepm.c_str());
            PrintStatsBlock(fRep, "Percentile table:", sE2e, "ms",
                            (uint64_t)vE2e.size());
            PrintHistogram(fRep, vE2e, "ms");

            // [3] Throughput summary
            fprintf(fRep,
                "%s\n  [3] Throughput Summary\n%s\n\n"
                "  %-30s  %12.0f samples/s\n"
                "  %-30s  %12.3f MB/s\n"
                "  %-30s  %12.3f MB/s  (payload only)\n"
                "  %-30s  %12.1f %%\n"
                "  %-30s  %12.3f ms\n"
                "  %-30s  %12" PRId64 " events\n\n",
                sep.c_str(), sepm.c_str(),
                "UDT sample throughput",       dAchievedRate,
                "Wire payload rate (total)",    dWireMBps,
                "Payload rate (excl. header)",
                    dAchievedRate * (double)sizeof(BenchUdtMsg) / 1e6,
                "Header overhead",
                    100.0 * (double)sizeof(kv8::Kv8UDTSample) / (double)kWireSize,
                "Total produce time",           dUdtTotalMs,
                "Queue-full events",            nUdtQueueFull);

            // [4] Post-session Kafka verify + field/counter checks
            if (bUdtVerifyRan)
            {
                const int64_t nLost2   = nUdtTotalSent - nUdtVerifyRecv;
                const double  dLoss2   = (nUdtTotalSent > 0)
                    ? (double)(nLost2 > 0 ? nLost2 : 0)
                      / (double)nUdtTotalSent * 100.0
                    : 0.0;
                const bool bPassed2 = (nLost2 <= 0 && nUdtSizeBad == 0
                                       && nUdtSeqOutOfOrder == 0);
                fprintf(fRep,
                    "%s\n  [4] Post-Session Kafka Verify\n"
                    "\n"
                    "  An independent consumer re-read every record in the UDT topic\n"
                    "  from offset 0 after the benchmark ended.  Three checks are run:\n"
                    "  (a) all produced samples arrived, (b) payload size matches the\n"
                    "  schema (all fields present), (c) nSeqCounter is sequential.\n"
                    "%s\n\n"
                    "  %-30s  %s\n"
                    "  %-30s  %12" PRId64 "\n"
                    "  %-30s  %12" PRId64
                    "  (+ %" PRId64 " warmup sentinel(s))\n"
                    "  %-30s  %12" PRId64,
                    sep.c_str(), sepm.c_str(),
                    "UDT topic",                  sUdtTopic.c_str(),
                    "Samples produced",           nUdtTotalSent,
                    "Samples read back",          nUdtVerifyRecv, nUdtVerifyWarmup,
                    "Missing samples",            (nLost2 > 0 ? nLost2 : (int64_t)0));
                if (nLost2 > 0)
                    fprintf(fRep, "  (%.4f%% loss)", dLoss2);
                fprintf(fRep,
                    "\n"
                    "  %-30s  %12" PRId64 "\n"
                    "  %-30s  %12" PRId64 "\n\n"
                    "  Result: %s\n\n",
                    "Size mismatches",            nUdtSizeBad,
                    "Counter out-of-order",       nUdtSeqOutOfOrder,
                    bPassed2 ? "PASSED" : "FAILED");
            }

            // [5] Timestamp regularity
            if (bUdtTsRan && !vUdtIntervalMs.empty())
            {
                Stats sTsR = ComputeStats(vUdtIntervalMs);
                const double dThrR = [&]() {
                    double t = sTsR.dMean * 50.0;
                    const double dExpR = (nUdtRate > 0)
                        ? 1000.0 / (double)nUdtRate : 0.0;
                    if (dExpR * 5.0 > t) t = dExpR * 5.0;
                    if (5.0 > t) t = 5.0;
                    return t;
                }();
                fprintf(fRep,
                    "%s\n  [5] UDT Payload Timestamp Regularity\n"
                    "\n"
                    "  Inter-sample intervals computed from tSendWallMs fields\n"
                    "  (wall-clock ms embedded in BenchUdtMsg by the producer).\n"
                    "  Read back in Kafka offset order from offset 0 (= send order).\n"
                    "%s\n\n",
                    sep.c_str(), sepm.c_str());
                PrintStatsBlock(fRep, "Interval distribution:", sTsR, "ms",
                                (uint64_t)vUdtIntervalMs.size());
                PrintHistogram(fRep, vUdtIntervalMs, "ms");
                fprintf(fRep,
                    "  Expected interval        : %.4f ms  (%d samples/s)\n"
                    "  Non-monotonic intervals  : %" PRId64 "\n"
                    "  Anomaly threshold (ms)   : %.4f\n"
                    "  Anomalies (>threshold)   : %" PRId64 "\n\n"
                    "  Result: %s\n\n",
                    (nUdtRate > 0 ? 1000.0 / (double)nUdtRate : 0.0),
                    (int)nUdtRate,
                    nUdtNonMono,
                    dThrR,
                    nUdtAnomalies,
                    (nUdtNonMono == 0 && nUdtAnomalies == 0)
                        ? "PASSED" : "ANOMALIES DETECTED");
            }

            if (fRep != stdout) fclose(fRep);
            fprintf(stdout, "[UDT] Report written to: %s\n", sReport.c_str());
        }

        // ── Optional cleanup ───────────────────────────────────────────────────
        if (bCleanup)
        {
            fprintf(stdout, "[UDT] Cleaning up topics...\n");
            auto admC = IKv8Consumer::Create(kCfg);
            if (admC)
            {
                SessionMeta sm;
                sm.sSessionID     = sSessionID;
                sm.sSessionPrefix = sChannel + "." + sSessionID;
                sm.sLogTopic      = sChannel + "." + sSessionID + "._log_unused";
                sm.sControlTopic  = sChannel + "." + sSessionID + "._ctl_unused";
                sm.dataTopics.insert(sUdtTopic);
                admC->DeleteSessionTopics(sm);
            }
            fprintf(stdout, "[UDT] Cleanup done.\n");
        }

        return 0;
    } // end if (bUdt)

    // ── Shared state between producer (main) and consumer (thread) ───────────
    std::atomic<int64_t>  gSent{0};         // total messages enqueued
    std::atomic<int64_t>  gRecv{0};         // total messages received by consumer
    std::atomic<bool>     gProduceDone{false}; // producer finished + flushed
    std::atomic<bool>     gConsumerStop{false};// signal consumer thread to exit
    std::atomic<bool>     gResetCounters{false}; // signal consumer to reset local counters after warmup

    // Latency sample arrays -- written by a single thread, read after join.
    // Raw heap arrays: no iterator overhead, no bounds check per write.
    // nReserve: upper bound on the number of latency SAMPLES (not total messages).
    // With 1-in-nSampleRate sampling, divide the message budget accordingly.
    // When tempo is set, use it as the msg/s upper bound instead of 5M.
    const double dMaxMps = (nTempoMps > 0) ? (double)nTempoMps : 5000000.0;
    const size_t nReserve = bCountMode
                            ? (size_t)nCountArg / (size_t)nSampleRate + 1
                            : (size_t)(dDurationSec * dMaxMps) / (size_t)nSampleRate + 1;

    // Producer samples (main thread only)
    std::vector<double> vDispatchNs;      // kept as vector -- producer path is not the bottleneck
    vDispatchNs.reserve(nReserve);

    // Consumer samples (consumer thread only -- raw arrays, written by index)
    auto aProdToBrokerMs = std::unique_ptr<double[]>(new double[nReserve]);
    auto aBrokerToConsMs = std::unique_ptr<double[]>(new double[nReserve]);
    auto aE2eMs          = std::unique_ptr<double[]>(new double[nReserve]);
    std::atomic<size_t> nLatencySamples{0}; // written by consumer, read after join

    // Sequence tracking: one bit per possible sequence number.
    // After the run we scan for gaps to detect lost/stuck messages.
    // Size: max expected messages + margin.  For duration mode we estimate
    // from dMaxMps; for count mode we use nCountArg.  +1024 for headroom.
    const size_t nSeqCapacity = bCountMode
                                ? (size_t)nCountArg + 1024
                                : (size_t)(dDurationSec * dMaxMps) + 1024;
    auto aSeqSeen = std::unique_ptr<uint8_t[]>(new uint8_t[nSeqCapacity]());
    std::atomic<int64_t> nSeqDuplicates{0};
    std::atomic<int64_t> nSeqOutOfRange{0};

    std::vector<ProgressRow> vProgress;   // 1-s snapshots (main thread)

    // ── Start consumer thread ────────────────────────────────────────────────
    // Uses Subscribe/Poll so it runs concurrently with the producer.
    // The rebalance callback seeks partition 0 to OFFSET_BEGINNING on first
    // assignment, so no messages are missed even if the topic is created after
    // Subscribe() is called.
    std::thread consumerThread([&]()
    {
        auto consumer = IKv8Consumer::Create(kCfg);
        if (!consumer)
        {
            fprintf(stderr, "[BENCH] consumer thread: create failed\n");
            gConsumerStop.store(true);
            return;
        }
        consumer->Subscribe(sTopic);

        // Local counters -- no sharing with main thread during the run.
        size_t nLocalRecv    = 0;
        size_t nLocalSamples = 0;
        double *pPB = aProdToBrokerMs.get();
        double *pBC = aBrokerToConsMs.get();
        double *pE2 = aE2eMs.get();
        uint8_t *pSeq = aSeqSeen.get();
        int64_t  nLocalDups = 0;
        int64_t  nLocalOOR  = 0;

        while (!gConsumerStop.load())
        {
            // After warmup the main thread asks us to zero our local counters
            // so gRecv reflects only benchmark messages, not warmup ones.
            if (gResetCounters.load(std::memory_order_relaxed))
            {
                nLocalRecv    = 0;
                nLocalSamples = 0;
                gResetCounters.store(false, std::memory_order_relaxed);
            }

            // Capture wall clock ONCE per batch, not per message.
            // WallMs() calls QuerySystemTime on Windows (~300-500 ns each).
            // At 1 M msg/s, calling it per-message costs 300-500 ms/s --
            // the entire consumer throughput budget.
            // All messages in a single PollBatch arrive within fetch.wait.max.ms
            // (5 ms) of each other, so the per-batch timestamp is accurate to
            // within that window for B->C latency, which is ms-resolution anyway.
            const int64_t tBatchRecvMs = WallMs();

            // timeout=5 must match fetch.wait.max.ms (set in InitStreaming).
            consumer->PollBatch(50000, 5,
                [&](std::string_view /*topic*/,
                    const void *pPayload, size_t cbPayload,
                    int64_t tsKafkaMs)
                {
                    ++nLocalRecv;

                    // ── Sequence tracking (every message, not just sampled) ──
                    if (cbPayload >= sizeof(BenchMsg))
                    {
                        uint64_t seq;
                        memcpy(&seq, (const char*)pPayload + offsetof(BenchMsg, nSeq), sizeof(seq));
                        if (seq < nSeqCapacity)
                        {
                            if (pSeq[seq]) ++nLocalDups;
                            else           pSeq[seq] = 1;
                        }
                        else
                        {
                            ++nLocalOOR;
                        }
                    }

                    // Throughput is the primary metric; latency is reference.
                    // Only 1-in-nSampleRate messages pay the full latency cost
                    // (memcpy + arithmetic + 3 array writes).  The rest just
                    // increment the counter above and return immediately.
                    if ((nLocalRecv % (size_t)nSampleRate) != 0) return;

                    if (cbPayload < sizeof(BenchMsg)) return;
                    if (tsKafkaMs <= 0)               return;

                    BenchMsg msg;
                    memcpy(&msg, pPayload, sizeof(BenchMsg));

                    const double pbMs = (double)(tsKafkaMs     - msg.tSendWallMs);
                    const double bcMs = (double)(tBatchRecvMs  - tsKafkaMs);

                    if (pbMs < 0.0 || pbMs > 60000.0) return;
                    if (bcMs < 0.0 || bcMs > 60000.0) return;

                    if (nLocalSamples < nReserve)
                    {
                        pPB[nLocalSamples] = pbMs;
                        pBC[nLocalSamples] = bcMs;
                        pE2[nLocalSamples] = pbMs + bcMs;
                        ++nLocalSamples;
                    }
                });
            // Publish progress counter back to main thread after each batch.
            // One relaxed store per PollBatch call -- near-zero overhead.
            gRecv.store((int64_t)nLocalRecv, std::memory_order_relaxed);

            if (gConsumerStop.load())
            {
                break;
            }
        }

        // ── Final drain: consume any messages already in librdkafka's internal
        // fetch queue.  When gConsumerStop fires, messages that have been
        // fetched from the broker but not yet delivered to the application sit
        // in the internal consumer queue.  A few more may still be in-flight
        // in the TCP fetch pipeline (fetch.wait.max.ms = 5 ms).
        //
        // Strategy: keep polling with a 100-ms timeout for up to 2 seconds.
        // Each round that returns 0 bumps the empty counter; 3 consecutive
        // empty rounds (300 ms idle) means nothing else is coming.
        {
            const auto tFinalStart = std::chrono::steady_clock::now();
            const auto tFinalMax   = tFinalStart + std::chrono::seconds(2);
            int nConsecutiveEmpty = 0;
            for (;;)
            {
                const int got = consumer->PollBatch(50000, 100,
                    [&](std::string_view /*topic*/,
                        const void *pPayload, size_t cbPayload,
                        int64_t /*tsKafkaMs*/)
                    {
                        ++nLocalRecv;
                        if (cbPayload >= sizeof(BenchMsg))
                        {
                            uint64_t seq;
                            memcpy(&seq, (const char*)pPayload + offsetof(BenchMsg, nSeq), sizeof(seq));
                            if (seq < nSeqCapacity)
                            {
                                if (pSeq[seq]) ++nLocalDups;
                                else           pSeq[seq] = 1;
                            }
                            else { ++nLocalOOR; }
                        }
                        // Skip latency sampling during final drain -- timestamps
                        // are no longer meaningful after the benchmark timer ended.
                    });
                if (got > 0)
                {
                    nConsecutiveEmpty = 0;
                    gRecv.store((int64_t)nLocalRecv, std::memory_order_relaxed);
                }
                else
                {
                    ++nConsecutiveEmpty;
                    if (nConsecutiveEmpty >= 3) break; // 300 ms idle
                }
                if (std::chrono::steady_clock::now() >= tFinalMax) break;
            }
        }

        nLatencySamples.store(nLocalSamples, std::memory_order_relaxed);
        gRecv.store((int64_t)nLocalRecv, std::memory_order_relaxed);
        nSeqDuplicates.store(nLocalDups, std::memory_order_relaxed);
        nSeqOutOfRange.store(nLocalOOR, std::memory_order_relaxed);
        consumer->Stop();
    });

    // Give the consumer thread a moment to call Subscribe() before we create
    // the producer (avoids a race where the producer sends before the consumer
    // even registers its subscription).
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // ── Create producer ──────────────────────────────────────────────────────
    auto producer = IKv8Producer::Create(kCfg);
    if (!producer)
    {
        fprintf(stderr, "[BENCH] producer create failed\n");
        gConsumerStop.store(true);
        consumerThread.join();
        return 1;
    }

    // ── Warmup: wait for consumer partition assignment ────────────────────────
    // The consumer must complete the full JoinGroup/SyncGroup protocol and get
    // its partition assignment before the benchmark timer starts.  Otherwise
    // the first 3-4 seconds show recv=0 and the throughput numbers are skewed.
    //
    // Strategy: send a single sentinel message and busy-wait until the consumer
    // receives it.  This guarantees the consumer is fully operational.
    {
        fprintf(stdout, "[BENCH] Warming up consumer (waiting for partition assignment)...\n");
        BenchMsg warmup{};
        warmup.qSendTick   = TimerNow();
        warmup.tSendWallMs = QpcToWallMs(warmup.qSendTick);
        warmup.nSeq        = UINT64_MAX; // sentinel -- distinct from any benchmark seq
        // Keep sending the warmup message until the producer accepts it.
        while (!producer->Produce(sTopic, &warmup, sizeof(warmup),
                                  &warmup.nSeq, sizeof(warmup.nSeq)))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        producer->Flush(5000);

        // Wait until the consumer has received at least one message (the warmup).
        const auto tWarmDeadline = std::chrono::steady_clock::now()
                                 + std::chrono::seconds(30);
        while (gRecv.load(std::memory_order_relaxed) == 0)
        {
            if (std::chrono::steady_clock::now() >= tWarmDeadline)
            {
                fprintf(stderr, "[BENCH] WARNING: consumer did not receive warmup "
                                "message within 30 s -- proceeding anyway.\n");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Reset counters so warmup messages do not pollute the benchmark data.
        gSent.store(0, std::memory_order_relaxed);
        gRecv.store(0, std::memory_order_relaxed);
        // Tell the consumer thread to zero its local nLocalRecv/nLocalSamples
        // so that gRecv reflects only benchmark messages (not warmup).
        gResetCounters.store(true, std::memory_order_relaxed);
        // Wait a brief moment for the consumer to pick up the reset flag.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        fprintf(stdout, "[BENCH] Consumer ready.\n");
    }

    // ── Phase 1: Produce messages ─────────────────────────────────────────────
    fprintf(stdout, "[BENCH] Producing");
    if (bCountMode) fprintf(stdout, " %" PRIu64 " messages", nCountArg);
    else            fprintf(stdout, " for %.1f s", dDurationSec);
    if (nTempoMps > 0) fprintf(stdout, " @ %" PRId64 " msg/s", nTempoMps);
    fprintf(stdout, " (consumer running concurrently)...\n");
    fprintf(stdout, "[BENCH] %6s  %12s  %12s  %12s  %10s  %10s\n",
            "t(s)", "sent", "recv", "lag", "send Mps", "recv Mps");
    fprintf(stdout, "[BENCH] %s\n", std::string(72, '-').c_str());

    const auto tProduceStart = std::chrono::steady_clock::now();
    const auto tProduceDeadline = tProduceStart
        + std::chrono::milliseconds((int64_t)(dDurationSec * 1000.0));

    // ── Tempo rate-limiter state ──────────────────────────────────────────────
    // Simple token-bucket: at the start of every 1-ms window we grant up to
    // (nTempoMps / 1000) tokens.  Once exhausted, the producer spin-sleeps
    // until the next window opens.  When nTempoMps==0 the limiter is bypassed.
    const int64_t nTokensPerMs = (nTempoMps > 0) ? (nTempoMps / 1000) : 0;
    auto    tWindowStart  = tProduceStart;
    int64_t nWindowTokens = nTokensPerMs; // remaining tokens in current window

    auto tLastProgress = tProduceStart;
    int64_t nSentLastSec = 0;
    int64_t nRecvLastSec = 0;
    int64_t nQueueFull   = 0;
    int     nElapsedSec  = 0;

    for (;;)
    {
        // Stop condition
        if (bCountMode)
        {
            if (gSent.load(std::memory_order_relaxed) >= (int64_t)nCountArg) break;
        }
        else
        {
            if (std::chrono::steady_clock::now() >= tProduceDeadline) break;
        }

        // ── Tempo rate limiter ────────────────────────────────────────────────
        // When nTempoMps > 0, throttle the producer to the requested msg/s.
        // Tokens are replenished every 1 ms.  When the window is exhausted the
        // producer yields until the next millisecond boundary.
        if (nTokensPerMs > 0)
        {
            if (nWindowTokens <= 0)
            {
                // Spin-sleep until next 1-ms window
                auto tNextWindow = tWindowStart + std::chrono::milliseconds(1);
                while (std::chrono::steady_clock::now() < tNextWindow)
                    ; // busy-wait for sub-ms precision
                tWindowStart  = tNextWindow;
                nWindowTokens = nTokensPerMs;
            }
            --nWindowTokens;
        }

        BenchMsg msg;
        msg.qSendTick   = TimerNow();
        // Derive wall-clock ms from the QPC tick using the startup calibration.
        // Avoids a WallMs()/GetSystemTimeAsFileTime syscall per message (~400 ns
        // on Windows) while keeping the same timescale as the broker timestamp.
        msg.tSendWallMs = QpcToWallMs(msg.qSendTick);
        msg.nSeq        = (uint64_t)gSent.load(std::memory_order_relaxed);

        const uint64_t t0 = msg.qSendTick;
        const bool ok = producer->Produce(sTopic,
                                          &msg, sizeof(msg),
                                          &msg.nSeq, sizeof(msg.nSeq));
        const uint64_t t1 = TimerNow();

        if (ok)
        {
            vDispatchNs.push_back(TicksToNs(t1 - t0));
            gSent.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            ++nQueueFull;
            // Back-pressure: yield briefly to let rdkafka drain the queue
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        // Progress every ~1 s
        const auto tNow = std::chrono::steady_clock::now();
        if (tNow - tLastProgress >= std::chrono::milliseconds(1000))
        {
            ++nElapsedSec;
            const int64_t curSent = gSent.load(std::memory_order_relaxed);
            const int64_t curRecv = gRecv.load(std::memory_order_relaxed);
            const double  sMps    = (double)(curSent - nSentLastSec) / 1e6;
            const double  rMps    = (double)(curRecv - nRecvLastSec) / 1e6;
            const int64_t lag     = curSent - curRecv;

            fprintf(stdout, "[BENCH] %6d  %12" PRId64 "  %12" PRId64
                            "  %12" PRId64 "  %10.3f  %10.3f\n",
                    nElapsedSec, curSent, curRecv, lag, sMps, rMps);

            ProgressRow pr{};
            pr.tSec        = nElapsedSec;
            pr.nSent       = curSent;
            pr.nRecv       = curRecv;
            pr.nQueueFull  = nQueueFull;
            pr.sendRateMps = sMps;
            pr.recvRateMps = rMps;
            vProgress.push_back(pr);

            nSentLastSec = curSent;
            nRecvLastSec = curRecv;
            tLastProgress = tNow;
        }
    }

    const auto tProduceEnd  = std::chrono::steady_clock::now();
    const double dProduceTotalMs =
        (double)std::chrono::duration_cast<std::chrono::microseconds>(
            tProduceEnd - tProduceStart).count() / 1000.0;

    const int64_t nTotalSent = gSent.load();
    fprintf(stdout, "[BENCH] %s\n", std::string(72, '-').c_str());
    fprintf(stdout, "[BENCH] Produced %" PRId64 " messages in %.1f ms, flushing...\n",
            nTotalSent, dProduceTotalMs);
    producer->Flush(60000);
    gProduceDone.store(true);
    const int64_t nDrFail = producer->GetDeliveryFailures();
    const int64_t nDrOk   = producer->GetDeliverySuccess();
    fprintf(stdout, "[BENCH] Flush complete. delivered=%" PRId64
                    "  dr-failures=%" PRId64 "\n", nDrOk, nDrFail);
    fprintf(stdout, "[BENCH] Waiting for consumer to drain...\n");

    // ── Wait for consumer to catch up ────────────────────────────────────────
    // The producer has flushed -- all messages are now stored in the Kafka
    // broker.  The real-time consumer may still lag behind, though: at high
    // throughput, the consumer's fetch pipeline can be several seconds behind
    // the broker head offset.
    //
    // Strategy: keep waiting as long as the consumer is making progress (recv
    // counter is still increasing).  Abort only when:
    //   (a) all messages have been received, OR
    //   (b) the consumer stalls (no new messages for 3 consecutive seconds), OR
    //   (c) a hard 30-second deadline is reached.
    //
    // This replaces the previous fixed 3-second timeout which was too short
    // for high-throughput runs and caused the consumer to be killed while it
    // was still actively ingesting messages.
    {
        const auto tDrainStart    = std::chrono::steady_clock::now();
        const auto tDrainHardMax  = tDrainStart + std::chrono::seconds(30);
        const int  nStallLimitMs  = 3000; // declare stall after 3 s of no progress

        int64_t nRecvLastProgress = gRecv.load(std::memory_order_relaxed);
        auto    tLastProgress     = tDrainStart;
        int64_t nRecvLastPrint    = -1;

        while (true)
        {
            const int64_t curRecv = gRecv.load(std::memory_order_relaxed);
            const auto    tNow    = std::chrono::steady_clock::now();

            // (a) All messages received.
            if (curRecv >= nTotalSent)
            {
                fprintf(stdout, "[BENCH] Consumer fully caught up.\n");
                break;
            }

            // (b) Stall detection: no new messages for nStallLimitMs.
            if (curRecv > nRecvLastProgress)
            {
                nRecvLastProgress = curRecv;
                tLastProgress     = tNow;
            }
            else
            {
                const int64_t stallMs = std::chrono::duration_cast<
                    std::chrono::milliseconds>(tNow - tLastProgress).count();
                if (stallMs >= nStallLimitMs)
                {
                    fprintf(stdout, "[BENCH] Consumer stalled for %d ms with lag=%" PRId64
                                    " -- stopping drain.\n",
                            (int)stallMs, nTotalSent - curRecv);
                    break;
                }
            }

            // (c) Hard deadline.
            if (tNow >= tDrainHardMax)
            {
                fprintf(stdout, "[BENCH] Drain hard deadline (30 s) reached with lag=%" PRId64
                                " -- stopping.\n",
                        nTotalSent - curRecv);
                break;
            }

            // Progress log (every time recv changes, at most every 200 ms).
            if (curRecv != nRecvLastPrint)
            {
                const double dDrainSec = std::chrono::duration_cast<
                    std::chrono::milliseconds>(tNow - tDrainStart).count() / 1000.0;
                fprintf(stdout, "[BENCH] draining %.1f s ... recv=%" PRId64 " / %" PRId64
                                " (lag=%" PRId64 ")\n",
                        dDrainSec, curRecv, nTotalSent, nTotalSent - curRecv);
                nRecvLastPrint = curRecv;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    gConsumerStop.store(true);
    consumerThread.join();

    const int64_t nTotalRecv = gRecv.load();
    fprintf(stdout, "[BENCH] Consumer done. Received %" PRId64 " / %" PRId64 "\n",
            nTotalRecv, nTotalSent);

    // ── Sequence continuity analysis ─────────────────────────────────────────
    {
        const int64_t nExpected = nTotalSent; // seq 0..nTotalSent-1
        int64_t nMissing  = 0;
        int64_t nFirstGap = -1;
        int64_t nLastGap  = -1;
        const size_t nScanEnd = (size_t)nExpected < nSeqCapacity
                                ? (size_t)nExpected : nSeqCapacity;
        for (size_t i = 0; i < nScanEnd; ++i)
        {
            if (!aSeqSeen[i])
            {
                ++nMissing;
                if (nFirstGap < 0) nFirstGap = (int64_t)i;
                nLastGap = (int64_t)i;
            }
        }
        // Check for warmup message (seq 0 from the warmup phase -- it was
        // produced before the benchmark started, so the consumer saw it but
        // the reset zeroed nLocalRecv; the seq bit may or may not be set
        // depending on timing of the reset vs. the warmup delivery).
        const bool bWarmupSeqSeen = (nScanEnd > 0 && aSeqSeen[0]);

        fprintf(stdout, "[BENCH] Sequence check: expected=%" PRId64
                        "  missing=%" PRId64 "  duplicates=%" PRId64
                        "  out-of-range=%" PRId64 "\n",
                nExpected, nMissing,
                nSeqDuplicates.load(), nSeqOutOfRange.load());
        if (bWarmupSeqSeen)
            fprintf(stdout, "[BENCH]   Note: seq 0 was seen (warmup message, "
                            "counted in recv but not in sent).\n");
        if (nMissing > 0)
            fprintf(stdout, "[BENCH]   First missing seq=%" PRId64
                            "  last missing seq=%" PRId64 "\n",
                    nFirstGap, nLastGap);
        if (nMissing == 0 && nSeqDuplicates.load() == 0 && nSeqOutOfRange.load() == 0)
            fprintf(stdout, "[BENCH]   All sequences accounted for -- no loss.\n");
    }

    // ── Verification consumer pass (independent read from offset 0) ─────────
    // After the benchmark, create a SECOND consumer that reads the entire topic
    // from the beginning and independently verifies that every BenchMsg.nSeq
    // from 0..nTotalSent-1 is present exactly once.  This catches messages the
    // concurrent consumer might have missed due to rebalance timing, consumer
    // lag, or transient issues.
    int64_t nVerifyRecv     = 0;
    int64_t nVerifyMissing  = 0;
    int64_t nVerifyDups     = 0;
    int64_t nVerifyOOR      = 0;
    int64_t nVerifyFirstGap = -1;
    int64_t nVerifyLastGap  = -1;
    bool    bVerifyRan      = false;
    // Timestamp regularity analysis results (populated by verification pass)
    std::vector<double> vTsIntervalUs;   // consecutive inter-sample intervals (us)
    int64_t nTsNonMono   = 0;           // non-monotonic payload timestamps
    int64_t nTsAnomalies = 0;           // intervals exceeding anomaly threshold
    bool    bTsAnalysisRan = false;
    {
        fprintf(stdout, "\n[BENCH] %s\n", std::string(72, '=').c_str());
        fprintf(stdout, "[BENCH] Post-session Kafka gap check\n");
        fprintf(stdout, "[BENCH] %s\n", std::string(72, '-').c_str());
        fprintf(stdout, "[BENCH] The real-time benchmark session is over. A second, independent\n"
                        "[BENCH] consumer now re-reads every message stored in Kafka topic\n"
                        "[BENCH] '%s' from offset 0 and checks that all sequence\n"
                        "[BENCH] numbers 0..%" PRId64 " are present exactly once.\n",
                sTopic.c_str(), nTotalSent - 1);
        fprintf(stdout, "[BENCH] Reading topic from beginning (timeout 30 s)...\n");

        auto verifier = IKv8Consumer::Create(kCfg);
        if (!verifier)
        {
            fprintf(stderr, "[BENCH] WARNING: could not create verification consumer.\n");
        }
        else
        {
            bVerifyRan = true;
            auto aVerifySeq  = std::unique_ptr<uint8_t[]>(new uint8_t[nSeqCapacity]());
            auto aVerifyTick = std::unique_ptr<uint64_t[]>(new uint64_t[nSeqCapacity]());
            int64_t localRecv = 0;
            int64_t localDups = 0;
            int64_t localOOR  = 0;
            int64_t localWarmup = 0;
            uint8_t  *pV = aVerifySeq.get();
            uint64_t *pT = aVerifyTick.get();

            verifier->ConsumeTopicFromBeginning(sTopic, 30000,
                [&](const void *pPayload, size_t cbPayload, int64_t /*tsKafkaMs*/)
                {
                    if (cbPayload < sizeof(BenchMsg)) return;
                    uint64_t seq;
                    memcpy(&seq, (const char*)pPayload + offsetof(BenchMsg, nSeq),
                           sizeof(seq));
                    if (seq == UINT64_MAX) { ++localWarmup; return; } // skip warmup sentinel
                    ++localRecv;
                    if (seq < nSeqCapacity)
                    {
                        if (pV[seq])
                            ++localDups;
                        else
                        {
                            pV[seq] = 1;
                            uint64_t tick;
                            memcpy(&tick, (const char*)pPayload
                                   + offsetof(BenchMsg, qSendTick), sizeof(tick));
                            pT[seq] = tick;
                        }
                    }
                    else
                    {
                        ++localOOR;
                    }
                });

            // Scan for missing sequences.
            const size_t nScanEnd = (size_t)nTotalSent < nSeqCapacity
                                    ? (size_t)nTotalSent : nSeqCapacity;
            int64_t localMissing = 0;
            for (size_t i = 0; i < nScanEnd; ++i)
            {
                if (!pV[i])
                {
                    ++localMissing;
                    if (nVerifyFirstGap < 0) nVerifyFirstGap = (int64_t)i;
                    nVerifyLastGap = (int64_t)i;
                }
            }

            nVerifyRecv    = localRecv;
            nVerifyMissing = localMissing;
            nVerifyDups    = localDups;
            nVerifyOOR     = localOOR;

            const double dLossRate = (nTotalSent > 0)
                ? (double)nVerifyMissing / (double)nTotalSent * 100.0 : 0.0;

            fprintf(stdout, "[BENCH]\n");
            fprintf(stdout, "[BENCH] Kafka gap check results:\n");
            fprintf(stdout, "[BENCH]   Topic               : %s\n", sTopic.c_str());
            fprintf(stdout, "[BENCH]   Messages produced    : %" PRId64 "\n", nTotalSent);
            fprintf(stdout, "[BENCH]   Messages read back   : %" PRId64
                            "  (+ %" PRId64 " warmup sentinel(s) skipped)\n",
                    nVerifyRecv, localWarmup);
            fprintf(stdout, "[BENCH]   Unique sequences seen: %" PRId64 " / %" PRId64 "\n",
                    nTotalSent - nVerifyMissing, nTotalSent);
            fprintf(stdout, "[BENCH]   Missing sequences    : %" PRId64, nVerifyMissing);
            if (nVerifyMissing > 0)
                fprintf(stdout, "  (%.4f%% loss)", dLossRate);
            fprintf(stdout, "\n");
            fprintf(stdout, "[BENCH]   Duplicate sequences  : %" PRId64 "\n", nVerifyDups);
            fprintf(stdout, "[BENCH]   Out-of-range seqs    : %" PRId64 "\n", nVerifyOOR);
            if (nVerifyMissing > 0)
                fprintf(stdout, "[BENCH]   First missing seq    : %" PRId64 "\n"
                                "[BENCH]   Last  missing seq    : %" PRId64 "\n",
                        nVerifyFirstGap, nVerifyLastGap);
            fprintf(stdout, "[BENCH]\n");
            if (nVerifyMissing == 0 && nVerifyDups == 0 && nVerifyOOR == 0)
                fprintf(stdout, "[BENCH]   >>> PASSED -- every produced sequence was found in Kafka. No gaps.\n");
            else
                fprintf(stdout, "[BENCH]   >>> FAILED -- Kafka topic has integrity issues "
                                "(gaps, duplicates, or out-of-range).\n");
            fprintf(stdout, "[BENCH] %s\n", std::string(72, '=').c_str());

            // -- Telemetry payload timestamp regularity analysis --------
            // Compute inter-sample intervals from BenchMsg.qSendTick (QPC
            // tick captured at the Add() call, before the Kafka produce).
            // Only consecutive valid sequence pairs contribute an interval;
            // gaps caused by missing sequences are excluded.
            if (nTotalSent > 1)
            {
                fprintf(stdout, "\n[BENCH] %s\n", std::string(72, '=').c_str());
                fprintf(stdout, "[BENCH] Telemetry payload timestamp regularity\n");
                fprintf(stdout, "[BENCH] %s\n", std::string(72, '-').c_str());
                fprintf(stdout, "[BENCH] Analyzing inter-sample intervals from payload QPC ticks\n"
                                "[BENCH] (captured at the Add() call, before Kafka produce).\n");

                vTsIntervalUs.reserve((size_t)(nTotalSent - 1));
                int64_t localNonMono = 0;
                int64_t prevIdx = -1;
                for (int64_t i = 0; i < (int64_t)nScanEnd; ++i)
                {
                    if (!pV[i]) continue;
                    if (prevIdx >= 0 && i == prevIdx + 1)
                    {
                        if (pT[i] >= pT[prevIdx])
                        {
                            double dtUs = TicksToNs(pT[i] - pT[prevIdx]) / 1000.0;
                            vTsIntervalUs.push_back(dtUs);
                        }
                        else
                        {
                            ++localNonMono;
                            double dtUs = -(double)TicksToNs(pT[prevIdx] - pT[i]) / 1000.0;
                            vTsIntervalUs.push_back(dtUs);
                        }
                    }
                    prevIdx = i;
                }

                nTsNonMono = localNonMono;
                bTsAnalysisRan = true;

                if (!vTsIntervalUs.empty())
                {
                    const size_t N = vTsIntervalUs.size();
                    // Single-pass min/max/mean
                    double dMin = vTsIntervalUs[0];
                    double dMax = vTsIntervalUs[0];
                    double dSum = 0.0;
                    for (double v : vTsIntervalUs)
                    {
                        if (v < dMin) dMin = v;
                        if (v > dMax) dMax = v;
                        dSum += v;
                    }
                    const double dMean = dSum / (double)N;

                    // Median via nth_element on a temporary copy (O(n))
                    std::vector<double> vTmp(vTsIntervalUs);
                    const size_t nMid = N / 2;
                    std::nth_element(vTmp.begin(),
                                     vTmp.begin() + (ptrdiff_t)nMid,
                                     vTmp.end());
                    const double dMedian = vTmp[nMid];

                    // Anomaly detection: intervals > max(mean*50, 5000 us)
                    const double dMT = dMean * 50.0;
                    const double dThreshold = (dMT > 5000.0) ? dMT : 5000.0;
                    int64_t localAnomalies = 0;
                    for (double v : vTsIntervalUs)
                    {
                        if (v > dThreshold) ++localAnomalies;
                    }
                    nTsAnomalies = localAnomalies;

                    const bool bTsOk = (nTsNonMono == 0 && nTsAnomalies == 0);

                    fprintf(stdout, "[BENCH]\n");
                    fprintf(stdout, "[BENCH] Interval statistics (us):\n");
                    fprintf(stdout, "[BENCH]   Consecutive pairs    : %zu\n", N);
                    fprintf(stdout, "[BENCH]   Min interval         : %10.3f us\n", dMin);
                    fprintf(stdout, "[BENCH]   Median interval      : %10.3f us\n", dMedian);
                    fprintf(stdout, "[BENCH]   Mean interval        : %10.3f us\n", dMean);
                    fprintf(stdout, "[BENCH]   Max interval         : %10.3f us\n", dMax);
                    fprintf(stdout, "[BENCH]   Non-monotonic        : %" PRId64 "\n", nTsNonMono);
                    fprintf(stdout, "[BENCH]   Anomaly threshold    : %10.1f us\n", dThreshold);
                    fprintf(stdout, "[BENCH]   Anomalies            : %" PRId64 "\n", nTsAnomalies);
                    fprintf(stdout, "[BENCH]\n");
                    if (bTsOk)
                        fprintf(stdout, "[BENCH]   >>> PASSED -- payload timestamps monotonic and regular.\n");
                    else
                        fprintf(stdout, "[BENCH]   >>> ANOMALIES DETECTED -- see report for details.\n");
                }
                else
                {
                    fprintf(stdout, "[BENCH] Not enough consecutive sequences for interval analysis.\n");
                }
                fprintf(stdout, "[BENCH] %s\n", std::string(72, '=').c_str());
            }
        }
    }

    const size_t nSamples = nLatencySamples.load();
    if (vDispatchNs.empty() || nSamples == 0)
    {
        fprintf(stderr, "[BENCH] No samples collected -- nothing to report.\n");
        return 1;
    }

    // Wrap raw arrays in vectors for the Stats/Histogram functions (sort in-place, read-only after).
    std::vector<double> vProdToBrokerMs(aProdToBrokerMs.get(), aProdToBrokerMs.get() + nSamples);
    std::vector<double> vBrokerToConsMs(aBrokerToConsMs.get(), aBrokerToConsMs.get() + nSamples);
    std::vector<double> vE2eMs         (aE2eMs.get(),          aE2eMs.get()          + nSamples);

    // ── Compute statistics ───────────────────────────────────────────────────
    Stats sDispatch = ComputeStats(vDispatchNs);
    Stats sProdBrkr = ComputeStats(vProdToBrokerMs);
    Stats sBrkrCons = ComputeStats(vBrokerToConsMs);
    Stats sE2e      = ComputeStats(vE2eMs);

    // ── Write report ─────────────────────────────────────────────────────────
    FILE *fReport = fopen(sReport.c_str(), "w");
    if (!fReport)
    {
        fprintf(stderr, "[BENCH] Cannot open report file '%s'\n", sReport.c_str());
        fReport = stdout;
    }

    const std::string sep70(70, '=');
    const std::string sep70d(70, '-');

    // Header
    fprintf(fReport,
        "%s\n"
        "  kv8bench -- Latency Benchmark Report\n"
        "%s\n\n"
        "  Timestamp     : %s\n"
        "  Brokers       : %s\n"
        "  Topic         : %s\n"
        "  Mode          : %s\n"
        "  Messages sent : %" PRId64 "\n"
        "  Messages recv : %" PRId64 "\n"
        "  Lost          : %" PRId64 "\n"
        "  Queue-full    : %" PRId64 "  (Produce() back-pressure events)\n"
        "  Samples valid : %zu  (1-in-%d sample rate)\n"
        "  Produce time  : %.1f ms\n"

        "  Linger cfg    : %d ms  (library-default may differ)\n"
        "  Batch cfg     : %d     (library-default may differ)\n"
        "  Partitions    : %d\n"
        "  Tempo         : %s\n\n"
        "  Clock notes:\n"
        "    Dispatch   -- QPC/CLOCK_MONOTONIC (ns resolution, same-host)\n"
        "    P->B, B->C -- system_clock wall ms vs Kafka broker timestamp ms\n"
        "                  Valid because producer and consumer share the same host.\n"
        "    Consumer runs concurrently with producer (Subscribe/Poll thread).\n\n",
        sep70.c_str(), sep70.c_str(),
        NowUTC().c_str(),
        sBrokers.c_str(),
        sTopic.c_str(),
        bCountMode ? "count" : "duration",
        nTotalSent,
        nTotalRecv,
        nTotalSent - nTotalRecv,
        nQueueFull,
        vE2eMs.size(), nSampleRate,
        dProduceTotalMs,
        nLingerMs,
        nBatch,
        nPartitions,
        (nTempoMps > 0) ? (std::to_string(nTempoMps) + " msg/s").c_str() : "unlimited");

    // Per-second throughput and lag table
    fprintf(fReport, "%s\n  Per-Second Throughput and Lag\n%s\n\n", sep70.c_str(), sep70d.c_str());
    fprintf(fReport, "  %6s  %12s  %12s  %12s  %10s  %10s\n",
            "t(s)", "sent", "recv", "lag", "send Mps", "recv Mps");
    fprintf(fReport, "  %s\n", std::string(68, '-').c_str());
    for (auto &pr : vProgress)
    {
        fprintf(fReport,
                "  %6d  %12" PRId64 "  %12" PRId64 "  %12" PRId64
                "  %10.3f  %10.3f\n",
                pr.tSec, pr.nSent, pr.nRecv,
                pr.nSent - pr.nRecv,
                pr.sendRateMps, pr.recvRateMps);
    }
    // Final row (after drain)
    fprintf(fReport,
            "  %6s  %12" PRId64 "  %12" PRId64 "  %12" PRId64 "  %10s  %10s\n\n",
            "final", nTotalSent, nTotalRecv, nTotalSent - nTotalRecv, "-", "-");

    // 1. Dispatch latency
    fprintf(fReport, "%s\n  [1] Dispatch Latency  (Produce() call duration, QPC)\n%s\n\n",
            sep70.c_str(), sep70d.c_str());
    PrintStatsBlock(fReport, "Percentile table:", sDispatch, "ns", (uint64_t)vDispatchNs.size());
    PrintHistogram (fReport, vDispatchNs, "ns");

    // 2. Producer -> Broker latency
    fprintf(fReport, "%s\n  [2] Producer -> Broker Latency  (send wall clock -> broker timestamp)\n%s\n\n",
            sep70.c_str(), sep70d.c_str());
    PrintStatsBlock(fReport, "Percentile table:", sProdBrkr, "ms", (uint64_t)vProdToBrokerMs.size());
    PrintHistogram (fReport, vProdToBrokerMs, "ms");

    // 3. Broker -> Consumer latency
    fprintf(fReport, "%s\n  [3] Broker -> Consumer Latency  (broker timestamp -> consumer wall clock)\n%s\n\n",
            sep70.c_str(), sep70d.c_str());
    PrintStatsBlock(fReport, "Percentile table:", sBrkrCons, "ms", (uint64_t)vBrokerToConsMs.size());
    PrintHistogram (fReport, vBrokerToConsMs, "ms");

    // 4. End-to-end latency
    fprintf(fReport, "%s\n  [4] End-to-End Latency  (= [2]+[3], send wall clock -> consumer wall clock)\n%s\n\n",
            sep70.c_str(), sep70d.c_str());
    PrintStatsBlock(fReport, "Percentile table:", sE2e, "ms", (uint64_t)vE2eMs.size());
    PrintHistogram (fReport, vE2eMs, "ms");

    // 5. Throughput summary
    const double dThroughputMsgps = (dProduceTotalMs > 0.0)
                              ? (double)nTotalSent / (dProduceTotalMs / 1000.0)
                              : 0.0;
    const double dPayloadMBps = dThroughputMsgps * sizeof(BenchMsg) / 1e6;

    fprintf(fReport,
        "%s\n  [5] Throughput Summary\n%s\n\n"
        "  %-28s  %12.0f msg/s\n"
        "  %-28s  %12.3f MB/s  (payload only, %zu bytes/msg)\n"
        "  %-28s  %12.3f ms\n"
        "  %-28s  %12" PRId64 " events\n\n",
        sep70.c_str(), sep70d.c_str(),
        "Produce throughput",   dThroughputMsgps,
        "Produce payload rate", dPayloadMBps, sizeof(BenchMsg),
        "Total produce time",   dProduceTotalMs,
        "Queue-full events",    nQueueFull);

    // 6. Post-session Kafka gap check
    if (bVerifyRan)
    {
        const double dLossRate = (nTotalSent > 0)
            ? (double)nVerifyMissing / (double)nTotalSent * 100.0 : 0.0;
        const bool bPassed = (nVerifyMissing == 0 && nVerifyDups == 0 && nVerifyOOR == 0);

        fprintf(fReport,
            "%s\n"
            "  [6] Post-Session Kafka Gap Check\n"
            "\n"
            "  After the real-time benchmark session ended, an independent consumer\n"
            "  re-read every Kafka record in the topic from offset 0 and verified\n"
            "  that all sequence numbers 0..N-1 are present exactly once.\n"
            "%s\n\n",
            sep70.c_str(), sep70d.c_str());
        fprintf(fReport,
            "  %-28s  %s\n"
            "  %-28s  %12" PRId64 "\n"
            "  %-28s  %12" PRId64 "\n"
            "  %-28s  %12" PRId64 " / %" PRId64 "\n"
            "  %-28s  %12" PRId64,
            "Topic",                sTopic.c_str(),
            "Messages produced",    nTotalSent,
            "Messages read back",   nVerifyRecv,
            "Unique sequences seen", nTotalSent - nVerifyMissing, nTotalSent,
            "Missing sequences",    nVerifyMissing);
        if (nVerifyMissing > 0)
            fprintf(fReport, "  (%.4f%% loss)", dLossRate);
        fprintf(fReport, "\n"
            "  %-28s  %12" PRId64 "\n"
            "  %-28s  %12" PRId64 "\n",
            "Duplicate sequences",  nVerifyDups,
            "Out-of-range seqs",    nVerifyOOR);
        if (nVerifyMissing > 0)
            fprintf(fReport,
                "  First missing seq     = %" PRId64 "\n"
                "  Last  missing seq     = %" PRId64 "\n",
                nVerifyFirstGap, nVerifyLastGap);
        fprintf(fReport, "\n  Result: %s\n\n", bPassed ? "PASSED" : "FAILED");
    }

    // 7. Telemetry payload timestamp regularity
    if (bTsAnalysisRan && !vTsIntervalUs.empty())
    {
        Stats sTs = ComputeStats(vTsIntervalUs);
        fprintf(fReport,
            "%s\n"
            "  [7] Telemetry Payload Timestamp Regularity\n"
            "\n"
            "  Inter-sample intervals computed from consecutive BenchMsg.qSendTick\n"
            "  values (QPC/CLOCK_MONOTONIC, captured at the Add() call before Kafka\n"
            "  produce).  Sorted by sequence number; only consecutive valid pairs\n"
            "  are included (gaps from missing sequences are excluded).\n"
            "%s\n\n",
            sep70.c_str(), sep70d.c_str());
        PrintStatsBlock(fReport, "Interval distribution:", sTs, "us",
                        (uint64_t)vTsIntervalUs.size());
        PrintHistogram(fReport, vTsIntervalUs, "us");
        fprintf(fReport,
            "  Monotonicity check:\n"
            "    Non-monotonic intervals  : %" PRId64 "\n"
            "    Anomaly threshold (us)   : %.1f\n"
            "    Anomalies (>threshold)   : %" PRId64 "\n\n"
            "  Result: %s\n\n",
            nTsNonMono,
            (vTsIntervalUs.empty() ? 0.0
                : ((sTs.dMean * 50.0 > 5000.0) ? sTs.dMean * 50.0 : 5000.0)),
            nTsAnomalies,
            (nTsNonMono == 0 && nTsAnomalies == 0) ? "PASSED" : "ANOMALIES DETECTED");
    }

    if (fReport != stdout) fclose(fReport);

    // ── Print summary to stdout ───────────────────────────────────────────────
    fprintf(stdout,
        "\n[BENCH] ---------- Summary ----------\n"
        "[BENCH] Dispatch  p50 = %7.1f ns    p99 = %7.1f ns\n"
        "[BENCH] P->Broker p50 = %7.3f ms    p99 = %7.3f ms\n"
        "[BENCH] B->Cons   p50 = %7.3f ms    p99 = %7.3f ms\n"
        "[BENCH] E2E       p50 = %7.3f ms    p99 = %7.3f ms\n"
        "[BENCH] Throughput    = %.0f msg/s\n"
        "[BENCH] Queue-full    = %" PRId64 " events\n",
        sDispatch.dP50, sDispatch.dP99,
        sProdBrkr.dP50, sProdBrkr.dP99,
        sBrkrCons.dP50, sBrkrCons.dP99,
        sE2e.dP50,      sE2e.dP99,
        dThroughputMsgps,
        nQueueFull);
    if (bVerifyRan)
    {
        const bool bPassed = (nVerifyMissing == 0 && nVerifyDups == 0 && nVerifyOOR == 0);
        const double dLossRate = (nTotalSent > 0)
            ? (double)nVerifyMissing / (double)nTotalSent * 100.0 : 0.0;
        fprintf(stdout,
            "[BENCH] Kafka gap check : %s  (read %" PRId64 " records, "
            "missing=%" PRId64 "  dups=%" PRId64 "  oor=%" PRId64,
            bPassed ? "PASSED" : "FAILED",
            nVerifyRecv, nVerifyMissing, nVerifyDups, nVerifyOOR);
        if (nVerifyMissing > 0)
            fprintf(stdout, "  loss=%.4f%%", dLossRate);
        fprintf(stdout, ")\n");
    }
    if (bTsAnalysisRan && !vTsIntervalUs.empty())
    {
        fprintf(stdout,
            "[BENCH] Timestamp chk : %s  (intervals=%zu  non-mono=%" PRId64
            "  anomalies=%" PRId64 ")\n",
            (nTsNonMono == 0 && nTsAnomalies == 0) ? "PASSED" : "ANOMALIES",
            vTsIntervalUs.size(), nTsNonMono, nTsAnomalies);
    }
    fprintf(stdout, "[BENCH] Report written to: %s\n", sReport.c_str());

    // ── Optional cleanup ──────────────────────────────────────────────────────
    if (bCleanup)
    {
        fprintf(stdout, "[BENCH] Cleaning up topic '%s'...\n", sTopic.c_str());
        SessionMeta sm;
        sm.sSessionID     = sTopic;
        sm.sSessionPrefix = sTopic;
        sm.sLogTopic      = sTopic + ".__log_unused";
        sm.sControlTopic  = sTopic + ".__ctrl_unused";
        sm.dataTopics.insert(sTopic);
        auto consumer = IKv8Consumer::Create(kCfg);
        if (consumer) consumer->DeleteSessionTopics(sm);
        fprintf(stdout, "[BENCH] Cleanup done.\n");
    }

    return 0;
}
