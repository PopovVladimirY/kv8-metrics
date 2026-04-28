////////////////////////////////////////////////////////////////////////////////
// kv8cli â€” Kafka consumer for Kv8 telemetry channels                      //
//                                                                            //
// Connects to a Kafka broker, reads the _registry topic for a given channel  //
// prefix, discovers all data topics (d.<hash>), subscribes to them, and logs  //
// every telemetry message to stdout in human-readable format.                //
//                                                                            //
// Usage:                                                                     //
//   kv8cli --brokers localhost:19092 --channel kv8/myapp                       //
//          [--user kv8producer] [--pass kv8secret]                           //
//          [--security-proto sasl_plaintext] [--sasl-mechanism PLAIN]        //
//                                                                            //
// Clean exit: Ctrl+C or Esc                                                  //
////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#  include <conio.h>
#else
#  include <signal.h>
#  include <termios.h>
#  include <unistd.h>
#  include <poll.h>
#endif

#include <kv8/IKv8Consumer.h>

#include <kv8util/Kv8AppUtils.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <ctime>
#include <string>
#include <map>
#include <vector>
#include <atomic>
#include <chrono>

using namespace kv8;



////////////////////////////////////////////////////////////////////////////////
// -- Globals removed -- all state is now local to main() or passed by ref --
////////////////////////////////////////////////////////////////////////////////

// Tool-local display registry (counter name lookup for DATA messages)
// hash -> { counterID -> name }
struct CounterInfo
{
    std::string sName;
    uint16_t    wID;
    uint16_t    wFlags;
    double      dbMin;
    double      dbAlarmMin;
    double      dbMax;
    double      dbAlarmMax;
};

using RegistryMap  = std::map<uint32_t, std::map<uint16_t, CounterInfo>>;
using HashGroupMap = std::map<uint32_t, std::string>;

// hash -> log-site descriptor (file/line/func/format)
struct LogSiteEntry
{
    std::string sFile;
    std::string sFunc;
    std::string sFmt;
    uint32_t    dwLine = 0;
};
using LogSiteMap = std::map<uint32_t, LogSiteEntry>;


////////////////////////////////////////////////////////////////////////////////
// -- Signal / key handling provided by kv8util::AppSignal, kv8util::CheckEscKey
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// -- Command-line arguments ---------------------------------------------------
////////////////////////////////////////////////////////////////////////////////

struct Config
{
    std::string sBrokers        = "localhost:19092";
    std::string sChannel;                            // e.g. "kv8.myapp" (slashes are auto-sanitized to dots)
    std::string sSecurityProto  = "sasl_plaintext";
    std::string sSaslMechanism  = "PLAIN";
    std::string sSaslUser       = "kv8producer";
    std::string sSaslPass       = "kv8secret";
    std::string sGroupID        = "";                // auto-generated if empty
    bool        bFromBeginning  = false;             // --from-beginning: replay all stored messages
};

static void PrintUsage(const char *pExe)
{
    fprintf(stderr,
        "Usage: %s --channel <prefix> [options]\n"
        "\n"
        "Required:\n"
        "  --channel <prefix>         Channel prefix, e.g. kv8.myapp (slashes are accepted and\n"
        "                             auto-converted to dots to match Kafka topic naming).\n"
        "\n"
        "Options:\n"
        "  --brokers <list>           Comma-separated list of Kafka broker host:port\n"
        "                             pairs used for initial cluster discovery.\n"
        "                             Only one broker is needed to bootstrap; the\n"
        "                             client will discover the rest automatically.\n"
        "                             Example: broker1:19092,broker2:19092\n"
        "                             [default: localhost:19092]\n"
        "  --security-proto <proto>   plaintext|sasl_plaintext|sasl_ssl [sasl_plaintext]\n"
        "  --sasl-mechanism <mech>    PLAIN|SCRAM-SHA-256|SCRAM-SHA-512 [PLAIN]\n"
        "  --user <username>          SASL username [kv8producer]\n"
        "  --pass <password>          SASL password [kv8secret]\n"
        "  --group <group-id>         Consumer group ID [auto]\n"
        "  --from-beginning           Replay all stored messages (offset=earliest).\n"
        "                             Default: start from NOW (offset=latest) â€” only\n"
        "                             messages produced after the consumer connects are\n"
        "                             delivered, keeping the consumer aligned with the\n"
        "                             current producer session and avoiding replaying\n"
        "                             data from prior sessions.\n"
        "  --help                     Show this help\n"
        "\n"
        "The tool discovers all sessions under <prefix>., reads their _registry\n"
        "topics, subscribes to all d.<hash> data topics, and prints every telemetry\n"
        "value to stdout.\n"
        "\n"
        "Press Ctrl+C or Esc to exit cleanly.\n",
        pExe);
}

