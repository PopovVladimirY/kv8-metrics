////////////////////////////////////////////////////////////////////////////////
// kv8probe -- Kafka timestamp integrity probe (producer side)
//
// Writes N samples of Kv8TelValue directly to Kafka via IKv8Producer.
//
// DETERMINISTIC SYNTHETIC CLOCK -- no real timer is used.
//   qwTimer[i] = PROBE_START_TICK + i * PROBE_TICK_INTERVAL
//   dbValue[i] = i & 0x3FF              (rolling 0..1023)
//   wSeqN[i]   = i & 0xFFFF
//
// Session registration:
//   Three KafkaRegistryRecord entries are written to <channel>._registry
//   BEFORE any data is sent:
//     KV8_CID_LOG     -- announces the session (name + log topic)
//     KV8_CID_GROUP   -- announces the KV8 group (timer anchors, hash, group topic)
//     counter record  -- describes the synthetic counter (name, wID, data topic)
//
//   This makes the session immediately visible to kv8maint, kv8scope, and
//   kv8cli via IKv8Consumer::DiscoverSessions().
//
// Topic layout (per-counter, matching kv8bridge output):
//   group topic : <channel>.<sessionID>.d.<XXXXXXXX>        (groups only, no data)
//   data topic  : <channel>.<sessionID>.d.<XXXXXXXX>.<CCCC> (carries Kv8TelValue)
//
//   XXXXXXXX = 8-hex FNV-32 of the channel name
//   CCCC     = 4-hex counter wID
//
// Usage:
//   kv8probe --channel <name> [options]
////////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string>
#include <vector>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <kv8/IKv8Producer.h>
#include <kv8/IKv8Consumer.h>
#include <kv8/Kv8Types.h>

#include <kv8util/Kv8AppUtils.h>
#include <kv8util/Kv8TopicUtils.h>

using namespace kv8;

////////////////////////////////////////////////////////////////////////////////
// Synthetic clock parameters (compile-time constants).
// kv8verify reads frequency, start tick, and interval from the registry
// GROUP record, so changing these here is sufficient.
////////////////////////////////////////////////////////////////////////////////
static const uint64_t PROBE_FREQUENCY     = 1000000ULL; // 1 MHz synthetic clock (1 tick = 1 us)
static const uint64_t PROBE_START_TICK    = 1000000ULL; // anchor: t=1 s (arbitrary, non-zero)
static const uint64_t PROBE_TICK_INTERVAL = 230ULL;     // 230 us between consecutive samples

////////////////////////////////////////////////////////////////////////////////
// ProbeManifest -- written once to <dataTopic>._manifest with key "manifest".
// kv8verify --topic mode reads this to reconstruct expected qwTimer values.
////////////////////////////////////////////////////////////////////////////////
#pragma pack(push, 2)

struct ProbeManifest
{
    uint64_t qwFrequency;     // synthetic ticks per second
    uint64_t qwStartTick;     // qwTimer of sample index 0
    uint64_t qwTickInterval;  // ticks between consecutive samples
    uint64_t qwTotalSamples;  // total samples to expect
    uint16_t wID;             // counter wID used for all samples
    uint16_t wPad[3];         // reserved = 0
};

#pragma pack(pop)

// Extension header: type=2 (TEL_V2), subtype=2 (VALUE), size in bits [31:10]
static inline uint32_t MakeExtHeader(uint32_t size)
{
    return (2u) | (2u << 5) | (size << 10);
}

////////////////////////////////////////////////////////////////////////////////
// FNV-1a 32-bit hash of a channel name (for topic naming and registry keys).
////////////////////////////////////////////////////////////////////////////////
static uint32_t Fnv32(const std::string &s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

////////////////////////////////////////////////////////////////////////////////
// Get current FILETIME (100-ns ticks since 1601-01-01).
////////////////////////////////////////////////////////////////////////////////
static void GetCurrentFiletime(uint32_t &dwHi, uint32_t &dwLo)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    dwHi = ft.dwHighDateTime;
    dwLo = ft.dwLowDateTime;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    // Offset from 1601-01-01 to 1970-01-01: 116444736000000000 100-ns ticks.
    uint64_t ft = (uint64_t)ts.tv_sec  * 10000000ULL
                + (uint64_t)ts.tv_nsec / 100ULL
                + 116444736000000000ULL;
    dwHi = (uint32_t)(ft >> 32);
    dwLo = (uint32_t)(ft);
#endif
}

