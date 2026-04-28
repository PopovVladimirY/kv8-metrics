////////////////////////////////////////////////////////////////////////////////
// kv8log/test/test_log_e2e.cpp
//
// Phase L5 integration test: produce one record per severity level, consume
// from the resulting <ch>.<sid>._log topic, and verify every field.
//
// Skips (exit 0) when the broker is unreachable so CI without Docker stays
// green.  Requires kv8log_runtime.dll alongside the test executable -- the
// build system places both in the same directory automatically.
////////////////////////////////////////////////////////////////////////////////

#include "BrokerCheck.h"

#include "kv8log/KV8_Log.h"

#include <kv8/IKv8Consumer.h>
#include <kv8/Kv8Types.h>
#include <kv8util/Kv8AppUtils.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

// Argument parsing (mirrors kv8cli/kv8feeder defaults).
struct Args
{
    std::string sBrokers = "localhost:19092";
    std::string sUser    = "kv8producer";
    std::string sPass    = "kv8secret";
    // The default channel is hardcoded by the kv8log runtime to
    // "kv8log/<exe_name>" -> sanitized to "kv8log.<exe_name>".  Match that
    // here so we can find and clean up our own session reliably.
    std::string sChannel = "kv8log.test_log_e2e";
};

static Args ParseArgs(int argc, char** argv)
{
    Args a;
    for (int i = 1; i < argc; ++i)
    {
        const std::string s = argv[i];
        auto eat = [&](const char* k, std::string& dst) -> bool {
            if (s == k && i + 1 < argc) { dst = argv[++i]; return true; }
            return false;
        };
        if (eat("--brokers", a.sBrokers)) continue;
        if (eat("--user",    a.sUser))    continue;
        if (eat("--pass",    a.sPass))    continue;
    }
    return a;
}