static bool ParseArgs(int argc, char *argv[], Config &o_cfg)
{
    for (int i = 1; i < argc; ++i)
    {
        auto match = [&](const char *flag) { return strcmp(argv[i], flag) == 0; };
        auto next  = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };

        if (match("--channel"))      { auto v = next(); if (!v) return false; o_cfg.sChannel = v; }
        else if (match("--brokers")) { auto v = next(); if (!v) return false; o_cfg.sBrokers = v; }
        else if (match("--security-proto")) { auto v = next(); if (!v) return false; o_cfg.sSecurityProto = v; }
        else if (match("--sasl-mechanism")) { auto v = next(); if (!v) return false; o_cfg.sSaslMechanism = v; }
        else if (match("--user"))    { auto v = next(); if (!v) return false; o_cfg.sSaslUser = v; }
        else if (match("--pass"))    { auto v = next(); if (!v) return false; o_cfg.sSaslPass = v; }
        else if (match("--group"))         { auto v = next(); if (!v) return false; o_cfg.sGroupID = v; }
        else if (match("--from-beginning")) { o_cfg.bFromBeginning = true; }
        else if (match("--help"))           { return false; }
        else { fprintf(stderr, "Unknown argument: %s\n", argv[i]); return false; }
    }
    if (o_cfg.sChannel.empty())
    {
        fprintf(stderr, "Error: --channel is required.\n");
        return false;
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// â”€â”€ Process registry message â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
////////////////////////////////////////////////////////////////////////////////

// Returns the topic name to subscribe to (data topic for normal records,
// log topic for the sentinel wCounterID=0xFFFE record).  Empty if absent/malformed.
static std::string ProcessRegistryMessage(const void *pPayload, size_t cbPayload,
                                          RegistryMap &registry, HashGroupMap &hashToGroup,
                                          LogSiteMap &logSites)
{
    if (cbPayload < sizeof(KafkaRegistryRecord))
        return {};

    const KafkaRegistryRecord *pRec =
        reinterpret_cast<const KafkaRegistryRecord*>(pPayload);

    if (pRec->wVersion != KV8_REGISTRY_VERSION)
    {
        fprintf(stderr, "[WARN] Skipping registry record with unknown version %u (expected %u)\n",
                (unsigned)pRec->wVersion, (unsigned)KV8_REGISTRY_VERSION);
        return {};
    }

    size_t cbExpected = sizeof(KafkaRegistryRecord)
                        + pRec->wNameLen + pRec->wTopicLen;
    if (cbPayload < cbExpected)
        return {};

    const char *pVarData = reinterpret_cast<const char*>(pPayload)
                           + sizeof(KafkaRegistryRecord);

    // wCounterID=KV8_CID_LOG_SITE: trace-log call-site descriptor (Phase L1+).
    // The variable tail is a packed Kv8LogSiteInfo (file/line/func/fmt).  No
    // separate data topic is announced -- the log topic itself is published
    // via the KV8_CID_LOG sentinel below.
    if (pRec->wCounterID == KV8_CID_LOG_SITE)
    {
        Kv8LogSiteInfo info;
        if (Kv8DecodeLogSiteTail(pVarData, pRec->wNameLen, info))
        {
            LogSiteEntry e;
            e.sFile  = std::string(info.sFile);
            e.sFunc  = std::string(info.sFunc);
            e.sFmt   = std::string(info.sFmt);
            e.dwLine = info.dwLine;
            logSites[pRec->dwHash] = std::move(e);
            printf("[LOGSITE] hash=%08X  %s:%u %s()  fmt=\"%s\"\n",
                   pRec->dwHash,
                   std::string(info.sFile).c_str(),
                   info.dwLine,
                   std::string(info.sFunc).c_str(),
                   std::string(info.sFmt ).c_str());
        }
        else
        {
            fprintf(stderr, "[WARN] Malformed KV8_CID_LOG_SITE tail (hash=%08X cbTail=%u)\n",
                    pRec->dwHash, (unsigned)pRec->wNameLen);
        }
        return {};
    }

    std::string sName(pVarData, pRec->wNameLen);
    std::string sTopic(pVarData + pRec->wNameLen, pRec->wTopicLen);

    // wCounterID=KV8_CID_LOG: session log-topic announcement
    if (pRec->wCounterID == KV8_CID_LOG)
    {
        printf("[SESSION] log-topic=\"%s\"  session=\"%s\"\n",
               sTopic.c_str(), sName.c_str());
        return sTopic;
    }

    // Regular counter / group-level record
    CounterInfo ci;
    ci.sName      = sName;
    ci.wID        = pRec->wCounterID;
    ci.wFlags     = pRec->wFlags;
    ci.dbMin      = pRec->dbMin;
    ci.dbAlarmMin = pRec->dbAlarmMin;
    ci.dbMax      = pRec->dbMax;
    ci.dbAlarmMax = pRec->dbAlarmMax;

    registry[pRec->dwHash][pRec->wCounterID] = ci;
    if (pRec->wCounterID == KV8_CID_GROUP)
        hashToGroup[pRec->dwHash] = sName;

    const char *pEnabled = (pRec->wFlags & 1) ? "ON " : "OFF";
    printf("[REGISTRY] hash=%08X  cid=%-5u  %s  range=[%.2f .. %.2f]"
           "  alarm=[%.2f .. %.2f]  topic=\"%s\"  name=\"%s\"\n",
           pRec->dwHash, pRec->wCounterID, pEnabled,
           pRec->dbMin, pRec->dbMax, pRec->dbAlarmMin, pRec->dbAlarmMax,
           sTopic.c_str(), sName.c_str());

    return sTopic;
}

////////////////////////////////////////////////////////////////////////////////
// â”€â”€ Process telemetry data message â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
////////////////////////////////////////////////////////////////////////////////

static void ProcessDataMessage(const void *pPayload, size_t cbPayload,
                               uint32_t dwHash, int64_t tsKafkaMs,
                               const RegistryMap &registry,
                               const HashGroupMap &hashToGroup)
{
    if (cbPayload < sizeof(Kv8TelValue))
        return;

    const Kv8TelValue *pVal = reinterpret_cast<const Kv8TelValue*>(pPayload);

    // Resolve counter name
    std::string sCounterName;
    std::string sGroupName;
    auto itGroup = hashToGroup.find(dwHash);
    if (itGroup != hashToGroup.end())
        sGroupName = itGroup->second;
    else
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%08X", dwHash);
        sGroupName = buf;
    }
    auto itHash = registry.find(dwHash);
    if (itHash != registry.end())
    {
        auto itCid = itHash->second.find(pVal->wID);
        if (itCid != itHash->second.end())
            sCounterName = itCid->second.sName;
    }
    if (sCounterName.empty())
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "counter_%u", pVal->wID);
        sCounterName = buf;
    }

    std::string sTimestamp = Kv8FormatKafkaTimestamp(tsKafkaMs);

    printf("[DATA] %s | hash=%08X | group=%-30s | cid=%-5u | seq=%-5u | timer=%-20" PRIu64 " | value=%16.6f | name=\"%s\"\n",
           sTimestamp.c_str(),
           dwHash,
           sGroupName.c_str(),
           pVal->wID,
           pVal->wSeqN,
           pVal->qwTimer,
           pVal->dbValue,
           sCounterName.c_str());
}