////////////////////////////////////////////////////////////////////////////////
// Write one KafkaRegistryRecord (fixed header + name + topic) to regTopic.
////////////////////////////////////////////////////////////////////////////////
static void WriteRegistryRecord(IKv8Producer       *p,
                                const std::string  &regTopic,
                                uint32_t            dwHash,
                                uint16_t            wCounterID,
                                uint16_t            wFlags,
                                double              dbMin,
                                double              dbAlarmMin,
                                double              dbMax,
                                double              dbAlarmMax,
                                uint64_t            qwTimerFrequency,
                                uint64_t            qwTimerValue,
                                uint32_t            dwTimeHi,
                                uint32_t            dwTimeLo,
                                const std::string  &sName,
                                const std::string  &sTopic)
{
    KafkaRegistryRecord rec{};
    rec.dwHash           = dwHash;
    rec.wCounterID       = wCounterID;
    rec.wFlags           = wFlags;
    rec.dbMin            = dbMin;
    rec.dbAlarmMin       = dbAlarmMin;
    rec.dbMax            = dbMax;
    rec.dbAlarmMax       = dbAlarmMax;
    rec.wNameLen         = (uint16_t)sName.size();
    rec.wTopicLen        = (uint16_t)sTopic.size();
    rec.wVersion         = KV8_REGISTRY_VERSION;
    rec.wPad             = 0;
    rec.qwTimerFrequency = qwTimerFrequency;
    rec.qwTimerValue     = qwTimerValue;
    rec.dwTimeHi         = dwTimeHi;
    rec.dwTimeLo         = dwTimeLo;

    // Payload: fixed header followed immediately by name bytes then topic bytes.
    std::vector<uint8_t> payload(sizeof(rec) + sName.size() + sTopic.size());
    memcpy(payload.data(),                             &rec,           sizeof(rec));
    memcpy(payload.data() + sizeof(rec),               sName.data(),  sName.size());
    memcpy(payload.data() + sizeof(rec) + sName.size(), sTopic.data(), sTopic.size());

    p->Produce(regTopic, payload.data(), payload.size(), nullptr, 0);
}

