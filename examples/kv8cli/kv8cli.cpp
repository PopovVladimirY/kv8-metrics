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
                                          RegistryMap &registry, HashGroupMap &hashToGroup)
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

static void ProcessLogMessage(const void *pPayload, size_t cbPayload, int64_t tsKafkaMs)
{
    if (cbPayload == 0 || pPayload == nullptr)
        return;

    std::string sTimestamp = Kv8FormatKafkaTimestamp(tsKafkaMs);
    std::string sMsg(reinterpret_cast<const char*>(pPayload), cbPayload);

    printf("[LOG]  %s | %s\n", sTimestamp.c_str(), sMsg.c_str());
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
                                                               registry, hashToGroup);
                if (!sNewTopic.empty())
                    vPendingSubscribes.push_back(sNewTopic);
            }
            else if (nTopic >= 5 && sTopic.compare(nTopic - 5, 5, "._log") == 0)
            {
                qwLogCount++;
                ProcessLogMessage(pPayload, cbLen, tsMs);
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
