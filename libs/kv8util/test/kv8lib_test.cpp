////////////////////////////////////////////////////////////////////////////////
// kv8util_test -- End-to-end integration test for kv8util + libkv8
//
// Modelled on kv8bench, this test verifies the full Kafka produce/consume
// pipeline:
//
//   1. Connection check:    Create producer + consumer against a live broker.
//   2. Produce 100 000 messages with sequential nSeq.
//   3. Real-time gap check: Consumer runs concurrently; after production
//      finishes the test scans the received bitmap for missing sequences.
//   4. Post-production gap check: ConsumeTopicFromBeginning reads all
//      messages once more and verifies every sequence is present.
//   5. Clean disconnection: Stop consumer, destroy producer, delete topic.
//
// Exit code:
//   0 = PASS     All 100 000 messages received, zero gaps, clean exit.
//   0 = SKIP     Broker unreachable (logged, test aborted, not a failure).
//   1 = FAIL     Missing samples, creation failures, or timeout.
//
// Usage:
//   kv8lib_test [--brokers <b>] [--user <u>] [--pass <p>]
//               [--security-proto <s>] [--sasl-mechanism <m>]
//               [--count <N>]
//
// No global variables.  All state lives on the stack or in local objects.
////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#endif

#include <kv8/IKv8Producer.h>
#include <kv8/IKv8Consumer.h>

#include <kv8util/Kv8AppUtils.h>
#include <kv8util/Kv8BenchMsg.h>
#include <kv8util/Kv8Timer.h>
#include <kv8util/Kv8TopicUtils.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace kv8;
using namespace kv8util;

////////////////////////////////////////////////////////////////////////////////
// Helpers
////////////////////////////////////////////////////////////////////////////////

// Print a section header for readability.
static void Section(const char *pTitle)
{
    printf("\n================================================================\n");
    printf("  %s\n", pTitle);
    printf("================================================================\n");
}

