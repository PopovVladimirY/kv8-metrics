// kv8scope -- Kv8 Software Oscilloscope
// ConsumerThread.cpp -- Background Kafka consumer thread for one session.

#include "ConsumerThread.h"
#include "AnnotationStore.h"
#include "ConfigStore.h"
#include "StatsEngine.h"

#include <kv8/IKv8Consumer.h>
#include <kv8/Kv8Types.h>

#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ConsumerThread::ConsumerThread(const kv8::SessionMeta& meta,
                               const ConfigStore*       pConfig)
    : m_meta(meta)
    , m_pConfig(pConfig)
{
    InitTimeConverters();
    InitRingBuffers();
}

ConsumerThread::~ConsumerThread()
{
    RequestStop();
    Join();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ConsumerThread::Start()
{
    m_bStop.store(false, std::memory_order_relaxed);
    m_bDone.store(false, std::memory_order_relaxed);
    m_thread = std::thread(&ConsumerThread::Run, this);
}

void ConsumerThread::RequestStop()
{
    m_bStop.store(true, std::memory_order_relaxed);
}

void ConsumerThread::Join()
{
    if (m_thread.joinable())
        m_thread.join();
}

// ---------------------------------------------------------------------------
// Ring buffer access (main / render thread)
// ---------------------------------------------------------------------------

SpscRingBuffer<TelemetrySample>*
ConsumerThread::GetRingBuffer(uint32_t dwHash, uint16_t wCounterID)
{
    auto it = m_ringBuffers.find(MakeKey(dwHash, wCounterID));
    return (it != m_ringBuffers.end()) ? it->second.get() : nullptr;
}

std::vector<std::pair<uint32_t, uint16_t>>
ConsumerThread::GetCounterKeys() const
{
    std::vector<std::pair<uint32_t, uint16_t>> keys;
    keys.reserve(m_ringBuffers.size());
    for (const auto& kv : m_ringBuffers)
    {
        uint32_t dwHash = static_cast<uint32_t>(kv.first >> 16);
        uint16_t wID    = static_cast<uint16_t>(kv.first & 0xFFFF);
        keys.emplace_back(dwHash, wID);
    }
    return keys;
}

// ---------------------------------------------------------------------------
// Initialisation helpers (called from constructor, main thread)
// ---------------------------------------------------------------------------

void ConsumerThread::InitTimeConverters()
{
    m_topicCtx.reserve(m_meta.dataTopics.size());

    for (const auto& sTopic : m_meta.dataTopics)
    {
        TopicCtx ctx;
        ctx.sTopic = sTopic;

        // --- Resolve group hash for this topic ---
        // topicToGroupHash is keyed by group-level topics (0xFFFF records).
        // Per-counter topics are NOT in that map.  For those, scan hashToCounters
        // to find which group owns the counter on this topic.
        {
            auto itHash = m_meta.topicToGroupHash.find(sTopic);
            if (itHash != m_meta.topicToGroupHash.end())
            {
                ctx.dwHash = itHash->second;
            }
            else
            {
                // Per-counter topic: find hash via hashToCounters.
                for (const auto& [hash, counters] : m_meta.hashToCounters)
                {
                    for (const auto& cm : counters)
                    {
                        if (cm.sDataTopic == sTopic)
                        {
                            ctx.dwHash = hash;
                            break;
                        }
                    }
                    if (ctx.dwHash != 0) break;
                }
            }
        }

        // --- Resolve timer metadata for this topic ---
        // Timer anchor (frequency, QPC anchor, FILETIME) is stored on the
        // group-level topic entry, not on per-counter topic entries.
        // If the direct lookup misses, find the group topic with the same hash.
        std::string sTimerTopic = sTopic;
        if (ctx.dwHash != 0 &&
            m_meta.topicToFrequency.find(sTopic) == m_meta.topicToFrequency.end())
        {
            for (const auto& [gt, hash] : m_meta.topicToGroupHash)
            {
                if (hash == ctx.dwHash)
                {
                    sTimerTopic = gt;
                    break;
                }
            }
        }

        auto itFreq  = m_meta.topicToFrequency.find(sTimerTopic);
        auto itTimer = m_meta.topicToTimerValue.find(sTimerTopic);
        auto itHi    = m_meta.topicToTimeHi.find(sTimerTopic);
        auto itLo    = m_meta.topicToTimeLo.find(sTimerTopic);

        // Fallback: if this topic has no dedicated timing record (e.g. UDT topics),
        // use the first available group timing -- all topics in a session share
        // the same QPC reference.
        if (itFreq == m_meta.topicToFrequency.end() && !m_meta.topicToFrequency.empty())
        {
            const auto& first = *m_meta.topicToFrequency.begin();
            sTimerTopic = first.first;
            itFreq  = m_meta.topicToFrequency.find(sTimerTopic);
            itTimer = m_meta.topicToTimerValue.find(sTimerTopic);
            itHi    = m_meta.topicToTimeHi.find(sTimerTopic);
            itLo    = m_meta.topicToTimeLo.find(sTimerTopic);
        }

        if (itFreq  != m_meta.topicToFrequency.end()  &&
            itTimer != m_meta.topicToTimerValue.end() &&
            itHi    != m_meta.topicToTimeHi.end()     &&
            itLo    != m_meta.topicToTimeLo.end())
        {
            ctx.converter.Init(itFreq->second,
                               itTimer->second,
                               itHi->second,
                               itLo->second);
        }
        else
        {
            fprintf(stderr,
                    "[kv8scope] ConsumerThread: missing timer metadata for "
                    "topic \"%s\" (timer topic \"%s\") -- timestamps unavailable.\n",
                    sTopic.c_str(), sTimerTopic.c_str());
        }

        m_topicCtx.push_back(std::move(ctx));
    }
}

void ConsumerThread::InitRingBuffers()
{
    for (const auto& kv : m_meta.hashToCounters)
    {
        uint32_t dwHash = kv.first;
        for (const auto& cm : kv.second)
        {
            // UDT feed descriptors are not scalar traces; only virtual
            // scalar fields (bIsUdtVirtualField) and regular counters get ring buffers.
            if (cm.bIsUdtFeed)
                continue;

            uint64_t key = MakeKey(dwHash, cm.wCounterID);
            if (m_ringBuffers.find(key) == m_ringBuffers.end())
            {
                m_ringBuffers.emplace(
                    key,
                    std::make_unique<SpscRingBuffer<TelemetrySample>>(
                        kRingCapacity));
            }

            // Build UDT field decode context for hot-path decoding.
            if (cm.bIsUdtVirtualField && !cm.sDataTopic.empty())
            {
                UdtFieldDecodeCtx fc;
                fc.nKey       = key;
                fc.dwHash     = dwHash;
                fc.wCounterID = cm.wCounterID;
                fc.wOffset    = cm.wUdtFieldOffset;
                fc.nFieldType = cm.nUdtFieldType;
                m_udtTopicFields[cm.sDataTopic].push_back(fc);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Background thread entry point
// ---------------------------------------------------------------------------

void ConsumerThread::Run()
{
    // Build Kv8Config from the application config.
    kv8::Kv8Config cfg;
    const auto& sc     = m_pConfig->Get();
    cfg.sBrokers       = sc.sBrokers;
    cfg.sSecurityProto = sc.sSecurityProtocol;
    cfg.sSaslMechanism = sc.sSaslMechanism;
    cfg.sUser          = sc.sUsername;
    cfg.sPass          = sc.sPassword;
    // Unique consumer group per scope window so offset tracking is
    // independent across simultaneously opened sessions.
    cfg.sGroupID       = "kv8scope-" + m_meta.sSessionPrefix;

    auto pConsumer = kv8::IKv8Consumer::Create(cfg);
    if (!pConsumer)
    {
        fprintf(stderr,
                "[kv8scope] ConsumerThread: failed to create IKv8Consumer "
                "for session \"%s\".\n",
                m_meta.sSessionPrefix.c_str());
        return;
    }

    // Subscribe to every data topic in the session -- both scalar (.d.) and UDT (.u.).
    for (const auto& sTopic : m_meta.dataTopics)
    {
        pConsumer->Subscribe(sTopic);
    }

    // Subscribe to annotation + ctl topics for live updates immediately.
    // Historical replay is done on a background thread (Fix 4 -- KV8_UDT_STALL)
    // so the PollBatch hot loop can start without waiting for replay to finish.
    if (m_pAnnotationStore && !m_sAnnotationTopic.empty())
        pConsumer->Subscribe(m_sAnnotationTopic);
    if (!m_sCtlTopic.empty())
        pConsumer->Subscribe(m_sCtlTopic);

    m_bConnected.store(true, std::memory_order_relaxed);

    // Launch background replay thread for annotation & ctl history.
    // Uses a separate temporary IKv8Consumer so it does not interfere with
    // the main PollBatch loop.  Both PushFromKafka and ParseAndQueueCtl
    // are thread-safe (PushFromKafka operates on AnnotationStore's internal
    // mutex; ParseAndQueueCtl acquires m_ctlMu).
    std::thread replayThread;
    if (!m_bStop.load(std::memory_order_relaxed))
    {
        // Capture config by value for the replay thread.
        kv8::Kv8Config replayCfg = cfg;
        replayCfg.sGroupID = cfg.sGroupID + "-replay";

        replayThread = std::thread([this, replayCfg = std::move(replayCfg)]()
        {
            auto pReplay = kv8::IKv8Consumer::Create(replayCfg);
            if (!pReplay)
                return;

            if (m_pAnnotationStore && !m_sAnnotationTopic.empty()
                && !m_bStop.load(std::memory_order_relaxed))
            {
                pReplay->ConsumeTopicFromBeginning(
                    m_sAnnotationTopic,
                    2000,   // 2 s hard timeout (Fix 3)
                    [this](const void* pData, size_t cbData,
                           int64_t /*tsKafkaMs*/)
                    {
                        m_pAnnotationStore->PushFromKafka(
                            std::string{}, pData, cbData);
                    });
            }

            if (!m_sCtlTopic.empty()
                && !m_bStop.load(std::memory_order_relaxed))
            {
                pReplay->ConsumeTopicFromBeginning(
                    m_sCtlTopic,
                    1500,   // 1.5 s hard timeout (Fix 3)
                    [this](const void* pData, size_t cbData,
                           int64_t /*tsKafkaMs*/)
                    {
                        ParseAndQueueCtl(pData, cbData);
                    });
            }

            pReplay->Stop();
        });
    }

    // Pre-build the message callback outside the loop so the lambda is
    // constructed once (avoids repeated std::function wrapping).
    auto onMessage = [this](std::string_view sTopic,
                            const void*      pPayload,
                            size_t           cbPayload,
                            int64_t          /*tsKafkaMs*/)
    {
        // ---- Counter control (._ctl topic, JSON records) -----------------
        if (!m_sCtlTopic.empty() &&
            sTopic.size() == m_sCtlTopic.size() &&
            sTopic == m_sCtlTopic)
        {
            ParseAndQueueCtl(pPayload, cbPayload);
            return;
        }

        // ---- Annotation topic (JSON text records, variable size) ----------
        if (m_pAnnotationStore &&
            !m_sAnnotationTopic.empty() &&
            sTopic.size() == m_sAnnotationTopic.size() &&
            sTopic == m_sAnnotationTopic)
        {
            m_pAnnotationStore->PushFromKafka(std::string{}, pPayload, cbPayload);
            return;
        }

        // ---- UDT sample (binary Kv8UDTSample header + packed payload) ------
        // UDT topics carry ".u." in the name. Decode each virtual scalar field
        // using the pre-built m_udtTopicFields lookup table.
        if (sTopic.find(".u.") != std::string_view::npos)
        {
            if (cbPayload < sizeof(kv8::Kv8UDTSample))
                return;

            const auto* pUdt =
                reinterpret_cast<const kv8::Kv8UDTSample*>(pPayload);

            if (kv8::Kv8GetExtSubtype(pUdt->sCommonRaw) != kv8::KV8_TEL_TYPE_UDT)
                return;

            const uint32_t totalMsg = kv8::Kv8GetExtSize(pUdt->sCommonRaw);
            if (totalMsg < sizeof(kv8::Kv8UDTSample) ||
                totalMsg > cbPayload)
                return;
            const uint32_t dataBytes =
                static_cast<uint32_t>(totalMsg - sizeof(kv8::Kv8UDTSample));

            // Find the topic context for timestamp conversion.
            const TopicCtx* pCtx = nullptr;
            {
                const std::string sTopicStr(sTopic);
                for (const auto& ctx : m_topicCtx)
                    if (ctx.sTopic == sTopicStr) { pCtx = &ctx; break; }
            }
            if (!pCtx || !pCtx->converter.IsValid())
                return;

            const double dTimestamp = pCtx->converter.Convert(pUdt->qwTimer);
            const auto*  pData      =
                reinterpret_cast<const uint8_t*>(pPayload) + sizeof(kv8::Kv8UDTSample);

            // Decode each registered virtual field for this topic.
            const std::string sTopicStr(sTopic);
            auto tit = m_udtTopicFields.find(sTopicStr);
            if (tit == m_udtTopicFields.end())
                return;

            for (const auto& fc : tit->second)
            {
                const uint16_t wireSize = kv8::Kv8UdtFieldWireSize(fc.nFieldType);
                if (static_cast<uint32_t>(fc.wOffset) + wireSize > dataBytes)
                    continue;

                const double dVal =
                    kv8::Kv8DecodeUdtField(pData + fc.wOffset, fc.nFieldType);

                auto rbit = m_ringBuffers.find(fc.nKey);
                if (rbit == m_ringBuffers.end())
                    continue;

                TelemetrySample sample;
                sample.dTimestamp = dTimestamp;
                sample.dValue     = dVal;
                if (!rbit->second->Push(sample))
                    m_nDropped.fetch_add(1, std::memory_order_relaxed);

                if (m_pStatsEngine)
                    m_pStatsEngine->Feed(fc.dwHash, fc.wCounterID, dVal);
            }

            m_nTotalMessages.fetch_add(1, std::memory_order_relaxed);
            m_dLastMsgTime.store(dTimestamp, std::memory_order_relaxed);
            return;
        }

        // ---- Telemetry data (binary Kv8TelValue records) ----------------
        if (cbPayload < sizeof(kv8::Kv8TelValue))
            return;

        const auto* pVal =
            reinterpret_cast<const kv8::Kv8TelValue*>(pPayload);

        // Find the pre-computed topic context (linear scan; N is small).
        const TopicCtx* pCtx = nullptr;
        for (const auto& ctx : m_topicCtx)
        {
            if (ctx.sTopic.size() == sTopic.size() &&
                ctx.sTopic == sTopic)
            {
                pCtx = &ctx;
                break;
            }
        }
        if (!pCtx || !pCtx->converter.IsValid())
            return;

        // Convert QPC tick to wall-clock seconds.
        double dTimestamp = pCtx->converter.Convert(pVal->qwTimer);

        // Route to the per-counter ring buffer.
        uint64_t key = MakeKey(pCtx->dwHash, pVal->wID);
        auto it = m_ringBuffers.find(key);
        if (it == m_ringBuffers.end())
            return;   // unknown counter -- skip

        TelemetrySample sample;
        sample.dTimestamp = dTimestamp;
        sample.dValue     = pVal->dbValue;

        if (!it->second->Push(sample))
            m_nDropped.fetch_add(1, std::memory_order_relaxed);

        // Update full-duration running accumulators (O(1) per sample).
        if (m_pStatsEngine)
            m_pStatsEngine->Feed(pCtx->dwHash, pVal->wID, pVal->dbValue);

        m_nTotalMessages.fetch_add(1, std::memory_order_relaxed);
        m_dLastMsgTime.store(dTimestamp, std::memory_order_relaxed);
    };

    // ---- Hot loop -------------------------------------------------------
    while (!m_bStop.load(std::memory_order_relaxed))
    {
        pConsumer->PollBatch(4096, 10, onMessage);
    }

    pConsumer->Stop();

    // Wait for the background replay thread to finish before signalling done.
    if (replayThread.joinable())
        replayThread.join();

    // Signal that Run() is fully done (consumer destroyed, thread about to
    // exit).  The render thread polls IsDone() to know when Join() is safe
    // to call without blocking.
    m_bDone.store(true, std::memory_order_release);
}
void ConsumerThread::ParseAndQueueCtl(const void* pData, size_t cbData)
{
    if (!pData || cbData == 0 || cbData > 512) return;

    // Minimal strstr-based JSON parse -- avoids any library dependency.
    // Expected format: {"v":1,"cmd":"ctr_state","wid":N,"enabled":BOOL,...}
    const char* p = static_cast<const char*>(pData);

    const char* pWid = strstr(p, "\"wid\":");
    if (!pWid) return;
    pWid += 6; // skip "wid":
    while (*pWid == ' ') ++pWid;
    char* pEnd = nullptr;
    long wid = strtol(pWid, &pEnd, 10);
    if (pEnd == pWid || wid < 0 || wid > 1023) return;

    const char* pEn = strstr(p, "\"enabled\":");
    if (!pEn) return;
    pEn += 10; // skip "enabled":
    while (*pEn == ' ') ++pEn;
    bool bEnabled = (*pEn == 't' || *pEn == 'T' || *pEn == '1');

    CounterStateEvent ev;
    ev.wid     = static_cast<uint16_t>(wid);
    ev.bEnabled = bEnabled;

    std::lock_guard<std::mutex> lk(m_ctlMu);
    m_pendingCtl.push_back(ev);
}

void ConsumerThread::DrainCounterStateEvents(
    const std::function<void(uint16_t wid, bool bEnabled)>& cb)
{
    std::vector<CounterStateEvent> batch;
    {
        std::lock_guard<std::mutex> lk(m_ctlMu);
        batch.swap(m_pendingCtl);
    }
    for (const auto& ev : batch)
        cb(ev.wid, ev.bEnabled);
}