int main(int argc, char** argv)
{
    Args a = ParseArgs(argc, argv);

    if (!BrokerAvailable(a.sBrokers, a.sUser, a.sPass))
        return 0;  // SKIP

    // ── Pre-clean: drop any leftover channel from a prior run ──────────────
    {
        auto kCfg = kv8util::BuildKv8Config(
            a.sBrokers, "sasl_plaintext", "PLAIN", a.sUser, a.sPass, "");
        auto cleaner = kv8::IKv8Consumer::Create(kCfg);
        try { cleaner->DeleteChannel(a.sChannel); } catch (...) {}
    }

    // ── Producer side ──────────────────────────────────────────────────────
    // KV8_LOG_CONFIGURE sets brokers/user/pass; the channel name is taken
    // from the executable name by the kv8log runtime (kv8log/<exe>).
    KV8_LOG_CONFIGURE(a.sBrokers.c_str(), a.sChannel.c_str(),
                      a.sUser.c_str(), a.sPass.c_str());

    // Five distinct call sites -- one per severity.  Each owns its own static
    // atomic and emits exactly one record.  Lines must stay distinct: that's
    // what makes the FNV-32 site hashes unique.
    KV8_LOG_DEBUG("e2e debug message");
    KV8_LOG_INFO ("e2e info message");
    KV8_LOG_WARN ("e2e warning message");
    KV8_LOG_ERROR("e2e error message");
    KV8_LOG_FATAL("e2e fatal message");

    KV8_TEL_FLUSH();

    // ── Consumer side ──────────────────────────────────────────────────────
    auto kCfg = kv8util::BuildKv8Config(
        a.sBrokers, "sasl_plaintext", "PLAIN", a.sUser, a.sPass, "");
    auto consumer = kv8::IKv8Consumer::Create(kCfg);
    EXPECT(consumer != nullptr);

    // Brand-new topics need a moment for broker metadata to propagate to a
    // freshly-created consumer.  Retry DiscoverSessions a few times before
    // giving up.
    std::map<std::string, kv8::SessionMeta> mSessions;
    for (int attempt = 0; attempt < 15; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        try { mSessions = consumer->DiscoverSessions(a.sChannel); }
        catch (...) { mSessions.clear(); }
        if (!mSessions.empty()) break;
        fprintf(stdout, "[INFO] DiscoverSessions attempt %d: no sessions yet\n",
                attempt + 1);
    }
    EXPECT(!mSessions.empty());
    if (mSessions.size() != 1)
        fprintf(stdout, "[INFO] discovered %zu sessions, using the first\n",
                mSessions.size());

    const auto& sm = mSessions.begin()->second;
    EXPECT(!sm.sLogTopic.empty());
    fprintf(stdout, "[INFO] consuming log topic: %s\n", sm.sLogTopic.c_str());

    // Drain the log topic from the beginning into a vector for inspection.
    struct Decoded
    {
        kv8::Kv8LogRecord hdr;
        std::string       sPayload;
    };
    std::vector<Decoded> vDecoded;

    for (int attempt = 0; attempt < 10 && vDecoded.size() < 5; ++attempt)
    {
        vDecoded.clear();
        consumer->ConsumeTopicFromBeginning(
            sm.sLogTopic, 5000,
            [&](const void* pPayload, size_t cbPayload, int64_t /*tsMs*/)
            {
                kv8::Kv8LogRecord hdr{};
                std::string_view  view;
                if (!kv8::Kv8DecodeLogRecord(
                        static_cast<const uint8_t*>(pPayload), cbPayload, hdr, view))
                {
                    fprintf(stderr, "[WARN] malformed log record (cb=%zu) skipped\n",
                            cbPayload);
                    return;
                }
                Decoded d;
                d.hdr      = hdr;
                d.sPayload = std::string(view);
                vDecoded.push_back(std::move(d));
            });
        if (vDecoded.size() >= 5) break;
        fprintf(stdout, "[INFO] log topic attempt %d: %zu record(s) so far\n",
                attempt + 1, vDecoded.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    fprintf(stdout, "[INFO] decoded %zu log record(s)\n", vDecoded.size());

    // ── Verifications ──────────────────────────────────────────────────────
    EXPECT(vDecoded.size() == 5);

    // The records may arrive in any order at the broker (single-thread emit
    // typically preserves order, but be tolerant): check the *set* of levels.
    std::set<uint8_t>  setLevels;
    std::set<uint32_t> setHashes;
    const uint64_t qwNowNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    for (const auto& d : vDecoded)
    {
        setLevels.insert(d.hdr.bLevel);
        setHashes.insert(d.hdr.dwSiteHash);

        EXPECT(d.hdr.dwMagic    == kv8::KV8_LOG_MAGIC);
        EXPECT(d.hdr.dwSiteHash != 0u);
        EXPECT(d.hdr.bFlags     == kv8::KV8_LOG_FLAG_TEXT);
        EXPECT(d.hdr.wReserved  == 0);
        EXPECT(d.hdr.tsNs > 0ULL);
        // tsNs must be in the recent past, never in the future (allow 5s skew).
        EXPECT(d.hdr.tsNs < qwNowNs + 5ULL * 1000000000ULL);
        EXPECT(!d.sPayload.empty());
    }

    // All 5 severity values present.
    EXPECT(setLevels.count(static_cast<uint8_t>(kv8::Kv8LogLevel::Debug))   == 1);
    EXPECT(setLevels.count(static_cast<uint8_t>(kv8::Kv8LogLevel::Info))    == 1);
    EXPECT(setLevels.count(static_cast<uint8_t>(kv8::Kv8LogLevel::Warning)) == 1);
    EXPECT(setLevels.count(static_cast<uint8_t>(kv8::Kv8LogLevel::Error))   == 1);
    EXPECT(setLevels.count(static_cast<uint8_t>(kv8::Kv8LogLevel::Fatal))   == 1);

    // Each call site has a distinct FNV-32 hash (no collisions across the 5
    // adjacent source lines emitted above).
    EXPECT(setHashes.size() == 5);

    // ── Cleanup ────────────────────────────────────────────────────────────
    // Best-effort delete of the per-session topics + the channel registry.
    try { consumer->DeleteSessionTopics(sm); } catch (...) {}
    try { consumer->DeleteChannel(a.sChannel); } catch (...) {}

    fprintf(stdout, "[PASS] test_log_e2e: 5 severity levels delivered "
                    "with correct wire fields\n");
    return 0;
}
