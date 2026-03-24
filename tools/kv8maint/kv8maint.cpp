////////////////////////////////////////////////////////////////////////////////
// kv8maint -- Kv8 Kafka maintenance tool
//
// Lists sessions stored in a Kv8 channel, allows inspecting session details
// (topics, groups, counters, message counts) and deleting sessions.
//
// Usage:
//   kv8maint --channel <prefix> [options]
//
//   --channel <prefix>         Channel prefix (e.g. kv8.myapp)
//   --brokers <list>           Broker bootstrap list  [localhost:19092]
//   --user <username>          SASL username          [kv8producer]
//   --pass <password>          SASL password          [kv8secret]
//   --security-proto <proto>   plaintext|sasl_plaintext|sasl_ssl
//   --sasl-mechanism <mech>    PLAIN|SCRAM-SHA-256|SCRAM-SHA-512
//   --session <sessionID>      Jump directly to that session's detail view
//   --delete <sessionID>       Delete the named session non-interactively
//   --yes                      Skip confirmation when deleting
//   --help                     Show this help
//
// Interactive navigation:
//   Session list -> select by number -> session detail view
//   In detail view: press D to delete, B to go back, Q to quit
////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#  include <conio.h>
#else
#  include <termios.h>
#  include <unistd.h>
#endif

#include <kv8/IKv8Consumer.h>

#include <kv8util/Kv8AppUtils.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

using namespace kv8;

////////////////////////////////////////////////////////////////////////////////
// Kv8 telemetry counter packet (KV8_TEL_TYPE_COUNTER, subtype=1)
// Written into the data topic at counter creation time.
// Wire layout (pack 2):
//   offset  0 : uint32_t dwBits  -- Kv8PacketHeader: bits[4:0]=type(2), bits[9:5]=subtype(1), bits[31:10]=total size
//   offset  4 : uint16_t wID
//   offset  6 : uint16_t bOn
//   offset  8 : double   dbMin
//   offset 16 : double   dbAlarmMin
//   offset 24 : double   dbMax
//   offset 32 : double   dbAlarmMax
//   offset 40 : UTF-16LE name (variable length, null-terminated)
////////////////////////////////////////////////////////////////////////////////

#pragma pack(push, 2)
struct Kv8TelCounterV2Wire
{
    uint32_t dwBits;
    uint16_t wID;
    uint16_t bOn;
    double   dbMin;
    double   dbAlarmMin;
    double   dbMax;
    double   dbAlarmMax;
    // followed by UTF-16LE name bytes
};
#pragma pack(pop)

struct ScannedCounter
{
    uint16_t    wID;
    bool        bOn;
    double      dbMin;
    double      dbAlarmMin;
    double      dbMax;
    double      dbAlarmMax;
    std::string sName;
};

// topic -> counters found by scanning the data topic
using ScannedCounterMap = std::map<std::string, std::vector<ScannedCounter>>;

////////////////////////////////////////////////////////////////////////////////
// Config and argument parsing
////////////////////////////////////////////////////////////////////////////////

struct Config
{
    std::string sBrokers       = "localhost:19092";
    std::string sChannel;
    std::string sSecurityProto = "sasl_plaintext";
    std::string sSaslMechanism = "PLAIN";
    std::string sSaslUser      = "kv8producer";
    std::string sSaslPass      = "kv8secret";
    std::string sSession;     // --session: jump to detail / --delete target
    bool        bDelete        = false;
    bool        bDeleteChannel = false;
    bool        bYes           = false;
};

static void PrintUsage(const char *exe)
{
    fprintf(stderr,
        "kv8maint -- Kv8 Kafka maintenance tool\n"
        "\n"
        "Usage: %s [--channel <prefix>] [options]\n"
        "\n"
        "  --channel is optional. When omitted the tool auto-discovers all channels\n"
        "  on the broker (any topic ending in ._registry) and presents a list.\n"
        "\n"
        "Connection:\n"
        "  --brokers <list>           Bootstrap brokers  [localhost:19092]\n"
        "  --security-proto <proto>   plaintext|sasl_plaintext|sasl_ssl\n"
        "  --sasl-mechanism <mech>    PLAIN|SCRAM-SHA-256|SCRAM-SHA-512\n"
        "  --user <username>          SASL username  [kv8producer]\n"
        "  --pass <password>          SASL password  [kv8secret]\n"
        "\n"
        "Actions:\n"
        "  --channel <prefix>         Target channel, e.g. kv8.myapp\n"
        "                             (slashes auto-converted to dots)\n"
        "  --session <sessionID>      Jump directly to that session's detail view\n"
        "  --delete <sessionID>       Delete the named session (requires --channel)\n"
        "  --delete-channel           Delete the ENTIRE channel and all its topics\n"
        "  --yes                      Skip confirmation prompt when deleting\n"
        "\n"
        "  --help                     Show this help\n"
        "\n"
        "Interactive navigation (default):\n"
        "  1. All channels on the broker are auto-discovered; select one.\n"
        "  2. Sessions in that channel are listed; select number, C=delete channel, Q=quit.\n"
        "  3. Session detail view: [D] delete  [B] back  [Q] quit\n",
        exe);
}