////////////////////////////////////////////////////////////////////////////////
// main
////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    TimerInit();

    // -- CLI args (all optional) --------------------------------------------
    std::string sBrokers  = "localhost:19092";
    std::string sSecProto = "sasl_plaintext";
    std::string sSaslMech = "PLAIN";
    std::string sUser     = "kv8producer";
    std::string sPass     = "kv8secret";
    uint64_t    nCount    = 100000;

    for (int i = 1; i < argc; ++i) {
        auto match = [&](const char *f){ return strcmp(argv[i], f) == 0; };
        auto next  = [&]() -> const char* { return (i+1 < argc) ? argv[++i] : nullptr; };
        if      (match("--brokers"))        { auto v = next(); if (v) sBrokers  = v; }
        else if (match("--security-proto")) { auto v = next(); if (v) sSecProto = v; }
        else if (match("--sasl-mechanism")) { auto v = next(); if (v) sSaslMech = v; }
        else if (match("--user"))           { auto v = next(); if (v) sUser     = v; }
        else if (match("--pass"))           { auto v = next(); if (v) sPass     = v; }
        else if (match("--count"))          { auto v = next(); if (v) nCount    = (uint64_t)strtoull(v, nullptr, 10); }
        else if (match("--help"))           {
            fprintf(stderr,
                "Usage: kv8util_test [options]\n"
                "  --brokers <b>          Bootstrap brokers [localhost:19092]\n"
                "  --security-proto <p>   Security protocol [sasl_plaintext]\n"
                "  --sasl-mechanism <m>   SASL mechanism    [PLAIN]\n"
                "  --user / --pass        SASL credentials\n"
                "  --count <N>            Messages to produce [100000]\n");
            return 1;
        }
        else { fprintf(stderr, "[TEST] Unknown arg: %s\n", argv[i]); return 1; }
    }

    auto kCfg = BuildKv8Config(sBrokers, sSecProto, sSaslMech, sUser, sPass);

    Section("1. Connection check");

    // -- Step 1: verify broker reachability ---------------------------------
    printf("[TEST] Brokers : %s\n", sBrokers.c_str());
    printf("[TEST] Count   : %" PRIu64 "\n", nCount);

    auto producer = IKv8Producer::Create(kCfg);
    if (!producer)
    {
        printf("[TEST] SKIP: could not create producer -- broker unreachable.\n");
        printf("[TEST] Kafka server is not available. Test aborted (not a failure).\n");
        return 0;
    }

    auto testConsumer = IKv8Consumer::Create(kCfg);
    if (!testConsumer)
    {
        printf("[TEST] SKIP: could not create consumer -- broker unreachable.\n");
        printf("[TEST] Kafka server is not available. Test aborted (not a failure).\n");
        return 0;
    }
    testConsumer.reset(); // release early; we create a fresh one for the test
    printf("[TEST] Producer and consumer connected OK.\n");

    // Generate a unique topic to avoid collisions with other runs.
    std::string sTopic = GenerateTopicName("kv8test");
    printf("[TEST] Topic   : %s\n", sTopic.c_str());

    // -- Step 2: concurrent consumer thread ---------------------------------
    Section("2. Produce + concurrent consume (real-time gap check)");

    // Bitmap: one byte per sequence number.  Written by consumer thread only.
    auto pSeen = std::unique_ptr<uint8_t[]>(new uint8_t[nCount]());

    std::atomic<int64_t> nRecv{0};
    std::atomic<int64_t> nDuplicates{0};
    std::atomic<int64_t> nOutOfRange{0};
    std::atomic<bool>    bConsumerStop{false};

    // Consumer thread: subscribe + poll, recording every nSeq into pSeen[].
    std::thread consumerThread([&]()
    {
        auto consumer = IKv8Consumer::Create(kCfg);
        if (!consumer)
        {
            fprintf(stderr, "[TEST] consumer thread: create failed\n");
            bConsumerStop.store(true);
            return;
        }
        consumer->Subscribe(sTopic);

        int64_t nLocalRecv = 0;
        int64_t nLocalDups = 0;
        int64_t nLocalOOR  = 0;
        uint8_t *pBitmap   = pSeen.get();

        while (!bConsumerStop.load(std::memory_order_relaxed))
        {
            consumer->Poll(200, [&](std::string_view  /*topic*/,
                                    const void        *pPayload,
                                    size_t             cbPayload,
                                    int64_t            /*tsMs*/)
            {
                ++nLocalRecv;
                if (cbPayload < sizeof(BenchMsg)) return;

                BenchMsg msg;
                memcpy(&msg, pPayload, sizeof(BenchMsg));

                // Skip warmup sentinel.
                if (msg.nSeq == UINT64_MAX) return;

                if (msg.nSeq < nCount)
                {
                    if (pBitmap[msg.nSeq]) ++nLocalDups;
                    else                   pBitmap[msg.nSeq] = 1;
                }
                else
                {
                    ++nLocalOOR;
                }
            });

            nRecv.store(nLocalRecv, std::memory_order_relaxed);
            nDuplicates.store(nLocalDups, std::memory_order_relaxed);
            nOutOfRange.store(nLocalOOR, std::memory_order_relaxed);
        }

        // Final drain: keep polling briefly to catch in-flight messages.
        {
            const auto tEnd = std::chrono::steady_clock::now()
                            + std::chrono::seconds(5);
            int nEmpty = 0;
            while (nEmpty < 5 && std::chrono::steady_clock::now() < tEnd)
            {
                int nGot = 0;
                consumer->Poll(500, [&](std::string_view /*topic*/,
                                        const void       *pPayload,
                                        size_t            cbPayload,
                                        int64_t           /*tsMs*/)
                {
                    ++nGot;
                    ++nLocalRecv;
                    if (cbPayload < sizeof(BenchMsg)) return;
                    BenchMsg msg;
                    memcpy(&msg, pPayload, sizeof(BenchMsg));
                    if (msg.nSeq == UINT64_MAX) return;
                    if (msg.nSeq < nCount)
                    {
                        if (pBitmap[msg.nSeq]) ++nLocalDups;
                        else                   pBitmap[msg.nSeq] = 1;
                    }
                    else { ++nLocalOOR; }
                });
                if (nGot == 0) ++nEmpty;
                else           nEmpty = 0;
            }
        }

        nRecv.store(nLocalRecv, std::memory_order_relaxed);
        nDuplicates.store(nLocalDups, std::memory_order_relaxed);
        nOutOfRange.store(nLocalOOR, std::memory_order_relaxed);
        consumer->Stop();
    });

    // Give consumer time to subscribe and get partition assignment.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // -- Step 2b: warmup (wait for consumer to be ready) --------------------
    {
        printf("[TEST] Warming up consumer (waiting for partition assignment)...\n");
        BenchMsg warmup{};
        warmup.qSendTick   = TimerNow();
        warmup.tSendWallMs = QpcToWallMs(warmup.qSendTick);
        warmup.nSeq        = UINT64_MAX; // sentinel
        while (!producer->Produce(sTopic, &warmup, sizeof(warmup),
                                  &warmup.nSeq, sizeof(warmup.nSeq)))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        producer->Flush(5000);

        const auto tWarmDeadline = std::chrono::steady_clock::now()
                                 + std::chrono::seconds(30);
        while (nRecv.load(std::memory_order_relaxed) == 0)
        {
            if (std::chrono::steady_clock::now() >= tWarmDeadline)
            {
                fprintf(stderr, "[TEST] WARNING: consumer did not receive warmup "
                                "within 30 s -- proceeding.\n");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Reset counter (warmup sentinel had nSeq=UINT64_MAX, so pSeen is clean).
        nRecv.store(0, std::memory_order_relaxed);
        printf("[TEST] Consumer ready.\n");
    }

    // -- Step 2c: produce 100K messages -------------------------------------
    printf("[TEST] Producing %" PRIu64 " messages...\n", nCount);

    const auto tProdStart = std::chrono::steady_clock::now();
    int64_t nQueueFull = 0;

    for (uint64_t i = 0; i < nCount; ++i)
    {
        BenchMsg msg;
        msg.qSendTick   = TimerNow();
        msg.tSendWallMs = QpcToWallMs(msg.qSendTick);
        msg.nSeq        = i;

        if (!producer->Produce(sTopic, &msg, sizeof(msg),
                               &msg.nSeq, sizeof(msg.nSeq)))
        {
            ++nQueueFull;
            // Back off briefly and retry once.
            producer->Flush(100);
            if (!producer->Produce(sTopic, &msg, sizeof(msg),
                                   &msg.nSeq, sizeof(msg.nSeq)))
            {
                fprintf(stderr, "[TEST] FAILED to produce seq %" PRIu64 "\n", i);
            }
        }

        // Periodic flush to avoid unbounded librdkafka queue growth.
        if ((i & 0xFFF) == 0xFFF)
            producer->Flush(0);

        // Progress report every 10K.
        if ((i + 1) % 10000 == 0)
        {
            int64_t recv = nRecv.load(std::memory_order_relaxed);
            printf("[TEST]   sent=%-8" PRIu64 "  recv=%-8" PRId64 "  lag=%-8" PRId64 "\n",
                   i + 1, recv, (int64_t)(i + 1) - recv);
        }
    }

    // Final flush: wait for all messages to reach the broker.
    printf("[TEST] Flushing producer...\n");
    producer->Flush(30000);

    const auto tProdEnd = std::chrono::steady_clock::now();
    double dProdMs = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                        tProdEnd - tProdStart).count();
    printf("[TEST] Produce complete. %.1f ms, queue_full=%" PRId64 "\n",
           dProdMs, nQueueFull);

    // Let consumer drain for a few seconds.
    printf("[TEST] Draining consumer...\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    bConsumerStop.store(true, std::memory_order_relaxed);
    consumerThread.join();

    // -- Step 3: real-time gap analysis -------------------------------------
    Section("3. Real-time gap analysis");

    int64_t nRtMissing = 0;
    for (uint64_t i = 0; i < nCount; ++i)
        if (!pSeen[i]) ++nRtMissing;

    int64_t nRtRecv = nRecv.load();
    int64_t nRtDups = nDuplicates.load();
    int64_t nRtOOR  = nOutOfRange.load();

    printf("[TEST] Real-time consumer results:\n");
    printf("[TEST]   expected     : %" PRIu64 "\n", nCount);
    printf("[TEST]   received     : %" PRId64 "\n", nRtRecv);
    printf("[TEST]   unique seen  : %" PRId64 "\n", (int64_t)nCount - nRtMissing);
    printf("[TEST]   missing      : %" PRId64 "\n", nRtMissing);
    printf("[TEST]   duplicates   : %" PRId64 "\n", nRtDups);
    printf("[TEST]   out-of-range : %" PRId64 "\n", nRtOOR);

    bool bRtPass = (nRtMissing == 0) && (nRtDups == 0) && (nRtOOR == 0);
    printf("[TEST]   RESULT       : %s\n", bRtPass ? "PASS" : "FAIL");

    // -- Step 4: post-production verification (ConsumeTopicFromBeginning) ----
    Section("4. Post-production verification");

    printf("[TEST] Re-reading topic from beginning...\n");

    auto ppSeen = std::unique_ptr<uint8_t[]>(new uint8_t[nCount]());
    int64_t nPpRecv = 0;
    int64_t nPpDups = 0;
    int64_t nPpOOR  = 0;
    uint8_t *pPpBitmap = ppSeen.get();

    {
        auto ppConsumer = IKv8Consumer::Create(kCfg);
        if (!ppConsumer)
        {
            fprintf(stderr, "[TEST] FAIL: could not create post-production consumer.\n");
            return 1;
        }

        ppConsumer->ConsumeTopicFromBeginning(sTopic, 30000,
            [&](const void *pPayload, size_t cbPayload, int64_t /*tsMs*/)
            {
                ++nPpRecv;
                if (cbPayload < sizeof(BenchMsg)) return;

                BenchMsg msg;
                memcpy(&msg, pPayload, sizeof(BenchMsg));

                // Skip the warmup sentinel.
                if (msg.nSeq == UINT64_MAX) return;

                if (msg.nSeq < nCount)
                {
                    if (pPpBitmap[msg.nSeq]) ++nPpDups;
                    else                     pPpBitmap[msg.nSeq] = 1;
                }
                else
                {
                    ++nPpOOR;
                }
            });
    }

    int64_t nPpMissing = 0;
    for (uint64_t i = 0; i < nCount; ++i)
        if (!ppSeen[i]) ++nPpMissing;

    printf("[TEST] Post-production results:\n");
    printf("[TEST]   expected     : %" PRIu64 "\n", nCount);
    printf("[TEST]   received     : %" PRId64 "\n", nPpRecv);
    printf("[TEST]   unique seen  : %" PRId64 "\n", (int64_t)nCount - nPpMissing);
    printf("[TEST]   missing      : %" PRId64 "\n", nPpMissing);
    printf("[TEST]   duplicates   : %" PRId64 "\n", nPpDups);
    printf("[TEST]   out-of-range : %" PRId64 "\n", nPpOOR);

    bool bPpPass = (nPpMissing == 0) && (nPpDups == 0) && (nPpOOR == 0);
    printf("[TEST]   RESULT       : %s\n", bPpPass ? "PASS" : "FAIL");

    // -- Step 5: clean disconnection + topic cleanup ------------------------
    Section("5. Clean disconnection");

    printf("[TEST] Destroying producer...\n");
    producer.reset();
    printf("[TEST] Producer destroyed.\n");

    // Delete the test topic to keep the broker clean.
    printf("[TEST] Deleting test topic '%s'...\n", sTopic.c_str());
    {
        auto cleaner = IKv8Consumer::Create(kCfg);
        if (cleaner)
        {
            SessionMeta sm;
            sm.sSessionID     = sTopic;
            sm.sSessionPrefix = sTopic;
            sm.sLogTopic      = sTopic + ".__log_unused";
            sm.sControlTopic  = sTopic + ".__ctrl_unused";
            sm.dataTopics.insert(sTopic);
            cleaner->DeleteSessionTopics(sm);
            printf("[TEST] Topic deleted.\n");
        }
        else
        {
            fprintf(stderr, "[TEST] WARNING: could not create cleaner consumer.\n");
        }
    }

    // -- Final verdict -------------------------------------------------------
    Section("FINAL VERDICT");

    bool bOverall = bRtPass && bPpPass;
    printf("[TEST] Real-time check  : %s\n", bRtPass ? "PASS" : "FAIL");
    printf("[TEST] Post-production  : %s\n", bPpPass ? "PASS" : "FAIL");
    printf("[TEST] ====== OVERALL: %s ======\n\n", bOverall ? "PASS" : "FAIL");

    return bOverall ? 0 : 1;
}
