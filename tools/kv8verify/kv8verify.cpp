////////////////////////////////////////////////////////////////////////////////
// kv8verify -- Kafka telemetry integrity verifier (consumer side)
//
// Two operating modes:
//
// MODE 1: Manifest verification (--topic <name>)
//   Reads the manifest from <topic>._manifest, then consumes all messages from
//   <topic> and verifies every Kv8TelValue payload against kv8probe's
//   deterministic synthetic clock.
//
// MODE 2: Sequence gap detection (--channel <name> [--session <id>])
//   Discovers sessions from the channel registry, reads all data topics from
//   offset 0 (BEGINNING), and verifies that wSeqN values are contiguous for
//   each counter (wID).  Reports gaps, duplicates, and out-of-order samples.
//   This mode works with real kv8 telemetry -- no manifest required.
//
// Exit code: 0 = PASS, 1 = FAIL
//
// Usage:
//   kv8verify --topic <name> [options]              # manifest mode
//   kv8verify --channel <name> [--session <id>]     # seqgap mode
////////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>
#include <algorithm>

#include <time.h>
#include <kv8/IKv8Consumer.h>

#include <kv8util/Kv8AppUtils.h>

using namespace kv8;

////////////////////////////////////////////////////////////////////////////////
// ProbeManifest -- matches kv8probe exactly (pack(2)).
// Kv8PacketHeader and Kv8TelValue are provided by kv8/Kv8Types.h.
////////////////////////////////////////////////////////////////////////////////
#pragma pack(push, 2)

struct ProbeManifest
{
    uint64_t qwFrequency;
    uint64_t qwStartTick;
    uint64_t qwTickInterval;
    uint64_t qwTotalSamples;
    uint16_t wID;
    uint16_t wPad[3];
};

#pragma pack(pop)

////////////////////////////////////////////////////////////////////////////////
// Per-counter sequence tracker for seqgap mode
////////////////////////////////////////////////////////////////////////////////

struct SeqTracker
{
    uint16_t wID;             // counter ID
    std::string sTopic;       // topic this counter was seen on
    uint64_t nReceived;       // total messages received for this counter
    uint64_t nGaps;           // number of sequence gaps detected
    uint64_t nGapSamples;     // total missing samples across all gaps
    uint64_t nDuplicates;     // same wSeqN seen again (without wrapping)
    uint64_t nOutOfOrder;     // wSeqN went backwards unexpectedly
    bool     bFirstSeen;      // true until first message arrives
    uint16_t wLastSeqN;       // last seen wSeqN
    uint64_t qwFirstTimer;    // qwTimer of first sample
    uint64_t qwLastTimer;     // qwTimer of last sample

    // Gap detail log (first N gaps per counter for diagnostics)
    static const int MAX_GAP_LOG = 20;
    struct GapEntry { uint16_t wExpected; uint16_t wGot; uint64_t nMissed; };
    std::vector<GapEntry> gapLog;
};

////////////////////////////////////////////////////////////////////////////////
// run_manifest_mode -- original kv8probe verification
////////////////////////////////////////////////////////////////////////////////

