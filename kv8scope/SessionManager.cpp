// kv8scope -- Kv8 Software Oscilloscope
// SessionManager.cpp -- Background Kafka discovery thread implementation.

#include "SessionManager.h"
#include "ConfigStore.h"

#include <kv8/IKv8Consumer.h>
#include <kv8/Kv8Types.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Build a Kv8Config from the application's ScopeConfig.
static kv8::Kv8Config MakeKv8Config(const ScopeConfig& sc)
{
    kv8::Kv8Config cfg;
    cfg.sBrokers       = sc.sBrokers;
    cfg.sSecurityProto = sc.sSecurityProtocol;
    cfg.sSaslMechanism = sc.sSaslMechanism;
    cfg.sUser          = sc.sUsername;
    cfg.sPass          = sc.sPassword;
    // Use a unique group ID so this consumer does not interfere with
    // other tools.  An empty string lets libkv8 auto-generate one.
    cfg.sGroupID.clear();
    return cfg;
}

/// Probe the liveness of a single session using the following priority:
///   1. If a <sessionPrefix>.hb topic exists: read the latest HbRecord and
///      classify based on its tsUnixMs vs the T_live / T_dead thresholds.
///   2. Otherwise: find the most recent Kafka timestamp across all data topics
///      and classify based on that age.
///
/// The returned SessionLivenessInfo encodes both the final state and the
/// raw timestamps so the UI can render hover tooltips.
static SessionLivenessInfo ProbeSessionLiveness(
    kv8::IKv8Consumer& consumer,
    const kv8::SessionMeta& meta,
    const ScopeConfig::LivenessConfig& lv)
{
    SessionLivenessInfo info;

    // Snapshot the current wall-clock time BEFORE any Kafka calls.
    // ReadLatestRecord and GetTopicLatestTimestampMs can each block for up to
    // their timeout (1500 ms).  Taking nowMs after those calls would inflate
    // the apparent age of the heartbeat / data timestamps by the probe latency,
    // which could incorrectly push a session from Live into GoingOffline.
    using namespace std::chrono;
    const int64_t nowMs = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();

    // -- Layer 1: Heartbeat (.hb topic) ----------------------------------------
    const std::string sHbTopic = meta.sSessionPrefix + ".hb";

    bool bReadHb = consumer.ReadLatestRecord(sHbTopic, 1500,
        [&](const void *pPayload, size_t cbPayload, int64_t /*tsKafkaMs*/)
        {
            if (cbPayload >= sizeof(kv8::HbRecord))
            {
                kv8::HbRecord hb;
                memcpy(&hb, pPayload, sizeof(hb));
                if (hb.bVersion == kv8::KV8_HB_VERSION)
                {
                    info.bHasHeartbeat  = true;
                    info.tsHeartbeatMs  = hb.tsUnixMs;
                    if (hb.bState == kv8::KV8_HB_STATE_SHUTDOWN)
                        info.bRegistryClosed = true;
                }
            }
        });

    if (bReadHb && info.bHasHeartbeat)
    {
        const int64_t ageMs = nowMs - info.tsHeartbeatMs;

        if (info.bRegistryClosed)
        {
            info.eState = SessionLiveness::Historical;
        }
        else if (ageMs < static_cast<int64_t>(lv.iHeartbeatLiveS) * 1000)
        {
            info.eState = SessionLiveness::Live;
        }
        else if (ageMs < static_cast<int64_t>(lv.iHeartbeatDeadS) * 1000)
        {
            info.eState = SessionLiveness::GoingOffline;
        }
        else
        {
            info.eState = SessionLiveness::Offline;
        }
        return info;
    }

    // -- Layer 2: Data recency (fallback when no .hb topic) --------------------
    if (!meta.dataTopics.empty())
    {
        int64_t newestMs = -1;
        for (const auto& sTopic : meta.dataTopics)
        {
            int64_t tsMs = consumer.GetTopicLatestTimestampMs(sTopic, 1500);
            if (tsMs > newestMs)
                newestMs = tsMs;
        }
        info.tsLastSampleMs = newestMs;

        if (newestMs > 0)
        {
            const int64_t ageMs = nowMs - newestMs;

            if (ageMs < static_cast<int64_t>(lv.iDataLiveS) * 1000)
                info.eState = SessionLiveness::Live;
            else if (ageMs < static_cast<int64_t>(lv.iDataDeadS) * 1000)
                info.eState = SessionLiveness::GoingOffline;
            else
                info.eState = SessionLiveness::Offline;
        }
        else
        {
            // Topics exist but are empty.
            info.eState = SessionLiveness::Offline;
        }
    }
    else
    {
        info.eState = SessionLiveness::Offline;
    }

    return info;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SessionManager::SessionManager(const ConfigStore& config)
    : m_config(config)
{
}

SessionManager::~SessionManager()
{
    Stop();
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

void SessionManager::Start()
{
    if (m_bRunning.load(std::memory_order_relaxed))
        return;

    m_bStopRequested.store(false, std::memory_order_relaxed);
    m_bRunning.store(true, std::memory_order_relaxed);
    m_thread = std::thread(&SessionManager::ThreadFunc, this);
}

void SessionManager::Stop()
{
    if (!m_bRunning.load(std::memory_order_relaxed))
        return;

    m_bStopRequested.store(true, std::memory_order_release);
    if (m_thread.joinable())
        m_thread.join();

    m_bRunning.store(false, std::memory_order_relaxed);
}

void SessionManager::Restart()
{
    Stop();

    // Clear all discovered state so the next poll cycle starts fresh.
    m_knownSessions.clear();
    m_prevLiveness.clear();
    m_bEverConnected = false;

    {
        std::lock_guard<std::mutex> lk(m_evMutex);
        m_events.clear();
    }

    Start();
}

// ---------------------------------------------------------------------------
// DrainEvents
// ---------------------------------------------------------------------------

void SessionManager::DrainEvents(std::vector<SessionEvent>& out)
{
    std::lock_guard<std::mutex> lk(m_evMutex);
    out.insert(out.end(),
               std::make_move_iterator(m_events.begin()),
               std::make_move_iterator(m_events.end()));
    m_events.clear();
}

// ---------------------------------------------------------------------------
// Push helper (called from the worker thread)
// ---------------------------------------------------------------------------

static void PushEvent(std::mutex& mtx,
                      std::vector<SessionEvent>& q,
                      SessionEvent ev)
{
    std::lock_guard<std::mutex> lk(mtx);
    q.push_back(std::move(ev));
}

// ---------------------------------------------------------------------------
// Thread function
// ---------------------------------------------------------------------------

void SessionManager::ThreadFunc()
{
    const int iPollMs = m_config.Get().iSessionPollMs;

    while (!m_bStopRequested.load(std::memory_order_acquire))
    {
        bool bOk = RunOnePollCycle();
        if (!bOk)
        {
            // Consumer is in a bad state.  Drop it so the next cycle
            // creates a fresh connection.
            m_pConsumer.reset();
        }

        // Sleep in small increments so Stop() is responsive.
        const auto tWake = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(iPollMs);
        while (std::chrono::steady_clock::now() < tWake)
        {
            if (m_bStopRequested.load(std::memory_order_acquire))
            {
                m_pConsumer.reset();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    m_pConsumer.reset();
}

// ---------------------------------------------------------------------------
// RunOnePollCycle -- one full discovery sweep
// ---------------------------------------------------------------------------

bool SessionManager::RunOnePollCycle()
{
    const ScopeConfig& sc = m_config.Get();

    // 1. Reuse consumer across cycles to avoid per-cycle reconnection overhead.
    //    Create (or recreate after failure) only when not already connected.
    if (!m_pConsumer)
    {
        kv8::Kv8Config cfg = MakeKv8Config(sc);
        m_pConsumer = kv8::IKv8Consumer::Create(cfg);
        if (!m_pConsumer)
        {
            SessionEvent ev;
            ev.eType    = SessionEvent::ConnectionError;
            ev.sMessage = "Failed to create Kafka consumer (bad config?)";
            PushEvent(m_evMutex, m_events, std::move(ev));
            return false;
        }
    }

    // 2. Discover channels.
    std::vector<std::string> channels;
    try
    {
        channels = m_pConsumer->ListChannels(2000);
    }
    catch (const std::exception& ex)
    {
        SessionEvent ev;
        ev.eType    = SessionEvent::ConnectionError;
        ev.sMessage = std::string("ListChannels failed: ") + ex.what();
        PushEvent(m_evMutex, m_events, std::move(ev));
        return false;
    }
    catch (...)
    {
        SessionEvent ev;
        ev.eType    = SessionEvent::ConnectionError;
        ev.sMessage = "ListChannels failed (unknown error)";
        PushEvent(m_evMutex, m_events, std::move(ev));
        return false;
    }

    // If we get here, the broker is reachable.
    if (!m_bEverConnected)
    {
        m_bEverConnected = true;
        SessionEvent ev;
        ev.eType = SessionEvent::Connected;
        PushEvent(m_evMutex, m_events, std::move(ev));
    }

    // 3. Discover sessions for each channel.
    std::map<std::string, kv8::SessionMeta> currentSessions;

    for (const auto& sChannel : channels)
    {
        std::map<std::string, kv8::SessionMeta> found;
        try
        {
            found = m_pConsumer->DiscoverSessions(sChannel);
        }
        catch (...)
        {
            // Skip this channel on error; try again next cycle.
            continue;
        }

        for (auto& kv : found)
        {
            // Tag the channel on the event so the UI can group sessions.
            currentSessions[kv.first] = std::move(kv.second);
        }
    }

    // 4. Diff against previous state: detect Appeared / Disappeared.
    // -- Appeared --
    for (auto& kv : currentSessions)
    {
        const std::string& prefix = kv.first;
        if (m_knownSessions.find(prefix) == m_knownSessions.end())
        {
            SessionEvent ev;
            ev.eType          = SessionEvent::Appeared;
            ev.sSessionPrefix = prefix;
            ev.meta           = kv.second;

            // Derive channel from prefix (everything before the session ID segment).
            // Session prefix format: "<channel>.<sessionID>"
            // Channel = prefix up to the last-but-one dot before the session ID.
            // Use the meta's sSessionPrefix which is already filled.
            // The channel is the portion before ".<sessionID>".
            // SessionMeta::sSessionID holds just the ID segment.
            if (!kv.second.sSessionID.empty())
            {
                size_t pos = prefix.rfind(kv.second.sSessionID);
                if (pos != std::string::npos && pos > 0)
                    ev.sChannel = prefix.substr(0, pos - 1); // strip trailing dot
            }
            if (ev.sChannel.empty())
                ev.sChannel = prefix;

            PushEvent(m_evMutex, m_events, std::move(ev));
        }
    }

    // -- Disappeared --
    for (const auto& kv : m_knownSessions)
    {
        const std::string& prefix = kv.first;
        if (currentSessions.find(prefix) == currentSessions.end())
        {
            SessionEvent ev;
            ev.eType          = SessionEvent::Disappeared;
            ev.sSessionPrefix = prefix;
            PushEvent(m_evMutex, m_events, std::move(ev));
        }
    }

    // -- MetaUpdated: fire when an existing session gains new virtual fields --
    // Debounced (Fix 1 -- KV8_UDT_STALL): when nVirt increases, reset a
    // stabilization deadline instead of firing immediately.  Fire only once
    // the deadline expires with no further nVirt growth, collapsing multiple
    // UDT schema arrivals into a single MetaUpdated event.
    {
        using Clock = std::chrono::steady_clock;
        const auto stabilizeMs =
            std::chrono::milliseconds(sc.iMetaStabilizeMs);

        for (auto& kv : currentSessions)
        {
            const std::string& prefix = kv.first;
            if (m_knownSessions.find(prefix) == m_knownSessions.end())
                continue;  // newly appeared -- handled above

            // Count virtual UDT fields in the freshly scanned meta.
            size_t nVirt = 0;
            for (const auto& bucket : kv.second.hashToCounters)
                for (const auto& cm : bucket.second)
                    if (cm.bIsUdtVirtualField)
                        ++nVirt;

            auto it = m_prevVirtualFieldCount.find(prefix);
            const size_t nPrev = (it != m_prevVirtualFieldCount.end())
                                     ? it->second : 0u;

            if (nVirt > nPrev)
            {
                m_prevVirtualFieldCount[prefix] = nVirt;
                // Schema still arriving -- reset cooldown deadline.
                m_metaStabilizeDeadline[prefix] =
                    Clock::now() + stabilizeMs;
                m_metaStabilizeMeta[prefix] = kv.second;
            }
            else
            {
                // Keep tracking even if unchanged.
                if (it == m_prevVirtualFieldCount.end())
                    m_prevVirtualFieldCount[prefix] = nVirt;

                // Check if a pending stabilization deadline has expired.
                auto dl = m_metaStabilizeDeadline.find(prefix);
                if (dl != m_metaStabilizeDeadline.end()
                    && Clock::now() >= dl->second)
                {
                    SessionEvent ev;
                    ev.eType          = SessionEvent::MetaUpdated;
                    ev.sSessionPrefix = prefix;
                    ev.meta           = m_metaStabilizeMeta[prefix];
                    PushEvent(m_evMutex, m_events, std::move(ev));
                    m_metaStabilizeDeadline.erase(dl);
                    m_metaStabilizeMeta.erase(prefix);
                }
            }
        }
    }

    // Remove tracking for disappeared sessions.
    for (auto it = m_prevVirtualFieldCount.begin();
         it != m_prevVirtualFieldCount.end(); )
    {
        if (currentSessions.find(it->first) == currentSessions.end())
            it = m_prevVirtualFieldCount.erase(it);
        else
            ++it;
    }
    for (auto it = m_metaStabilizeDeadline.begin();
         it != m_metaStabilizeDeadline.end(); )
    {
        if (currentSessions.find(it->first) == currentSessions.end())
        {
            m_metaStabilizeMeta.erase(it->first);
            it = m_metaStabilizeDeadline.erase(it);
        }
        else
            ++it;
    }

    // 5. Liveness detection via heartbeat + data recency.
    const auto& lv = sc.liveness;
    for (auto& kv : currentSessions)
    {
        const std::string& prefix = kv.first;

        SessionLivenessInfo info = ProbeSessionLiveness(*m_pConsumer, kv.second, lv);

        // Emit StatusChanged whenever the state transitions, including the
        // first probe for a newly appeared session (Unknown -> Live/etc.).
        // This ensures the SessionListPanel entry and any open ScopeWindow
        // receive the correct initial liveness immediately -- not only after
        // a second state change occurs.
        {
            auto itPrev = m_prevLiveness.find(prefix);
            SessionLiveness ePrev = (itPrev != m_prevLiveness.end())
                ? itPrev->second
                : SessionLiveness::Unknown;

            if (info.eState != ePrev)
            {
                SessionEvent ev;
                ev.eType          = SessionEvent::StatusChanged;
                ev.sSessionPrefix = prefix;
                ev.liveness       = info;
                PushEvent(m_evMutex, m_events, std::move(ev));
            }
        }

        m_prevLiveness[prefix] = info.eState;
    }

    // Remove disappeared sessions from liveness tracking.
    for (auto it = m_prevLiveness.begin(); it != m_prevLiveness.end(); )
    {
        if (currentSessions.find(it->first) == currentSessions.end())
            it = m_prevLiveness.erase(it);
        else
            ++it;
    }

    // 6. Update known sessions for the next cycle.
    m_knownSessions = std::move(currentSessions);
    return true;
}

// ---------------------------------------------------------------------------
// DeleteSession / DeleteChannel -- main-thread helpers
// ---------------------------------------------------------------------------

bool SessionManager::DeleteSession(const std::string& sChannel,
                                   const kv8::SessionMeta& meta)
{
    kv8::Kv8Config cfg = MakeKv8Config(m_config.Get());
    auto pConsumer = kv8::IKv8Consumer::Create(cfg);
    if (!pConsumer)
        return false;

    try
    {
        pConsumer->MarkSessionDeleted(sChannel, meta);
        pConsumer->DeleteSessionTopics(meta);
    }
    catch (...)
    {
        return false;
    }
    return true;
}

bool SessionManager::DeleteChannel(const std::string& sChannel)
{
    kv8::Kv8Config cfg = MakeKv8Config(m_config.Get());
    auto pConsumer = kv8::IKv8Consumer::Create(cfg);
    if (!pConsumer)
        return false;

    try
    {
        pConsumer->DeleteChannel(sChannel);
    }
    catch (...)
    {
        return false;
    }
    return true;
}