////////////////////////////////////////////////////////////////////////////////
// â”€â”€ Process log message â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
////////////////////////////////////////////////////////////////////////////////

static const char* LogLevelName(uint8_t lvl)
{
    switch (lvl)
    {
        case 0: return "DEBUG";
        case 1: return "INFO ";
        case 2: return "WARN ";
        case 3: return "ERROR";
        case 4: return "FATAL";
        default: return "?????";
    }
}

static std::string FormatWallNs(uint64_t qwWallNs)
{
    // Convert ns since Unix epoch to ISO-8601 with microsecond precision.
    using namespace std::chrono;
    const uint64_t qwSec = qwWallNs / 1000000000ULL;
    const uint64_t qwUs  = (qwWallNs % 1000000000ULL) / 1000ULL;
    std::time_t t = static_cast<std::time_t>(qwSec);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06lluZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             static_cast<unsigned long long>(qwUs));
    return std::string(buf);
}

static void ProcessLogMessage(const void *pPayload, size_t cbPayload, int64_t tsKafkaMs,
                              const LogSiteMap &logSites)
{
    if (cbPayload == 0 || pPayload == nullptr)
        return;

    Kv8LogRecord    hdr;
    std::string_view sPayload;
    if (!Kv8DecodeLogRecord(pPayload, cbPayload, hdr, sPayload))
    {
        // Fall back: print Kafka timestamp + raw bytes so corrupt records are
        // still visible during diagnostics.  Caller has already classified the
        // topic as ._log so this branch indicates a wire-format violation.
        std::string sKaf = Kv8FormatKafkaTimestamp(tsKafkaMs);
        fprintf(stderr, "[LOG-BAD] %s | malformed record (cb=%zu)\n",
                sKaf.c_str(), cbPayload);
        return;
    }

    const std::string sTs = FormatWallNs(hdr.tsNs);
    const char* pLevel = LogLevelName(hdr.bLevel);

    auto it = logSites.find(hdr.dwSiteHash);
    if (it != logSites.end())
    {
        const LogSiteEntry &e = it->second;
        const std::string sMsg = (hdr.bFlags & KV8_LOG_FLAG_TEXT)
                                   ? std::string(sPayload)
                                   : std::string("<typed args, len=")
                                       + std::to_string(sPayload.size()) + ">";
        printf("[%s] [%s] [T:0x%08X CPU:%u] %s:%u %s()\n    %s\n",
               sTs.c_str(), pLevel,
               (unsigned)hdr.dwThreadID, (unsigned)hdr.wCpuID,
               e.sFile.c_str(), (unsigned)e.dwLine, e.sFunc.c_str(),
               sMsg.c_str());
    }
    else
    {
        // Site not yet registered -- print with hash placeholder.  This is
        // expected at startup when log records arrive before their registry
        // record (rare, only on first emission).
        const std::string sMsg = (hdr.bFlags & KV8_LOG_FLAG_TEXT)
                                   ? std::string(sPayload)
                                   : std::string("<typed args, len=")
                                       + std::to_string(sPayload.size()) + ">";
        printf("[%s] [%s] [T:0x%08X CPU:%u] <site:%08X>\n    %s\n",
               sTs.c_str(), pLevel,
               (unsigned)hdr.dwThreadID, (unsigned)hdr.wCpuID,
               hdr.dwSiteHash, sMsg.c_str());
    }
}