static bool ParseArgs(int argc, char *argv[], Config &cfg)
{
    bool bDeleteSet = false;
    for (int i = 1; i < argc; ++i)
    {
        auto match = [&](const char *f) { return strcmp(argv[i], f) == 0; };
        auto next  = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : nullptr; };

        if      (match("--channel"))         { auto v = next(); if (!v) return false; cfg.sChannel        = v; }
        else if (match("--brokers"))         { auto v = next(); if (!v) return false; cfg.sBrokers         = v; }
        else if (match("--security-proto"))  { auto v = next(); if (!v) return false; cfg.sSecurityProto   = v; }
        else if (match("--sasl-mechanism"))  { auto v = next(); if (!v) return false; cfg.sSaslMechanism   = v; }
        else if (match("--user"))            { auto v = next(); if (!v) return false; cfg.sSaslUser         = v; }
        else if (match("--pass"))            { auto v = next(); if (!v) return false; cfg.sSaslPass         = v; }
        else if (match("--session"))         { auto v = next(); if (!v) return false; cfg.sSession          = v; }
        else if (match("--delete"))
        {
            auto v = next(); if (!v) return false;
            cfg.sSession = v;
            cfg.bDelete  = true;
            bDeleteSet   = true;
        }
        else if (match("--delete-channel")) { cfg.bDeleteChannel = true; }
        else if (match("--yes"))    { cfg.bYes = true; }
        else if (match("--help"))   { return false; }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return false;
        }
    }
    (void)bDeleteSet;
    if (cfg.bDelete && cfg.sChannel.empty())
    {
        fprintf(stderr, "Error: --delete requires --channel <prefix>.\n");
        return false;
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Formatting helpers
////////////////////////////////////////////////////////////////////////////////

// Convert Windows FILETIME (100-ns ticks since 1601-01-01) to ISO 8601 string.
static std::string FormatFiletime(uint32_t dwHi, uint32_t dwLo)
{
    if (dwHi == 0 && dwLo == 0) return "(unknown)";

    // Combine into 64-bit 100-ns tick count.
    uint64_t ft = ((uint64_t)dwHi << 32) | (uint64_t)dwLo;

    // Convert to Unix epoch (seconds): offset from 1601-01-01 to 1970-01-01 is
    // 11644473600 seconds.  Divide by 10000000 to go from 100-ns to seconds.
    const uint64_t EPOCH_DIFF_S = 11644473600ULL;
    uint64_t tSec100ns = ft / 10000000ULL;
    if (tSec100ns < EPOCH_DIFF_S) return "(before 1970)";
    time_t tSec = (time_t)(tSec100ns - EPOCH_DIFF_S);

    struct tm tmBuf;
#ifdef _WIN32
    gmtime_s(&tmBuf, &tSec);
#else
    gmtime_r(&tSec, &tmBuf);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmBuf);
    return buf;
}

// Human-readable frequency string (Hz).
static std::string FormatFrequency(uint64_t hz)
{
    if (hz == 0) return "(unknown)";
    char buf[32];
    if (hz >= 1000000000ULL)
        snprintf(buf, sizeof(buf), "%.3f GHz", (double)hz / 1e9);
    else if (hz >= 1000000ULL)
        snprintf(buf, sizeof(buf), "%.3f MHz", (double)hz / 1e6);
    else if (hz >= 1000ULL)
        snprintf(buf, sizeof(buf), "%.1f kHz", (double)hz / 1e3);
    else
        snprintf(buf, sizeof(buf), "%" PRIu64 " Hz", hz);
    return buf;
}

// Human-readable message count (comma-grouped or "?").
static std::string FormatCount(int64_t n)
{
    if (n < 0) return "?";
    if (n == 0) return "0";
    // Simple grouping: build in reverse then reverse.
    char raw[32];
    snprintf(raw, sizeof(raw), "%" PRId64, n);
    std::string s;
    int len = (int)strlen(raw);
    for (int i = 0; i < len; ++i)
    {
        if (i > 0 && (len - i) % 3 == 0) s += ',';
        s += raw[i];
    }
    return s;
}

////////////////////////////////////////////////////////////////////////////////
// Topic counter scanner
////////////////////////////////////////////////////////////////////////////////