////////////////////////////////////////////////////////////////////////////////
// main
////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::string sBrokers     = "localhost:19092";
    std::string sChannel     = "kv8.probe";      // channel prefix (dots, no trailing dot)
    std::string sSessionName = "kv8probe";       // human-readable session name
    std::string sCounterName = "synthetic";      // human-readable counter name
    std::string sSecProto    = "sasl_plaintext";
    std::string sSaslMech    = "PLAIN";
    std::string sUser        = "kv8producer";
    std::string sPass        = "kv8secret";
    uint64_t    nCount       = 1000;
    uint64_t    qwStartTick  = PROBE_START_TICK;
    uint16_t    wID          = 0;

    for (int i = 1; i < argc; ++i) {
        auto match = [&](const char *f){ return strcmp(argv[i], f) == 0; };
        auto next  = [&]() -> const char* { return (i+1 < argc) ? argv[++i] : nullptr; };
        if      (match("--brokers"))        { auto v = next(); if (v) sBrokers     = v; }
        else if (match("--channel"))        { auto v = next(); if (v) sChannel     = v; }
        else if (match("--session-name"))   { auto v = next(); if (v) sSessionName = v; }
        else if (match("--counter-name"))   { auto v = next(); if (v) sCounterName = v; }
        else if (match("--count"))          { auto v = next(); if (v) nCount       = (uint64_t)strtoull(v, nullptr, 10); }
        else if (match("--start-tick"))     { auto v = next(); if (v) qwStartTick  = (uint64_t)strtoull(v, nullptr, 10); }
        else if (match("--wid"))            { auto v = next(); if (v) wID          = (uint16_t)atoi(v); }
        else if (match("--security-proto")) { auto v = next(); if (v) sSecProto    = v; }
        else if (match("--sasl-mechanism")) { auto v = next(); if (v) sSaslMech    = v; }
        else if (match("--user"))           { auto v = next(); if (v) sUser        = v; }
        else if (match("--pass"))           { auto v = next(); if (v) sPass        = v; }
        else if (match("--help"))           { goto usage; }
        else { fprintf(stderr, "[PROBE] unknown arg: %s\n", argv[i]); goto usage; }
    }

    if (false) {
    usage:
        fprintf(stderr,
            "Usage: kv8probe [options]\n"
            "\n"
            "  --channel <ch>         channel prefix (dots)  [kv8.probe]\n"
            "  --session-name <n>     Human-readable session name [kv8probe]\n"
            "  --counter-name <n>     Human-readable counter name [synthetic]\n"
            "  --brokers <b>          Bootstrap brokers [localhost:19092]\n"
            "  --count <N>            Number of samples [1000]\n"
            "  --start-tick <t>       Initial qwTimer value [%" PRIu64 "]\n"
            "  --wid <id>             Counter wID [0]\n"
            "  --security-proto <p>   plaintext|sasl_plaintext|sasl_ssl\n"
            "  --sasl-mechanism <m>   PLAIN|SCRAM-SHA-256|SCRAM-SHA-512\n"
            "  --user / --pass        SASL credentials\n"
            "\n"
            "Synthetic clock:\n"
            "  PROBE_FREQUENCY     = %" PRIu64 " Hz (1 tick = %" PRIu64 " ns)\n"
            "  PROBE_START_TICK    = %" PRIu64 "\n"
            "  PROBE_TICK_INTERVAL = %" PRIu64 " ticks = %" PRIu64 " us per sample\n",
            PROBE_START_TICK,
            PROBE_FREQUENCY, (uint64_t)(1000000000ULL / PROBE_FREQUENCY),
            PROBE_START_TICK,
            PROBE_TICK_INTERVAL, (uint64_t)(PROBE_TICK_INTERVAL * 1000000ULL / PROBE_FREQUENCY));
        return 1;
    }

    // ── Derive topic names ────────────────────────────────────────────────────
    // session prefix: <channel>.<sessionID>
    // group topic   : <prefix>.d.<XXXXXXXX>
    // data topic    : <prefix>.d.<XXXXXXXX>.<CCCC>   (per-counter layout)
    // registry topic: <channel>._registry
    // log topic     : <prefix>._log

    // Generate a unique session ID using the existing utility.
    // GenerateTopicName returns "<prefix>.TIMESTAMPZ-SUFFIX"; strip the prefix.
    std::string sFullGen = kv8util::GenerateTopicName("S");
    std::string sSessionID;
    {
        auto pos = sFullGen.find('.');
        sSessionID = (pos != std::string::npos) ? sFullGen.substr(pos + 1) : sFullGen;
    }

    uint32_t dwHash = Fnv32(sChannel);

    char szHash[16];
    snprintf(szHash, sizeof(szHash), "%08X", dwHash);

    char szCounterHex[16];
    snprintf(szCounterHex, sizeof(szCounterHex), "%04X", (unsigned)wID);

    std::string sPrefix      = sChannel + "." + sSessionID;
    std::string sGroupTopic  = sPrefix + ".d." + szHash;          // GROUP record only
    std::string sDataTopic   = sGroupTopic + "." + szCounterHex;  // per-counter data
    std::string sLogTopic    = sPrefix + "._log";
    std::string sRegTopic    = sChannel + "._registry";

    auto kCfg = kv8util::BuildKv8Config(sBrokers, sSecProto, sSaslMech, sUser, sPass);
    // Heartbeat starts automatically on connect; no manual StartHeartbeat needed.
    kCfg.sHeartbeatTopic      = sPrefix + ".hb";
    kCfg.nHeartbeatIntervalMs = 3000;

    auto producer = IKv8Producer::Create(kCfg);
    if (!producer) { fprintf(stderr, "[PROBE] producer create failed\n"); return 1; }

    // ── Pre-flight: data topic must be empty ──────────────────────────────────
    {
        auto consumer = IKv8Consumer::Create(kCfg);
        if (consumer)
        {
            auto counts = consumer->GetTopicMessageCounts({sDataTopic}, 8000);
            auto it = counts.find(sDataTopic);
            if (it != counts.end() && it->second > 0)
            {
                fprintf(stderr,
                        "[PROBE] ERROR: data topic '%s' already has %" PRId64 " message(s).\n"
                        "[PROBE]        Use a different --channel or re-run to get a new sessionID.\n",
                        sDataTopic.c_str(), it->second);
                return 2;
            }
        }
    }

    // ── Get current FILETIME for the GROUP timer anchor ───────────────────────
    uint32_t dwTimeHi = 0, dwTimeLo = 0;
    GetCurrentFiletime(dwTimeHi, dwTimeLo);

    printf("[PROBE] ====================================================\n");
    printf("[PROBE]  channel      : %s\n", sChannel.c_str());
    printf("[PROBE]  session      : %s\n", sSessionID.c_str());
    printf("[PROBE]  group hash   : 0x%08X\n", dwHash);
    printf("[PROBE]  registry     : %s\n", sRegTopic.c_str());
    printf("[PROBE]  group topic  : %s\n", sGroupTopic.c_str());
    printf("[PROBE]  data topic   : %s\n", sDataTopic.c_str());
    printf("[PROBE]  freq         : %" PRIu64 " Hz\n", PROBE_FREQUENCY);
    printf("[PROBE]  start_tick   : %" PRIu64 "\n", qwStartTick);
    printf("[PROBE]  interval     : %" PRIu64 " ticks (%" PRIu64 " us)\n",
           PROBE_TICK_INTERVAL, (uint64_t)(PROBE_TICK_INTERVAL * 1000000ULL / PROBE_FREQUENCY));
    printf("[PROBE]  total_samples: %" PRIu64 "\n", nCount);
    printf("[PROBE]  wID          : %u\n", (unsigned)wID);
    printf("[PROBE] ====================================================\n");

    // ── Write registry records BEFORE any data ────────────────────────────────
    // 1. KV8_CID_LOG: announces the session (name + log topic)
    WriteRegistryRecord(producer.get(), sRegTopic,
        /*hash*/    0,
        /*cid*/     KV8_CID_LOG,
        /*flags*/   0,
        /*min/max*/ 0.0, 0.0, 0.0, 0.0,
        /*timer*/   0, 0, 0, 0,
        /*name*/    sSessionName,
        /*topic*/   sLogTopic);

    // 2. KV8_CID_GROUP: announces the KV8 group (timer anchors + group topic)
    WriteRegistryRecord(producer.get(), sRegTopic,
        /*hash*/    dwHash,
        /*cid*/     KV8_CID_GROUP,
        /*flags*/   0,
        /*min/max*/ 0.0, 0.0, 0.0, 0.0,
        /*timer*/   PROBE_FREQUENCY, qwStartTick, dwTimeHi, dwTimeLo,
        /*name*/    sChannel,
        /*topic*/   sGroupTopic);

    // 3. Counter record: describes the synthetic counter (name + per-counter topic)
    WriteRegistryRecord(producer.get(), sRegTopic,
        /*hash*/    dwHash,
        /*cid*/     wID,
        /*flags*/   1,  // bit 0: enabled
        /*min/max*/ 0.0, 0.0, 1023.0, 1023.0,
        /*timer*/   0, 0, 0, 0,
        /*name*/    sCounterName,
        /*topic*/   sDataTopic);

    producer->Flush(5000);
    printf("[PROBE] Registry records written: LOG + GROUP + counter\n");

    printf("[PROBE] Heartbeat running on: %s (interval=3s)\n", kCfg.sHeartbeatTopic.c_str());

    // -- Write manifest for kv8verify --topic compatibility ────────────────────
    // kv8verify --topic <dataTopic> reads this to reconstruct expected timers.
    {
        ProbeManifest mf{};
        mf.qwFrequency    = PROBE_FREQUENCY;
        mf.qwStartTick    = qwStartTick;
        mf.qwTickInterval = PROBE_TICK_INTERVAL;
        mf.qwTotalSamples = nCount;
        mf.wID            = wID;
        std::string sMfTopic = sDataTopic + "._manifest";
        const char *mfKey = "manifest";
        if (!producer->Produce(sMfTopic, &mf, sizeof(mf), mfKey, strlen(mfKey)))
            fprintf(stderr, "[PROBE] manifest produce failed\n");
        producer->Flush(5000);
        printf("[PROBE] Manifest written to: %s\n", sMfTopic.c_str());
    }

    // ── Write data samples with deterministic synthetic timestamps ────────────
    Kv8TelValue val{};
    val.sCommonRaw.dwBits = MakeExtHeader((uint32_t)sizeof(Kv8TelValue));
    val.wID = wID;

    // Key: wID as 2-byte big-endian  (matches ClKafka convention)
    uint16_t keyBE = (uint16_t)(((wID & 0xFF) << 8) | ((wID >> 8) & 0xFF));
    uint64_t nFailed = 0;

    printf("[PROBE] Sending %" PRIu64 " samples to %s ...\n", nCount, sDataTopic.c_str());

    for (uint64_t i = 0; i < nCount; ++i) {
        val.wSeqN   = (uint16_t)(i & 0xFFFF);
        val.qwTimer = qwStartTick + i * PROBE_TICK_INTERVAL; // fully deterministic
        val.dbValue = (double)(i & 0x3FF);                   // rolling 0..1023

        if (!producer->Produce(sDataTopic, &val, sizeof(val), &keyBE, sizeof(keyBE)))
            nFailed++;

        if ((i & 0xFFF) == 0xFFF)
            producer->Flush(0);
    }

    producer->Flush(30000);

    printf("[PROBE] Send complete.  failed_produce=%" PRIu64 "\n", nFailed);
    printf("[PROBE] Expected qwTimer range: %" PRIu64 " .. %" PRIu64 "\n",
           qwStartTick,
           qwStartTick + (nCount - 1) * PROBE_TICK_INTERVAL);
    printf("[PROBE] RESULT: %s\n", nFailed == 0 ? "OK" : "ERRORS");

    // Print the kv8verify invocation for the new data topic.
    printf("\n[PROBE] ---- kv8verify shell command ----\n");
    printf("kv8verify"
           " --topic %s"
           " --count %" PRIu64
           " --start-tick %" PRIu64
           " --wid %u"
           " --brokers %s"
           " --security-proto %s"
           " --sasl-mechanism %s"
           " --user %s"
           " --pass %s"
           "\n",
           sDataTopic.c_str(), nCount, qwStartTick, (unsigned)wID,
           sBrokers.c_str(), sSecProto.c_str(), sSaslMech.c_str(),
           sUser.c_str(), sPass.c_str());

    printf("\n[PROBE] ---- launch.json args (paste into kv8verify config) ----\n");
    printf("\"--topic\", \"%s\",\n"
           "\"--count\", \"%" PRIu64 "\",\n"
           "\"--start-tick\", \"%" PRIu64 "\",\n"
           "\"--wid\", \"%u\",\n"
           "\"--brokers\", \"%s\",\n"
           "\"--security-proto\", \"%s\",\n"
           "\"--sasl-mechanism\", \"%s\",\n"
           "\"--user\", \"%s\",\n"
           "\"--pass\", \"%s\"\n",
           sDataTopic.c_str(), nCount, qwStartTick, (unsigned)wID,
           sBrokers.c_str(), sSecProto.c_str(), sSaslMech.c_str(),
           sUser.c_str(), sPass.c_str());
    printf("[PROBE] -----------------------------------------------------------\n");

    // Heartbeat stops automatically when the producer is destroyed below
    // (clean-shutdown marker is sent by the destructor).

    return (nFailed == 0) ? 0 : 1;
}