////////////////////////////////////////////////////////////////////////////////
// -- MAIN --------------------------------------------------------------------
////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
    // Disable stdout buffering so output is visible immediately, even when
    // stdout is redirected to a file.
    setvbuf(stdout, nullptr, _IONBF, 0);

    Config cfg;
    if (!ParseArgs(argc, argv, cfg))
    {
        PrintUsage(argv[0]);
        return 1;
    }

    // Sanitize channel prefix: replace '/' with '.' to match Kafka topic naming.
    for (char &c : cfg.sChannel) { if (c == '/') c = '.'; }

    // -- Set up clean exit ---------------------------------------------------
    kv8util::AppSignal::Install();
#ifndef _WIN32
    // Set stdin to non-canonical mode for Esc detection
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
#endif

    // -- Generate group ID if not provided -----------------------------------
    if (cfg.sGroupID.empty())
    {
        auto now = std::chrono::system_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()).count();
        char buf[64];
        snprintf(buf, sizeof(buf), "kv8cli-%lld", (long long)ms);
        cfg.sGroupID = buf;
    }

    printf("========================================================\n");
    printf("  kv8cli -- Kv8 Kafka Telemetry Consumer\n");
    printf("========================================================\n");
    printf("  Brokers  : %s\n", cfg.sBrokers.c_str());
    printf("  Channel  : %s\n", cfg.sChannel.c_str());
    printf("  Security : %s / %s\n", cfg.sSecurityProto.c_str(), cfg.sSaslMechanism.c_str());
    printf("  User     : %s\n", cfg.sSaslUser.c_str());
    printf("  Group    : %s\n", cfg.sGroupID.c_str());
    printf("========================================================\n");
    printf("  Press Ctrl+C or Esc to exit.\n");
    printf("========================================================\n\n");

    // -- Build Kv8Config and create consumer ---------------------------------
    auto kCfg = kv8util::BuildKv8Config(
        cfg.sBrokers, cfg.sSecurityProto, cfg.sSaslMechanism,
        cfg.sSaslUser, cfg.sSaslPass, cfg.sGroupID);

    auto consumer = IKv8Consumer::Create(kCfg);
    if (!consumer)
    {
        fprintf(stderr, "[ERROR] Failed to create Kafka consumer.\n");
#ifndef _WIN32
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif
        return 1;
    }

    // -- Initial subscription to the channel registry topic ------------------
    std::string sRegTopic = cfg.sChannel + "._registry";
    consumer->Subscribe(sRegTopic);

    printf("[INFO] Subscribed to %s._registry.\n"
           "[INFO] Log and data topics discovered via registry records.\n\n",
           cfg.sChannel.c_str());

    // -- Main consume loop ---------------------------------------------------
    RegistryMap  registry;
    HashGroupMap hashToGroup;
    LogSiteMap   logSites;

    uint64_t qwMsgCount  = 0;
    uint64_t qwDataCount = 0;
    uint64_t qwRegCount  = 0;
    uint64_t qwLogCount  = 0;

    // Topics discovered during a Poll() call must be subscribed to AFTER Poll
    // returns (Subscribe() must not be called from within the Poll callback).
    std::vector<std::string> vPendingSubscribes;

    while (kv8util::AppSignal::IsRunning())
    {
        // Check for Esc key
        if (kv8util::CheckEscKey())
        {
            printf("\n[INFO] Esc pressed. Exiting...\n");
            kv8util::AppSignal::RequestStop();
            break;
        }

        vPendingSubscribes.clear();

        consumer->Poll(200, [&](std::string_view  sTopic,
                                const void        *pPayload,
                                size_t             cbLen,
                                int64_t            tsMs)
        {
            qwMsgCount++;
            const size_t nTopic = sTopic.size();

            if (nTopic >= 10 && sTopic.compare(nTopic - 10, 10, "._registry") == 0)
            {
                qwRegCount++;
                std::string sNewTopic = ProcessRegistryMessage(pPayload, cbLen,
                                                               registry, hashToGroup,
                                                               logSites);
                if (!sNewTopic.empty())
                    vPendingSubscribes.push_back(sNewTopic);
            }
            else if (nTopic >= 5 && sTopic.compare(nTopic - 5, 5, "._log") == 0)
            {
                qwLogCount++;
                ProcessLogMessage(pPayload, cbLen, tsMs, logSites);
            }
            else
            {
                uint32_t dwHash = 0;
                if (Kv8ExtractHashFromTopic(sTopic, dwHash))
                {
                    qwDataCount++;
                    ProcessDataMessage(pPayload, cbLen, dwHash, tsMs,
                                       registry, hashToGroup);
                }
                else
                {
                    printf("[UNKN] topic=%s  len=%zu\n", sTopic.data(), cbLen);
                }
            }
        });

        // Subscribe to newly discovered topics (outside Poll callback)
        for (const auto &sTopic : vPendingSubscribes)
        {
            consumer->Subscribe(sTopic);
            printf("[INFO] Subscribed to topic: %s\n", sTopic.c_str());
        }

        // Periodic stats (every 1000 messages)
        if (qwMsgCount > 0 && qwMsgCount % 1000 == 0)
        {
            printf("[STAT] total=%" PRIu64 "  data=%" PRIu64
                   "  registry=%" PRIu64 "  log=%" PRIu64 "\n",
                   qwMsgCount, qwDataCount, qwRegCount, qwLogCount);
        }
    }

    // -- Clean shutdown ------------------------------------------------------
    consumer->Stop();

    printf("\n[INFO] Shutting down...\n");
    printf("[STAT] Final: total=%" PRIu64 "  data=%" PRIu64
           "  registry=%" PRIu64 "  log=%" PRIu64 "\n",
           qwMsgCount, qwDataCount, qwRegCount, qwLogCount);

    // Print discovered registry summary
    if (!registry.empty())
    {
        printf("\n[INFO] Registry summary (%zu group(s)):\n", registry.size());
        for (auto &hashEntry : registry)
        {
            auto itGroup = hashToGroup.find(hashEntry.first);
            printf("  [%08X] %s (%zu counter(s))\n",
                   hashEntry.first,
                   itGroup != hashToGroup.end() ? itGroup->second.c_str() : "<unknown>",
                   hashEntry.second.size());
            for (auto &cidEntry : hashEntry.second)
            {
                printf("    cid=%-5u  %s  range=[%.2f..%.2f]  name=\"%s\"\n",
                       cidEntry.second.wID,
                       (cidEntry.second.wFlags & 1) ? "ON " : "OFF",
                       cidEntry.second.dbMin, cidEntry.second.dbMax,
                       cidEntry.second.sName.c_str());
            }
        }
    }

    printf("[INFO] Cleanup complete. Goodbye.\n");

#ifndef _WIN32
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif
    return 0;
}