static int run_manifest_mode(const Kv8Config &kCfg, const std::string &sTopic,
                             uint64_t nCount, uint64_t qwStartTick, uint16_t wID)
{
    auto consumer = IKv8Consumer::Create(kCfg);
    if (!consumer) { fprintf(stderr, "[VERIFY] FAIL: could not create consumer.\n"); return 1; }

    // -- Step 1: read manifest ------------------------------------------------
    std::string sManifestTopic = sTopic + "._manifest";

    ProbeManifest mf{};
    bool gotManifest = false;

    printf("[VERIFY] Reading manifest from %s...\n", sManifestTopic.c_str());
    consumer->ConsumeTopicFromBeginning(sManifestTopic, 15000,
        [&](const void *pPayload, size_t cbPayload, int64_t)
        {
            if (cbPayload == sizeof(ProbeManifest))
            {
                memcpy(&mf, pPayload, sizeof(ProbeManifest));
                gotManifest = true;
            }
        });

    if (!gotManifest)
    {
        fprintf(stderr, "[VERIFY] FAIL: manifest not found in %s\n", sManifestTopic.c_str());
        return 1;
    }

    printf("[VERIFY] Manifest:\n");
    printf("[VERIFY]   freq=%" PRIu64 " Hz  start_tick=%" PRIu64
           "  interval=%" PRIu64 " ticks (%" PRIu64 " us)\n",
           mf.qwFrequency, mf.qwStartTick,
           mf.qwTickInterval, (uint64_t)(mf.qwTickInterval * 1000000ULL / mf.qwFrequency));
    printf("[VERIFY]   total_samples=%" PRIu64 "  wID=%u\n",
           mf.qwTotalSamples, (unsigned)mf.wID);

    // Cross-validate CLI args against manifest
    bool bArgMismatch = false;
    if (nCount      != 0           && nCount      != mf.qwTotalSamples) {
        fprintf(stderr, "[VERIFY] WARN: --count %" PRIu64 " != manifest.qwTotalSamples=%" PRIu64 "\n",
                nCount, mf.qwTotalSamples);
        bArgMismatch = true;
    }
    if (qwStartTick != (uint64_t)-1 && qwStartTick != mf.qwStartTick) {
        fprintf(stderr, "[VERIFY] WARN: --start-tick %" PRIu64 " != manifest.qwStartTick=%" PRIu64 "\n",
                qwStartTick, mf.qwStartTick);
        bArgMismatch = true;
    }
    if (wID         != (uint16_t)-1 && wID         != mf.wID) {
        fprintf(stderr, "[VERIFY] WARN: --wid %u != manifest.wID=%u\n",
                (unsigned)wID, (unsigned)mf.wID);
        bArgMismatch = true;
    }
    if (bArgMismatch)
        fprintf(stderr, "[VERIFY] Proceeding with manifest values.\n");

    // -- Step 2: consume data topic and verify --------------------------------
    printf("[VERIFY] Consuming data topic %s...\n\n", sTopic.c_str());

    // Sequence tracking
    // wSeqN wraps at 65536 -- we unwrap to globalIndex using a running counter
    uint64_t nReceived      = 0;
    uint64_t nTimerErrors   = 0;  // qwTimer mismatch
    uint64_t nValueErrors   = 0;  // dbValue mismatch
    uint64_t nIDErrors      = 0;  // wID mismatch
    uint64_t nOrderErrors   = 0;  // messages arrived out of order
    uint64_t nDuplicates    = 0;  // same globalIndex seen twice

    uint64_t nextExpected   = 0;

    std::vector<bool> seen(mf.qwTotalSamples, false);

    // First few and last few timer values for eyeball verification
    static const int NSHOW = 6;
    struct Sample { uint64_t idx; uint64_t timer; double val; };
    std::vector<Sample> firstSamples, lastSamples;

    uint64_t nPollData  = 0;
    uint64_t nPollSmall = 0;

    consumer->ConsumeTopicFromBeginning(sTopic, 35000,
        [&](const void *pPayload, size_t cbPayload, int64_t)
        {
            if (cbPayload < sizeof(Kv8TelValue))
            {
                nPollSmall++;
                return;
            }
            nPollData++;
            const Kv8TelValue *pVal = reinterpret_cast<const Kv8TelValue *>(pPayload);

            // Unwrap wSeqN to globalIndex.
            uint64_t wrapCount = nReceived / 65536;
            uint64_t globalIdx = wrapCount * 65536 + pVal->wSeqN;
            if (globalIdx + 32768 < nReceived) globalIdx += 65536;

            nReceived++;

            if (globalIdx != nextExpected)
            {
                nOrderErrors++;
                if (nOrderErrors <= 5)
                    fprintf(stderr,
                            "[VERIFY] ORDER ERROR  idx=%" PRIu64
                            "  expected=%" PRIu64 "\n",
                            globalIdx, nextExpected);
            }
            nextExpected = globalIdx + 1;

            if (globalIdx < mf.qwTotalSamples)
            {
                if (seen[globalIdx]) {
                    nDuplicates++;
                    if (nDuplicates <= 5)
                        fprintf(stderr, "[VERIFY] DUPLICATE  idx=%" PRIu64 "\n", globalIdx);
                } else {
                    seen[globalIdx] = true;
                }
            }

            if (pVal->wID != mf.wID) {
                nIDErrors++;
                if (nIDErrors <= 5)
                    fprintf(stderr,
                            "[VERIFY] WID ERROR    idx=%" PRIu64
                            "  got=%u  expected=%u\n",
                            globalIdx, (unsigned)pVal->wID, (unsigned)mf.wID);
            }

            uint64_t expectedTimer = mf.qwStartTick + globalIdx * mf.qwTickInterval;
            if (pVal->qwTimer != expectedTimer) {
                nTimerErrors++;
                if (nTimerErrors <= 20)
                    fprintf(stderr,
                            "[VERIFY] TIMER ERROR  idx=%-10" PRIu64
                            "  got=%-20" PRIu64
                            "  expected=%-20" PRIu64
                            "  diff=%" PRId64 "\n",
                            globalIdx,
                            pVal->qwTimer, expectedTimer,
                            (int64_t)pVal->qwTimer - (int64_t)expectedTimer);
            }

            double expectedVal = (double)(globalIdx & 0x3FF);
            if (pVal->dbValue != expectedVal) {
                nValueErrors++;
                if (nValueErrors <= 10)
                    fprintf(stderr,
                            "[VERIFY] VALUE ERROR  idx=%-10" PRIu64
                            "  got=%.0f  expected=%.0f\n",
                            globalIdx, pVal->dbValue, expectedVal);
            }

            if ((int)firstSamples.size() < NSHOW)
                firstSamples.push_back({globalIdx, pVal->qwTimer, pVal->dbValue});
            lastSamples.push_back({globalIdx, pVal->qwTimer, pVal->dbValue});
            if ((int)lastSamples.size() > NSHOW)
                lastSamples.erase(lastSamples.begin());

            if (nReceived % 50000 == 0)
                printf("[VERIFY] progress: %" PRIu64 " / %" PRIu64 "\n",
                       nReceived, mf.qwTotalSamples);
        });

    // -- Count missing samples ------------------------------------------------
    uint64_t nMissing = 0;
    for (uint64_t i = 0; i < mf.qwTotalSamples; ++i)
        if (!seen[i]) nMissing++;

    // -- Print summary --------------------------------------------------------
    printf("\n[VERIFY] ====== Results ======\n");
    printf("[VERIFY]   expected     : %" PRIu64 "\n", mf.qwTotalSamples);
    printf("[VERIFY]   received     : %" PRIu64 "\n", nReceived);
    printf("[VERIFY]   missing      : %" PRIu64 "\n", nMissing);
    printf("[VERIFY]   duplicates   : %" PRIu64 "\n", nDuplicates);
    printf("[VERIFY]   order errors : %" PRIu64 "\n", nOrderErrors);
    printf("[VERIFY]   timer errors : %" PRIu64 "\n", nTimerErrors);
    printf("[VERIFY]   value errors : %" PRIu64 "\n", nValueErrors);
    printf("[VERIFY]   wID errors   : %" PRIu64 "\n", nIDErrors);

    printf("\n[VERIFY] First %d samples (idx | qwTimer | dbValue | timer_ok):\n",
           NSHOW);
    for (auto &s : firstSamples) {
        uint64_t exp = mf.qwStartTick + s.idx * mf.qwTickInterval;
        printf("[VERIFY]   idx=%-8" PRIu64
               "  qwTimer=%-12" PRIu64
               "  expected=%-12" PRIu64
               "  dbValue=%-6.0f  %s\n",
               s.idx, s.timer, exp,
               s.val, s.timer == exp ? "OK" : "MISMATCH");
    }

    printf("\n[VERIFY] Last %d samples (idx | qwTimer | dbValue | timer_ok):\n",
           NSHOW);
    for (auto &s : lastSamples) {
        uint64_t exp = mf.qwStartTick + s.idx * mf.qwTickInterval;
        printf("[VERIFY]   idx=%-8" PRIu64
               "  qwTimer=%-12" PRIu64
               "  expected=%-12" PRIu64
               "  dbValue=%-6.0f  %s\n",
               s.idx, s.timer, exp,
               s.val, s.timer == exp ? "OK" : "MISMATCH");
    }

    bool pass = (nReceived     == mf.qwTotalSamples)
             && (nMissing      == 0)
             && (nDuplicates   == 0)
             && (nOrderErrors  == 0)
             && (nTimerErrors  == 0)
             && (nValueErrors  == 0)
             && (nIDErrors     == 0);

    printf("\n[VERIFY] ====== OVERALL: %s ======\n\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

////////////////////////////////////////////////////////////////////////////////
// run_seqgap_mode -- sequence gap detection on kv8 telemetry
//
// Discovers sessions from the channel registry, reads all data topics from
// BEGINNING, and checks wSeqN continuity per counter (wID).
// Works with any kv8 telemetry -- does not require a kv8probe manifest.
////////////////////////////////////////////////////////////////////////////////

static int run_seqgap_mode(const Kv8Config &kCfg, const std::string &sChannel,
                           const std::string &sSession)
{
    std::string sChannelK = Kv8SanitizeChannel(sChannel);

    auto consumer = IKv8Consumer::Create(kCfg);
    if (!consumer) { fprintf(stderr, "[SEQGAP] FAIL: could not create consumer.\n"); return 1; }

    // -- Step 1: discover sessions -------------------------------------------
    printf("[SEQGAP] Discovering sessions on channel '%s' ...\n", sChannelK.c_str());

    auto sessions = consumer->DiscoverSessions(sChannelK);
    if (sessions.empty())
    {
        fprintf(stderr, "[SEQGAP] FAIL: no sessions found on channel '%s'\n",
                sChannelK.c_str());
        return 1;
    }

    printf("[SEQGAP] Found %zu session(s):\n", sessions.size());
    for (auto &kv : sessions)
        printf("[SEQGAP]   %-40s  data_topics=%zu  counters=%zu\n",
               kv.first.c_str(),
               kv.second.dataTopics.size(),
               kv.second.topicToCounter.size());

    // -- Step 2: select session ----------------------------------------------
    const SessionMeta *pTarget = nullptr;

    if (!sSession.empty())
    {
        // Exact match first, then prefix/substring match
        auto it = sessions.find(sSession);
        if (it != sessions.end())
        {
            pTarget = &it->second;
        }
        else
        {
            // Try substring match on session prefix
            for (auto &kv : sessions)
            {
                if (kv.first.find(sSession) != std::string::npos)
                {
                    pTarget = &kv.second;
                    printf("[SEQGAP] Matched session: %s\n", kv.first.c_str());
                    break;
                }
            }
        }

        if (!pTarget)
        {
            fprintf(stderr, "[SEQGAP] FAIL: session '%s' not found.\n",
                    sSession.c_str());
            return 1;
        }
    }
    else
    {
        // Auto-select: pick the last (newest) session
        pTarget = &sessions.rbegin()->second;
        printf("[SEQGAP] Auto-selected newest session: %s\n",
               sessions.rbegin()->first.c_str());
    }

    const SessionMeta &sm = *pTarget;

    printf("\n[SEQGAP] Session: %s\n", sm.sSessionPrefix.c_str());
    printf("[SEQGAP]   Display name : %s\n", sm.sName.c_str());
    printf("[SEQGAP]   Data topics  : %zu\n", sm.dataTopics.size());
    printf("[SEQGAP]   Log topic    : %s\n", sm.sLogTopic.c_str());
    printf("[SEQGAP]   Control topic: %s\n", sm.sControlTopic.c_str());

    if (sm.dataTopics.empty())
    {
        fprintf(stderr, "[SEQGAP] FAIL: session has no data topics.\n");
        return 1;
    }

    // -- Step 3: build counter name lookup from registry ----------------------
    // Map (topic, wID) -> human-readable counter name for reporting
    std::map<std::pair<std::string, uint16_t>, std::string> counterNames;
    for (auto &hc : sm.hashToCounters)
    {
        for (auto &cm : hc.second)
        {
            counterNames[{cm.sDataTopic, cm.wCounterID}] = cm.sName;
        }
    }

    // -- Step 4: read each data topic from beginning and track sequences ------
    // Key: (topic, wID) -> SeqTracker
    std::map<std::pair<std::string, uint16_t>, SeqTracker> trackers;

    uint64_t nTotalMessages   = 0;
    uint64_t nTotalUndersized = 0;

    for (auto &sTopic : sm.dataTopics)
    {
        printf("\n[SEQGAP] Reading topic: %s\n", sTopic.c_str());

        uint64_t nTopicMsg = 0;

        consumer->ConsumeTopicFromBeginning(sTopic, 45000,
            [&](const void *pPayload, size_t cbPayload, int64_t)
            {
                if (cbPayload < sizeof(Kv8TelValue))
                {
                    nTotalUndersized++;
                    return;
                }

                const Kv8TelValue *pVal =
                    reinterpret_cast<const Kv8TelValue *>(pPayload);

                auto key = std::make_pair(sTopic, pVal->wID);

                SeqTracker &tr = trackers[key];
                if (tr.nReceived == 0)
                {
                    // First time seeing this counter
                    tr.wID       = pVal->wID;
                    tr.sTopic    = sTopic;
                    tr.nReceived = 0;
                    tr.nGaps     = 0;
                    tr.nGapSamples  = 0;
                    tr.nDuplicates  = 0;
                    tr.nOutOfOrder  = 0;
                    tr.bFirstSeen   = true;
                    tr.wLastSeqN    = 0;
                    tr.qwFirstTimer = pVal->qwTimer;
                    tr.qwLastTimer  = pVal->qwTimer;
                }

                if (tr.bFirstSeen)
                {
                    // First message for this counter -- establish baseline
                    tr.bFirstSeen  = false;
                    tr.wLastSeqN   = pVal->wSeqN;
                    tr.qwFirstTimer = pVal->qwTimer;
                }
                else
                {
                    // Expected next wSeqN (wrapping at 65536)
                    uint16_t wExpected = (uint16_t)(tr.wLastSeqN + 1);

                    if (pVal->wSeqN == tr.wLastSeqN)
                    {
                        // Duplicate
                        tr.nDuplicates++;
                        if (tr.nDuplicates <= 5)
                            fprintf(stderr,
                                    "[SEQGAP] DUPLICATE  topic=%s  wID=%u"
                                    "  wSeqN=%u  msg#=%" PRIu64 "\n",
                                    sTopic.c_str(), (unsigned)pVal->wID,
                                    (unsigned)pVal->wSeqN, tr.nReceived);
                    }
                    else if (pVal->wSeqN != wExpected)
                    {
                        // Compute gap size (forward distance in mod-65536
                        // arithmetic)
                        int32_t diff = (int32_t)(uint16_t)(pVal->wSeqN - wExpected);
                        if (diff < 0) diff += 65536;

                        if (diff > 32768)
                        {
                            // wSeqN went backwards -- out of order
                            tr.nOutOfOrder++;
                            if (tr.nOutOfOrder <= 5)
                                fprintf(stderr,
                                        "[SEQGAP] OUT-OF-ORDER  topic=%s"
                                        "  wID=%u  wSeqN=%u  expected=%u\n",
                                        sTopic.c_str(),
                                        (unsigned)pVal->wID,
                                        (unsigned)pVal->wSeqN,
                                        (unsigned)wExpected);
                        }
                        else
                        {
                            // Forward gap -- missing samples
                            tr.nGaps++;
                            tr.nGapSamples += (uint64_t)diff;

                            if ((int)tr.gapLog.size() < SeqTracker::MAX_GAP_LOG)
                                tr.gapLog.push_back({wExpected, pVal->wSeqN,
                                                     (uint64_t)diff});

                            if (tr.nGaps <= 10)
                                fprintf(stderr,
                                        "[SEQGAP] GAP  topic=%s  wID=%u"
                                        "  expected=%u  got=%u"
                                        "  missed=%d  msg#=%" PRIu64 "\n",
                                        sTopic.c_str(),
                                        (unsigned)pVal->wID,
                                        (unsigned)wExpected,
                                        (unsigned)pVal->wSeqN,
                                        diff, tr.nReceived);
                        }
                    }

                    tr.wLastSeqN = pVal->wSeqN;
                }

                tr.qwLastTimer = pVal->qwTimer;
                tr.nReceived++;
                nTopicMsg++;
                nTotalMessages++;

                if (nTopicMsg % 50000 == 0)
                    printf("[SEQGAP]   progress: %" PRIu64 " messages\n",
                           nTopicMsg);
            });

        printf("[SEQGAP]   done: %" PRIu64 " data messages\n", nTopicMsg);
    }

    // -- Step 5: print per-counter results -----------------------------------
    printf("\n[SEQGAP] ====== Per-Counter Results ======\n\n");

    bool bOverallPass = true;
    uint64_t nTotalGaps       = 0;
    uint64_t nTotalGapSamples = 0;
    uint64_t nTotalDuplicates = 0;
    uint64_t nTotalOutOfOrder = 0;

    // Sort trackers by topic then wID for deterministic output
    std::vector<std::pair<std::pair<std::string, uint16_t>, SeqTracker*>> sorted;
    for (auto &kv : trackers)
        sorted.push_back({kv.first, &kv.second});
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    for (auto &entry : sorted)
    {
        SeqTracker &tr = *entry.second;

        // Look up human-readable counter name
        auto nameIt = counterNames.find(entry.first);
        const char *pName = (nameIt != counterNames.end())
                          ? nameIt->second.c_str() : "(unknown)";

        bool counterPass = (tr.nGaps == 0)
                        && (tr.nDuplicates == 0)
                        && (tr.nOutOfOrder == 0);

        if (!counterPass) bOverallPass = false;

        nTotalGaps       += tr.nGaps;
        nTotalGapSamples += tr.nGapSamples;
        nTotalDuplicates += tr.nDuplicates;
        nTotalOutOfOrder += tr.nOutOfOrder;

        printf("[SEQGAP]   wID=%-5u %-30s topic=%s\n",
               (unsigned)tr.wID, pName, tr.sTopic.c_str());
        printf("[SEQGAP]     received=%-10" PRIu64
               "  gaps=%-5" PRIu64
               "  missed=%-8" PRIu64
               "  dups=%-5" PRIu64
               "  ooo=%-5" PRIu64
               "  %s\n",
               tr.nReceived, tr.nGaps, tr.nGapSamples,
               tr.nDuplicates, tr.nOutOfOrder,
               counterPass ? "OK" : "FAIL");

        // Print gap details if any
        if (!tr.gapLog.empty())
        {
            printf("[SEQGAP]     gap details (first %d):\n",
                   (int)tr.gapLog.size());
            for (auto &g : tr.gapLog)
                printf("[SEQGAP]       expected=%5u  got=%5u  missed=%" PRIu64 "\n",
                       (unsigned)g.wExpected, (unsigned)g.wGot, g.nMissed);
        }
    }

    // -- Step 6: overall summary ---------------------------------------------
    printf("\n[SEQGAP] ====== Summary ======\n");
    printf("[SEQGAP]   session          : %s\n", sm.sSessionPrefix.c_str());
    printf("[SEQGAP]   data topics      : %zu\n", sm.dataTopics.size());
    printf("[SEQGAP]   counters tracked : %zu\n", trackers.size());
    printf("[SEQGAP]   total messages   : %" PRIu64 "\n", nTotalMessages);
    printf("[SEQGAP]   undersized msgs  : %" PRIu64 "\n", nTotalUndersized);
    printf("[SEQGAP]   total gaps       : %" PRIu64 "\n", nTotalGaps);
    printf("[SEQGAP]   total missed     : %" PRIu64 "\n", nTotalGapSamples);
    printf("[SEQGAP]   total duplicates : %" PRIu64 "\n", nTotalDuplicates);
    printf("[SEQGAP]   total out-of-ord : %" PRIu64 "\n", nTotalOutOfOrder);

    if (nTotalMessages > 0 && nTotalGapSamples > 0)
    {
        double dLossRate = 100.0 * (double)nTotalGapSamples
                         / (double)(nTotalMessages + nTotalGapSamples);
        printf("[SEQGAP]   loss rate        : %.4f%%\n", dLossRate);
    }

    printf("\n[SEQGAP] ====== OVERALL: %s ======\n\n",
           bOverallPass ? "PASS" : "FAIL");
    return bOverallPass ? 0 : 1;
}

////////////////////////////////////////////////////////////////////////////////
// main
////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::string sBrokers    = "localhost:19092";
    std::string sTopic;
    std::string sChannel;
    std::string sSession;
    std::string sSecProto   = "sasl_plaintext";
    std::string sSaslMech   = "PLAIN";
    std::string sUser       = "kv8producer";
    std::string sPass       = "kv8secret";
    // Optional overrides -- cross-validated against manifest if provided
    uint64_t    nCount      = 0;         // 0 = not specified
    uint64_t    qwStartTick = (uint64_t)-1; // -1 = not specified
    uint16_t    wID         = (uint16_t)-1; // -1 = not specified

    for (int i = 1; i < argc; ++i) {
        auto match = [&](const char *f){ return strcmp(argv[i], f) == 0; };
        auto next  = [&]() -> const char* { return (i+1 < argc) ? argv[++i] : nullptr; };
        if      (match("--brokers"))        { auto v = next(); if (v) sBrokers    = v; }
        else if (match("--topic"))          { auto v = next(); if (v) sTopic      = v; }
        else if (match("--channel"))        { auto v = next(); if (v) sChannel    = v; }
        else if (match("--session"))        { auto v = next(); if (v) sSession    = v; }
        else if (match("--count"))          { auto v = next(); if (v) nCount      = (uint64_t)strtoull(v, nullptr, 10); }
        else if (match("--start-tick"))     { auto v = next(); if (v) qwStartTick = (uint64_t)strtoull(v, nullptr, 10); }
        else if (match("--wid"))            { auto v = next(); if (v) wID         = (uint16_t)atoi(v); }
        else if (match("--security-proto")) { auto v = next(); if (v) sSecProto   = v; }
        else if (match("--sasl-mechanism")) { auto v = next(); if (v) sSaslMech   = v; }
        else if (match("--user"))           { auto v = next(); if (v) sUser       = v; }
        else if (match("--pass"))           { auto v = next(); if (v) sPass       = v; }
        else if (match("--help"))           { goto usage; }
        else { fprintf(stderr, "[VERIFY] unknown arg: %s\n", argv[i]); goto usage; }
    }

    if (sTopic.empty() && sChannel.empty()) {
    usage:
        fprintf(stderr,
            "Usage:\n"
            "  kv8verify --topic <name> [options]            Manifest mode (kv8probe data)\n"
            "  kv8verify --channel <name> [--session <id>]   Sequence gap mode (KV8 telemetry)\n"
            "\n"
            "Common options:\n"
            "  --brokers <b>          Bootstrap brokers [localhost:19092]\n"
            "  --security-proto <p>   plaintext|sasl_plaintext|sasl_ssl\n"
            "  --sasl-mechanism <m>   PLAIN|SCRAM-SHA-256|SCRAM-SHA-512\n"
            "  --user / --pass        SASL credentials\n"
            "\n"
            "Manifest mode only:\n"
            "  --count <N>            Expected sample count (cross-validated vs manifest)\n"
            "  --start-tick <t>       Expected initial qwTimer (cross-validated vs manifest)\n"
            "  --wid <id>             Expected counter wID (cross-validated vs manifest)\n"
            "\n"
            "Sequence gap mode only:\n"
            "  --channel <prefix>     Channel prefix (e.g. kv8/test or kv8.test)\n"
            "  --session <id>         Session ID to verify (default: newest)\n");
        return 1;
    }

    // -- Build Kafka config ---------------------------------------------------
    auto kCfg = kv8util::BuildKv8Config(sBrokers, sSecProto, sSaslMech, sUser, sPass);

    // -- Dispatch to appropriate mode -----------------------------------------
    if (!sChannel.empty())
        return run_seqgap_mode(kCfg, sChannel, sSession);
    else
        return run_manifest_mode(kCfg, sTopic, nCount, qwStartTick, wID);
}