// Convert a UTF-16LE byte sequence to a narrow ASCII/Latin-1 string.
// Non-ASCII wide chars are replaced with '?'.
static std::string Utf16LeToNarrow(const uint8_t *pWide, size_t nBytes)
{
    std::string s;
    for (size_t i = 0; i + 1 < nBytes; i += 2)
    {
        uint16_t wc = (uint16_t)pWide[i] | ((uint16_t)pWide[i + 1] << 8);
        if (wc == 0) break;
        s += (wc < 128u) ? (char)(unsigned char)wc : '?';
    }
    return s;
}

// Scan the beginning of a data topic for KV8_TEL_TYPE_COUNTER packets (subtype=1).
// Counter definition packets appear at the very start of each session, so a
// short hard timeout is sufficient.  Returns counters sorted by wID.
static std::vector<ScannedCounter>
ScanCountersFromTopic(IKv8Consumer *consumer,
                      const std::string &sTopic,
                      int timeoutMs = 2000)
{
    std::vector<ScannedCounter> result;
    std::set<uint16_t> seenIDs;

    consumer->ConsumeTopicFromBeginning(sTopic, timeoutMs,
        [&](const void *pPayload, size_t cbPayload, int64_t)
        {
            if (cbPayload < sizeof(Kv8TelCounterV2Wire)) return;

            const Kv8TelCounterV2Wire *r =
                reinterpret_cast<const Kv8TelCounterV2Wire*>(pPayload);

            // type = bits[4:0], subtype = bits[9:5], totalSize = bits[31:10]
            uint32_t type    =  r->dwBits        & 0x1Fu;
            uint32_t subtype = (r->dwBits >> 5u) & 0x1Fu;
            uint32_t size    =  r->dwBits >> 10u;

            // KV8_TEL_TYPE_COUNTER: type==2 (telemetry), subtype==1
            if (type != 2u || subtype != 1u) return;
            if (size < sizeof(Kv8TelCounterV2Wire) || size > cbPayload) return;
            if (seenIDs.count(r->wID)) return;
            seenIDs.insert(r->wID);

            ScannedCounter sc;
            sc.wID        = r->wID;
            sc.bOn        = (r->bOn != 0);
            sc.dbMin      = r->dbMin;
            sc.dbAlarmMin = r->dbAlarmMin;
            sc.dbMax      = r->dbMax;
            sc.dbAlarmMax = r->dbAlarmMax;

            const uint8_t *pName =
                reinterpret_cast<const uint8_t*>(r) + sizeof(Kv8TelCounterV2Wire);
            size_t nameBytes = size - sizeof(Kv8TelCounterV2Wire);
            sc.sName = Utf16LeToNarrow(pName, nameBytes);

            result.push_back(std::move(sc));
        });

    std::sort(result.begin(), result.end(),
              [](const ScannedCounter &a, const ScannedCounter &b)
              { return a.wID < b.wID; });
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Separator line
////////////////////////////////////////////////////////////////////////////////

static void Separator(char c = '-', int w = 70)
{
    for (int i = 0; i < w; ++i) putchar(c);
    putchar('\n');
}

////////////////////////////////////////////////////////////////////////////////
// Display helpers
////////////////////////////////////////////////////////////////////////////////

// Print the session list with index numbers.
static void PrintSessionList(const std::vector<const SessionMeta*> &list)
{
    Separator('=');
    printf("  Sessions in channel\n");
    Separator('=');
    printf("  %-4s  %-36s  %s\n", "No.", "Session ID", "Name");
    Separator();
    for (size_t i = 0; i < list.size(); ++i)
    {
        const SessionMeta *s = list[i];
        printf("  %-4zu  %-36s  %s\n",
               i + 1,
               s->sSessionID.c_str(),
               s->sName.c_str());
    }
    Separator();
}

// Print full details for one session.
// counts:   topic -> message count (may contain -1 for unknown)
// scanned:  topic -> counters found by scanning (legacy fallback; empty for per-counter layout)
static void PrintSessionDetail(const SessionMeta                    &sm,
                               const std::map<std::string,int64_t>  &counts,
                               const ScannedCounterMap               &scanned)
{
    Separator('=');
    printf("  Session detail\n");
    Separator('=');
    printf("  ID      : %s\n", sm.sSessionID.c_str());
    printf("  Name    : %s\n", sm.sName.c_str());
    printf("  Prefix  : %s\n", sm.sSessionPrefix.c_str());
    printf("  Control : %s\n", sm.sControlTopic.c_str());
    printf("  Log     : %s\n", sm.sLogTopic.c_str());

    // Determine layout before printing the topics count so we can report the
    // number of counter topics rather than all dataTopics entries (which
    // include virtual group topics in the per-counter layout).
    bool bPerCounterLayout = !sm.topicToCounter.empty();
    printf("  Topics  : %zu data topic(s)\n",
           bPerCounterLayout ? sm.topicToCounter.size() : sm.dataTopics.size());
    Separator();

    if (bPerCounterLayout)
    {
        // ── Per-counter layout ────────────────────────────────────────────────
        // Each counter owns its dedicated topic.
        // Virtual group topics (d.<channelID>) carry only GROUP metadata.

        // ── Summary table: counter topics only ───────────────────────────────
        printf("  %-52s  %-12s  %s\n", "Counter topic", "Messages", "Counter");
        Separator('-', 70);

        for (const auto &entry : sm.topicToCounter)
        {
            const std::string &sTopic = entry.first;
            const CounterMeta &cm     = entry.second;

            int64_t msgCount = -1;
            auto itC = counts.find(sTopic);
            if (itC != counts.end()) msgCount = itC->second;

            printf("  %-52s  %-12s  [%4u] %s\n",
                   sTopic.c_str(),
                   FormatCount(msgCount).c_str(),
                   (unsigned)cm.wCounterID,
                   cm.sName.c_str());
        }
        Separator();

        // ── Per-group detail blocks ───────────────────────────────────────────
        // Group counter topics by deriving their virtual group topic
        // (= counter topic with the trailing ".<counterID>" segment removed).
        std::set<std::string> groupsShown;

        for (const auto &entry : sm.topicToCounter)
        {
            const std::string &sCounterTopic = entry.first;

            // Derive virtual group topic by stripping last component
            size_t dotPos = sCounterTopic.rfind('.');
            if (dotPos == std::string::npos) continue;
            std::string sVirtualTopic = sCounterTopic.substr(0, dotPos);

            if (!groupsShown.insert(sVirtualTopic).second) continue; // already shown

            // Group metadata from virtual topic
            std::string sGroupName;
            uint32_t    hash    = 0;
            bool        hasHash = false;
            {
                auto itGN = sm.topicToGroupName.find(sVirtualTopic);
                if (itGN != sm.topicToGroupName.end()) sGroupName = itGN->second;
                auto itGH = sm.topicToGroupHash.find(sVirtualTopic);
                if (itGH != sm.topicToGroupHash.end())
                {
                    hash    = itGH->second;
                    hasHash = true;
                }
            }
            uint64_t freq = 0;
            uint32_t ftHi = 0, ftLo = 0;
            {
                auto itF = sm.topicToFrequency.find(sVirtualTopic);
                if (itF != sm.topicToFrequency.end()) freq = itF->second;
                auto itH = sm.topicToTimeHi.find(sVirtualTopic);
                if (itH != sm.topicToTimeHi.end()) ftHi = itH->second;
                auto itL = sm.topicToTimeLo.find(sVirtualTopic);
                if (itL != sm.topicToTimeLo.end()) ftLo = itL->second;
            }

            // Collect counters belonging to this group
            std::vector<const CounterMeta*> groupCounters;
            for (const auto &e : sm.topicToCounter)
            {
                size_t dp = e.first.rfind('.');
                if (dp == std::string::npos) continue;
                if (e.first.substr(0, dp) == sVirtualTopic)
                    groupCounters.push_back(&e.second);
            }
            std::sort(groupCounters.begin(), groupCounters.end(),
                      [](const CounterMeta *a, const CounterMeta *b)
                      { return a->wCounterID < b->wCounterID; });

            printf("  Group   : %s", sGroupName.empty() ? "(unnamed)" : sGroupName.c_str());
            if (hasHash) printf("  (hash %08X)", hash);
            printf("\n");
            printf("  Started : %s\n", FormatFiletime(ftHi, ftLo).c_str());
            printf("  Freq    : %s\n", FormatFrequency(freq).c_str());
            printf("  Counters: %zu\n", groupCounters.size());

            for (const CounterMeta *c : groupCounters)
            {
                int64_t ctrMsg = -1;
                auto itC = counts.find(c->sDataTopic);
                if (itC != counts.end()) ctrMsg = itC->second;

                printf("    [%4u] %-36s  messages=%-12s  min=%-10g  max=%-10g%s\n",
                       (unsigned)c->wCounterID,
                       c->sName.c_str(),
                       FormatCount(ctrMsg).c_str(),
                       c->dbMin, c->dbMax,
                       (c->wFlags & 1) ? "  [enabled]" : "");
            }
            Separator('-', 50);
        }
    }
    else
    {
        // ── Legacy layout: all counters share one topic per group ─────────────

        // Per-topic summary table
        printf("  %-52s  %-12s  %-6s  %s\n", "Data topic", "Messages", "Cntrs", "Group");
        Separator('-', 70);
        for (const auto &sTopic : sm.dataTopics)
        {
            int64_t msgCount = -1;
            auto itC = counts.find(sTopic);
            if (itC != counts.end()) msgCount = itC->second;

            std::string sGroup;
            auto itGN = sm.topicToGroupName.find(sTopic);
            if (itGN != sm.topicToGroupName.end()) sGroup = itGN->second;

            size_t nCtrs    = 0;
            bool   ctrsKnown = false;
            auto itGH = sm.topicToGroupHash.find(sTopic);
            if (itGH != sm.topicToGroupHash.end())
            {
                auto itCtrs = sm.hashToCounters.find(itGH->second);
                if (itCtrs != sm.hashToCounters.end() && !itCtrs->second.empty())
                {
                    nCtrs     = itCtrs->second.size();
                    ctrsKnown = true;
                }
            }
            if (!ctrsKnown)
            {
                auto itSc = scanned.find(sTopic);
                if (itSc != scanned.end() && !itSc->second.empty())
                {
                    nCtrs     = itSc->second.size();
                    ctrsKnown = true;
                }
            }

            char ctrBuf[8];
            if (ctrsKnown) snprintf(ctrBuf, sizeof(ctrBuf), "%zu", nCtrs);
            else           snprintf(ctrBuf, sizeof(ctrBuf), "?");

            printf("  %-52s  %-12s  %-6s  %s\n",
                   sTopic.c_str(),
                   FormatCount(msgCount).c_str(),
                   ctrBuf,
                   sGroup.empty() ? "(no group)" : sGroup.c_str());
        }
        Separator();

        // Per-group detail blocks
        std::vector<std::string> topicOrder(sm.dataTopics.begin(), sm.dataTopics.end());
        std::set<uint32_t> seenHashes;

        for (const auto &sTopic : topicOrder)
        {
            uint64_t freq = 0;
            uint32_t ftHi = 0, ftLo = 0;
            {
                auto itF = sm.topicToFrequency.find(sTopic);
                if (itF != sm.topicToFrequency.end()) freq = itF->second;
                auto itH = sm.topicToTimeHi.find(sTopic);
                if (itH != sm.topicToTimeHi.end()) ftHi = itH->second;
                auto itL = sm.topicToTimeLo.find(sTopic);
                if (itL != sm.topicToTimeLo.end()) ftLo = itL->second;
            }

            std::string sGroupName;
            uint32_t    hash    = 0;
            bool        hasHash = false;
            {
                auto itGH = sm.topicToGroupHash.find(sTopic);
                if (itGH != sm.topicToGroupHash.end())
                {
                    hash    = itGH->second;
                    hasHash = true;
                    auto itGN = sm.hashToGroup.find(hash);
                    if (itGN != sm.hashToGroup.end()) sGroupName = itGN->second;
                }
                if (sGroupName.empty())
                {
                    auto itGN = sm.topicToGroupName.find(sTopic);
                    if (itGN != sm.topicToGroupName.end()) sGroupName = itGN->second;
                }
            }

            const std::vector<CounterMeta> *pCtrs = nullptr;
            if (hasHash)
            {
                auto itCtrs = sm.hashToCounters.find(hash);
                if (itCtrs != sm.hashToCounters.end() && !itCtrs->second.empty())
                    pCtrs = &itCtrs->second;
            }

            if (hasHash && !seenHashes.insert(hash).second) continue;

            int64_t msgCount = -1;
            {
                auto itC = counts.find(sTopic);
                if (itC != counts.end()) msgCount = itC->second;
            }

            printf("  Group   : %s", sGroupName.empty() ? "(unnamed)" : sGroupName.c_str());
            if (hasHash) printf("  (hash %08X)", hash);
            printf("\n");
            printf("  Topic   : %s\n",    sTopic.c_str());
            printf("  Messages: %s\n",    FormatCount(msgCount).c_str());
            printf("  Started : %s\n",    FormatFiletime(ftHi, ftLo).c_str());
            printf("  Freq    : %s\n",    FormatFrequency(freq).c_str());
            if (pCtrs)
            {
                printf("  Counters: %zu\n", pCtrs->size());
                for (const auto &c : *pCtrs)
                {
                    printf("    [%4u] %-40s  min=%-10g  max=%-10g%s\n",
                           (unsigned)c.wCounterID,
                           c.sName.c_str(),
                           c.dbMin, c.dbMax,
                           (c.wFlags & 1) ? "  [enabled]" : "");
                }
            }
            else
            {
                auto itSc = scanned.find(sTopic);
                if (itSc != scanned.end() && !itSc->second.empty())
                {
                    const auto &sctrs = itSc->second;
                    printf("  Counters: %zu\n", sctrs.size());
                    for (const auto &c : sctrs)
                    {
                        printf("    [%4u] %-40s  min=%-10g  max=%-10g%s\n",
                               (unsigned)c.wID,
                               c.sName.c_str(),
                               c.dbMin, c.dbMax,
                               c.bOn ? "  [enabled]" : "");
                    }
                }
                else
                {
                    printf("  Counters: (none found)\n");
                }
            }
            Separator('-', 50);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Interactive key read (single character, no Enter needed)
////////////////////////////////////////////////////////////////////////////////

static char ReadChar()
{
#ifdef _WIN32
    int c = _getch();
    return (char)(c & 0xFF);
#else
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) c = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return c;
#endif
}

// Read a line from stdin, return it trimmed.
static std::string ReadLine()
{
    char buf[256] = {};
    if (!fgets(buf, sizeof(buf), stdin)) return {};
    // trim trailing newline/CR
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    return buf;
}

////////////////////////////////////////////////////////////////////////////////
// Session detail + actions
////////////////////////////////////////////////////////////////////////////////

static void RunSessionDetail(const SessionMeta   &sm,
                             IKv8Consumer        *consumer,
                             const std::string   &sChannel,
                             bool                 bStartWithDelete,
                             bool                 bYes)
{
    // Build list of all topics for count query
    std::vector<std::string> topicsForCount;
    for (auto &t : sm.dataTopics) topicsForCount.push_back(t);

    printf("\n  Querying message counts...\n");
    auto counts = consumer->GetTopicMessageCounts(topicsForCount, 5000);

    // For per-counter layout the registry already contains full counter metadata
    // (name, min/max, thresholds) so there is nothing useful to scan from the
    // data topics.  Only scan legacy sessions where counter definitions are
    // embedded as KV8_TEL_TYPE_COUNTER packets at the start of the shared topic.
    ScannedCounterMap scanned;
    if (sm.topicToCounter.empty())
    {
        printf("  Scanning data topics for counter definitions...\n");
        for (auto &t : sm.dataTopics)
            scanned[t] = ScanCountersFromTopic(consumer, t, 2000);
    }

    PrintSessionDetail(sm, counts, scanned);

    if (bStartWithDelete)
    {
        // Non-interactive path also goes through confirmation below
    }
    else
    {
        printf("\n  [D] Delete session   [B] Back   [Q] Quit\n  > ");
        fflush(stdout);
        char ch = (char)toupper((unsigned char)ReadChar());
        printf("\n");
        if (ch == 'B') return;
        if (ch == 'Q') { exit(0); }
        if (ch != 'D') return;
    }

    // Delete flow
    if (!bYes)
    {
        printf("\n  WARNING: This will permanently delete %zu data topic(s),\n"
               "           the log topic, and control for session:\n"
               "             %s  (%s)\n\n"
               "  Delete? [Y/N]: ",
               sm.dataTopics.size(),
               sm.sSessionID.c_str(),
               sm.sName.c_str());
        fflush(stdout);
        char cAns = (char)toupper((unsigned char)ReadChar());
        printf("\n");
        if (cAns != 'Y')
        {
            printf("  Cancelled.\n");
            return;
        }
    }

    printf("\n  Deleting session '%s'...\n", sm.sSessionID.c_str());
    consumer->DeleteSessionTopics(sm);
    consumer->MarkSessionDeleted(sChannel, sm);
    printf("  Done.\n\n");
}

////////////////////////////////////////////////////////////////////////////////
// Main
////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
    Config cfg;
    if (!ParseArgs(argc, argv, cfg))
    {
        PrintUsage(argv[0]);
        return 1;
    }

    // Sanitise channel if already provided
    if (!cfg.sChannel.empty())
        cfg.sChannel = Kv8SanitizeChannel(cfg.sChannel);

    // Build Kv8Config
    auto kCfg = kv8util::BuildKv8Config(
        cfg.sBrokers, cfg.sSecurityProto, cfg.sSaslMechanism,
        cfg.sSaslUser, cfg.sSaslPass);

    auto consumer = IKv8Consumer::Create(kCfg);
    if (!consumer)
    {
        fprintf(stderr, "[ERROR] Could not create Kafka consumer.\n");
        return 1;
    }

    printf("\n  kv8maint -- Kv8 Kafka maintenance\n");
    printf("  Brokers : %s\n", cfg.sBrokers.c_str());

    // ── Channel discovery screen (when --channel was not given) ──────────────
    if (cfg.sChannel.empty())
    {
        for (;;)
        {
            printf("\n  Scanning broker for Kv8 channels...\n");
            auto channels = consumer->ListChannels(10000);

            if (channels.empty())
            {
                fprintf(stderr, "\n  No Kv8 channels found on %s.\n"
                                "  Check --brokers and credentials, or use --channel explicitly.\n\n",
                        cfg.sBrokers.c_str());
                return 1;
            }

            if (channels.size() == 1)
            {
                cfg.sChannel = channels[0];
                printf("  Channel : %s  (only one found, auto-selected)\n", cfg.sChannel.c_str());
                break;
            }

            Separator('=');
            printf("  Kv8 channels on broker\n");
            Separator('=');
            for (size_t i = 0; i < channels.size(); ++i)
                printf("  [%zu] %s\n", i + 1, channels[i].c_str());
            Separator();
            printf("  Enter number to open, number(s) to delete (e.g. 1,3), or Q to quit: ");
            fflush(stdout);

            std::string input = ReadLine();
            if (input.empty()) continue;
            if (input == "q" || input == "Q")
            {
                printf("  Goodbye.\n\n");
                return 0;
            }

            // Parse one or more channel indices (comma or space separated).
            for (char& c2 : input)
                if (c2 == ',') c2 = ' ';
            std::istringstream ss(input);
            std::vector<size_t> sel;
            std::string token;
            bool bBad = false;
            while (ss >> token)
            {
                char *ep = nullptr;
                long idx = strtol(token.c_str(), &ep, 10);
                if (!ep || *ep != '\0' || idx < 1 || idx > (long)channels.size())
                {
                    printf("  Invalid selection: %s\n", token.c_str());
                    bBad = true;
                    break;
                }
                size_t si = (size_t)(idx - 1);
                bool bDup = false;
                for (size_t ex : sel) if (ex == si) { bDup = true; break; }
                if (!bDup) sel.push_back(si);
            }
            if (bBad || sel.empty()) continue;

            if (sel.size() == 1)
            {
                // Single selection: navigate into that channel.
                cfg.sChannel = channels[sel[0]];
                break;
            }

            // Multi-selection: confirm then delete all selected channels.
            printf("\n  About to delete %zu channel(s):\n", sel.size());
            for (size_t i : sel)
                printf("    [%zu]  %s\n", i + 1, channels[i].c_str());
            printf("\n  Delete? [Y/N]: ");
            fflush(stdout);
            char cDel = (char)toupper((unsigned char)ReadChar());
            printf("\n");
            if (cDel == 'Y')
            {
                for (size_t i : sel)
                {
                    printf("  Deleting channel '%s'...\n", channels[i].c_str());
                    consumer->DeleteChannel(channels[i]);
                }
                printf("  Done.\n\n");
                // Loop back to re-scan remaining channels.
            }
            else
            {
                printf("  Cancelled.\n");
            }
        }
    }

    printf("  Channel : %s\n\n", cfg.sChannel.c_str());

    // Non-interactive --delete-channel
    if (cfg.bDeleteChannel)
    {
        if (!cfg.bYes)
        {
            printf("  WARNING: This will permanently delete the ENTIRE channel '%s'\n"
                   "           including ALL sessions and the registry topic.\n\n"
                   "  Delete? [Y/N]: ",
                   cfg.sChannel.c_str());
            fflush(stdout);
            char cAns = (char)toupper((unsigned char)ReadChar());
            printf("\n");
            if (cAns != 'Y')
            {
                printf("  Cancelled.\n");
                return 0;
            }
        }
        printf("\n  Deleting channel '%s'...\n", cfg.sChannel.c_str());
        consumer->DeleteChannel(cfg.sChannel);
        printf("  Done.\n\n");
        return 0;
    }

    // Discover sessions
    auto sessions = consumer->DiscoverSessions(cfg.sChannel);
    if (sessions.empty())
    {
        fprintf(stderr, "\n  No sessions found in channel '%s'.\n"
                        "  Check --channel, --brokers, and credentials.\n\n",
                cfg.sChannel.c_str());
        return 1;
    }

    // Non-interactive --delete mode
    if (cfg.bDelete)
    {
        if (cfg.sSession.empty())
        {
            fprintf(stderr, "[ERROR] --delete requires a session ID (--delete <sessionID>).\n");
            return 1;
        }
        // Find by session ID
        SessionMeta *pSm = nullptr;
        for (auto &kv : sessions)
            if (kv.second.sSessionID == cfg.sSession) { pSm = &kv.second; break; }

        if (!pSm)
        {
            fprintf(stderr, "[ERROR] Session '%s' not found.\n", cfg.sSession.c_str());
            // List available sessions
            printf("Available sessions:\n");
            for (auto &kv : sessions)
                printf("  %s  (%s)\n", kv.second.sSessionID.c_str(), kv.second.sName.c_str());
            return 1;
        }
        RunSessionDetail(*pSm, consumer.get(), cfg.sChannel, true, cfg.bYes);
        return 0;
    }

    // Build sorted vector for display
    std::vector<const SessionMeta*> list;
    list.reserve(sessions.size());
    for (auto &kv : sessions) list.push_back(&kv.second);
    // Sort by session ID (lexicographic -> chronological for ISO timestamps)
    std::sort(list.begin(), list.end(),
              [](const SessionMeta *a, const SessionMeta *b){
                  return a->sSessionID < b->sSessionID; });

    // If --session was given, jump directly to that session
    if (!cfg.sSession.empty())
    {
        const SessionMeta *pSm = nullptr;
        for (auto *s : list)
            if (s->sSessionID == cfg.sSession) { pSm = s; break; }
        if (!pSm)
        {
            fprintf(stderr, "[ERROR] Session '%s' not found.\n", cfg.sSession.c_str());
            PrintSessionList(list);
            return 1;
        }
        RunSessionDetail(*pSm, consumer.get(), cfg.sChannel, false, cfg.bYes);
        return 0;
    }

    // Interactive session selection loop
    for (;;)
    {
        // Re-read sessions after a possible deletion
        sessions = consumer->DiscoverSessions(cfg.sChannel);
        list.clear();
        for (auto &kv : sessions) list.push_back(&kv.second);
        std::sort(list.begin(), list.end(),
                  [](const SessionMeta *a, const SessionMeta *b){
                      return a->sSessionID < b->sSessionID; });

        if (list.empty())
        {
            printf("  No sessions remaining.\n\n");
            return 0;
        }

        printf("\n");
        PrintSessionList(list);
        printf("  Enter number(s) to select (e.g. 1 or 1,3,5), C to delete channel, or Q to quit: ");
        fflush(stdout);

        std::string input = ReadLine();
        if (input.empty()) continue;
        if (input == "q" || input == "Q") break;

        if (input == "c" || input == "C")
        {
            printf("  WARNING: This will permanently delete the ENTIRE channel '%s'\n"
                   "           including ALL sessions and the registry topic.\n\n"
                   "  Delete? [Y/N]: ",
                   cfg.sChannel.c_str());
            fflush(stdout);
            char cCh = (char)toupper((unsigned char)ReadChar());
            printf("\n");
            if (cCh == 'Y')
            {
                printf("\n  Deleting channel '%s'...\n", cfg.sChannel.c_str());
                consumer->DeleteChannel(cfg.sChannel);
                printf("  Done. Channel is gone.\n\n");
                return 0;
            }
            printf("  Cancelled.\n");
            continue;
        }

        // Parse one or more session indices (comma or space separated).
        std::vector<size_t> sel;
        {
            // Replace commas with spaces for uniform tokenisation.
            for (char& c : input)
                if (c == ',') c = ' ';
            std::istringstream ss(input);
            std::string token;
            bool bBad = false;
            while (ss >> token)
            {
                char *ep = nullptr;
                long idx = strtol(token.c_str(), &ep, 10);
                if (!ep || *ep != '\0' || idx < 1 || idx > (long)list.size())
                {
                    printf("  Invalid selection: %s\n", token.c_str());
                    bBad = true;
                    break;
                }
                // Avoid duplicates.
                size_t si = (size_t)(idx - 1);
                bool bDup = false;
                for (size_t ex : sel)
                    if (ex == si) { bDup = true; break; }
                if (!bDup)
                    sel.push_back(si);
            }
            if (bBad || sel.empty()) continue;
        }

        if (sel.size() == 1)
        {
            // Single selection: open detail view.
            RunSessionDetail(*list[sel[0]], consumer.get(), cfg.sChannel, false, cfg.bYes);
        }
        else
        {
            // Multi-selection: confirm once then delete all.
            printf("\n  About to delete %zu session(s):\n", sel.size());
            for (size_t i : sel)
                printf("    [%zu]  %s  (%s)\n",
                       i + 1,
                       list[i]->sSessionID.c_str(),
                       list[i]->sName.c_str());
            printf("\n  Delete? [Y/N]: ");
            fflush(stdout);
            char cDel = (char)toupper((unsigned char)ReadChar());
            printf("\n");
            if (cDel == 'Y')
            {
                for (size_t i : sel)
                {
                    printf("  Deleting '%s'...\n", list[i]->sSessionID.c_str());
                    consumer->DeleteSessionTopics(*list[i]);
                    consumer->MarkSessionDeleted(cfg.sChannel, *list[i]);
                }
                printf("  Done.\n\n");
            }
            else
            {
                printf("  Cancelled.\n");
            }
        }
    }

    printf("  Goodbye.\n\n");
    return 0;
}
