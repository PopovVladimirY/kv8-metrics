////////////////////////////////////////////////////////////////////////////////
// kv8zoom/ZoomBridge.cpp
////////////////////////////////////////////////////////////////////////////////

#include "ZoomBridge.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>

namespace kv8zoom {

using Clock = std::chrono::steady_clock;

static int64_t NowUs() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch())
        .count();
}

// Convert FILETIME (100-ns ticks since 1601-01-01) to ms since Unix epoch.
static int64_t FiletimeToMs(uint32_t hi, uint32_t lo) noexcept
{
    constexpr uint64_t kUnixEpochOffset = 116444736000000000ULL;
    const uint64_t ft = (static_cast<uint64_t>(hi) << 32) | lo;
    if (ft <= kUnixEpochOffset) return 0;
    return static_cast<int64_t>((ft - kUnixEpochOffset) / 10000);
}

ZoomBridge::ZoomBridge(const Config& cfg)
    : m_cfg(cfg)
    , m_kafka(cfg.kafka)
    , m_decim(cfg.bridge.decimation)
    , m_attFilter(cfg.bridge.attitude_slerp_window_ms)
    , m_lastSnapshot(Clock::now())
    , m_lastTimeline(Clock::now())
{
    m_ws.OnOpen([this](WsServer::WsType* ws) { OnClientOpen(ws); })
        .OnMessage([this](WsServer::WsType* ws, std::string_view msg) {
            OnClientMessage(ws, msg);
        })
        .OnClose([this](WsServer::WsType* ws) { OnClientClose(ws); })
        .AddTimer([this]() { OnTimer(); }, 10 /* ms */);
}

ZoomBridge::~ZoomBridge()
{
    Stop();
    if (m_kafkaThread.joinable())    m_kafkaThread.join();
    if (m_discoveryThread.joinable()) m_discoveryThread.join();
}

// -- Session helpers ----------------------------------------------------------

std::vector<SessionInfo> ZoomBridge::BuildSessionInfoList() const
{
    std::vector<SessionInfo> out;
    out.reserve(m_allSessions.size());

    for (auto& [prefix, meta] : m_allSessions) {
        SessionInfo si;
        si.id     = meta.sSessionID;
        si.name   = meta.sName;
        si.prefix = meta.sSessionPrefix;
        si.ts_ms  = 0;
        si.feeds  = 0;
        si.is_live = (m_sessionLiveness.count(prefix) && m_sessionLiveness.at(prefix));

        if (!meta.topicToTimeHi.empty()) {
            auto it   = meta.topicToTimeHi.begin();
            auto itLo = meta.topicToTimeLo.find(it->first);
            if (itLo != meta.topicToTimeLo.end())
                si.ts_ms = FiletimeToMs(it->second, itLo->second);
        }

        for (auto& [hash, counters] : meta.hashToCounters)
            for (auto& cm : counters)
                if (cm.bIsUdtFeed) ++si.feeds;

        out.push_back(std::move(si));
    }

    std::sort(out.begin(), out.end(),
              [](const SessionInfo& a, const SessionInfo& b) { return a.id > b.id; });
    return out;
}

void ZoomBridge::StartKafkaThread()
{
    m_kafkaThread = std::thread([this]() { KafkaThreadFunc(); });
}

void ZoomBridge::StartDiscoveryThread()
{
    m_discoveryThread = std::thread([this]() { DiscoveryThreadFunc(); });
}

// Runs in m_discoveryThread: wakes every ~20 s, refreshes the session list
// using a fresh temporary consumer (QuickDiscover is thread-safe).
void ZoomBridge::DiscoveryThreadFunc()
{
    for (;;) {
        // Sleep in 100 ms slices so Stop() wakes us quickly.
        for (int i = 0; i < 200; ++i) {
            if (m_stop.load(std::memory_order_relaxed)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (m_stop.load(std::memory_order_relaxed)) return;

        auto sessions = m_kafka.QuickDiscover();
        if (!sessions.empty()) {
            auto liveness = m_kafka.QuickCheckLiveness(sessions);
            std::lock_guard<std::mutex> lock(m_sessionsMtx);
            m_freshSessions = std::move(sessions);
            m_freshLiveness = std::move(liveness);
            m_sessionsDirty = true;
        }
    }
}

// -- Run / Stop ---------------------------------------------------------------

void ZoomBridge::Run()
{
    fprintf(stderr, "[ZoomBridge] Discovering sessions on channel '%s'...\n",
            m_cfg.kafka.channel.c_str());

    m_allSessions = m_kafka.DiscoverAllSessions();
    // Populate initial liveness using heartbeat records.
    if (!m_allSessions.empty())
        m_sessionLiveness = m_kafka.QuickCheckLiveness(m_allSessions);

    if (m_allSessions.empty()) {
        fprintf(stderr, "[ZoomBridge] No sessions found. Waiting for clients to connect.\n");
    } else {
        fprintf(stderr, "[ZoomBridge] %zu session(s) available:\n", m_allSessions.size());
        for (auto& [prefix, meta] : m_allSessions)
            fprintf(stderr, "  [%s] %s\n",
                    meta.sSessionID.c_str(),
                    meta.sName.empty() ? "(no name)" : meta.sName.c_str());
        fprintf(stderr, "\n");
    }

    StartDiscoveryThread();

    // Start the WebSocket event loop (blocks until Stop() is called).
    // The Kafka thread is NOT started until the user selects a session.
    m_ws.Run(m_cfg.bridge.ws_port);
}

void ZoomBridge::Stop()
{
    if (m_stop.exchange(true)) return;
    m_kafka.RequestStop();
    m_ws.Stop();
    // m_discoveryThread wakes within <=100 ms; joined in destructor.
}

void ZoomBridge::KafkaThreadFunc()
{
    while (!m_kafka.StopRequested())
        m_kafka.PollOnce(100);
}

// -- Session switch -----------------------------------------------------------

void ZoomBridge::SwitchToSession(const std::string& sessionId)
{
    auto it = m_allSessions.find(sessionId);
    if (it == m_allSessions.end()) {
        for (auto& [prefix, meta] : m_allSessions) {
            if (meta.sSessionID == sessionId) {
                it = m_allSessions.find(prefix);
                break;
            }
        }
    }
    if (it == m_allSessions.end()) {
        m_ws.BroadcastText(Serializer::Error("Unknown session: " + sessionId));
        return;
    }

    const kv8::SessionMeta& meta = it->second;
    fprintf(stderr, "[ZoomBridge] Switching to session '%s' (%s)...\n",
            meta.sSessionID.c_str(),
            meta.sName.empty() ? "no name" : meta.sName.c_str());

    m_kafka.RequestStop();
    if (m_kafkaThread.joinable()) m_kafkaThread.join();

    m_kafka.Reset();
    m_kafka.ClearStop();
    m_assembler    = FrameAssembler{};
    m_trail.Clear();
    m_attFilter    = AttitudeFilter{m_cfg.bridge.attitude_slerp_window_ms};
    m_decim        = DecimationController{m_cfg.bridge.decimation};
    m_lastSnapshot = Clock::now();
    m_tsFirst = 0; m_tsLatest = 0; m_tsEmit = 0;
    m_tsSessionStart = 0;
    m_pendingSeekTs.reset();
    m_offlineLoadDone    = false;
    m_seekTargetPosition = 0;
    m_lastFrameTime      = Clock::now();

    m_feedCount       = 0;
    m_activeSessionId = meta.sSessionID;
    m_activeMeta      = meta;

    // Derive session start time from FILETIME metadata (topic creation time).
    if (!meta.topicToTimeHi.empty()) {
        auto hiIt   = meta.topicToTimeHi.begin();
        auto loIt   = meta.topicToTimeLo.find(hiIt->first);
        if (loIt != meta.topicToTimeLo.end()) {
            const int64_t tsMs = FiletimeToMs(hiIt->second, loIt->second);
            if (tsMs > 0) m_tsSessionStart = tsMs * 1000LL;
        }
    }

    // Determine seek point depending on session liveness.
    const bool isLive = (m_sessionLiveness.count(it->first)
                      && m_sessionLiveness.at(it->first));
    int64_t seekMs = 0; // 0 = start from beginning (offline replay)

    if (isLive && !meta.dataTopics.empty()) {
        const std::string& anyTopic = *meta.dataTopics.begin();
        const int64_t latestMs = m_kafka.GetTopicLatestTimestampMs(anyTopic, 3000);
        if (latestMs > 0) {
            // Start 30 s before the live edge so recent path context is visible.
            constexpr int64_t kMarginMs = 30LL * 1000LL;
            seekMs = std::max(int64_t(0), latestMs - kMarginMs);
            fprintf(stderr, "[ZoomBridge] Live session: direct seek to ts_ms=%lld\n",
                    (long long)seekMs);
        }
    }

    // Subscribe (or direct-assign at timestamp for live/seek).
    if (seekMs > 0)
        m_feedCount = m_kafka.SubscribeToSessionFromTimestampMs(meta, seekMs);
    else
        m_feedCount = m_kafka.SubscribeToSession(meta);

    m_isLive = isLive;
    m_paused = false;

    m_ws.BroadcastText(Serializer::SessionSelected(meta.sSessionID, meta.sName));
    m_ws.BroadcastText(Serializer::SessionList(BuildSessionInfoList(), m_activeSessionId));

    StartKafkaThread();

    fprintf(stderr, "[ZoomBridge] Session '%s' active (%d feed(s)).\n",
            meta.sSessionID.c_str(), m_feedCount);
}

// -- Seek ---------------------------------------------------------------------

void ZoomBridge::ProcessSeek(int64_t seekTs)
{
    if (m_activeSessionId.empty()) return;

    fprintf(stderr, "[ZoomBridge] Seek to ts_us=%lld (paused=%d)\n",
            (long long)seekTs, (int)m_paused);

    m_kafka.RequestStop();
    if (m_kafkaThread.joinable()) m_kafkaThread.join();

    m_kafka.Reset();
    m_kafka.ClearStop();
    m_assembler    = FrameAssembler{};
    // For offline sessions keep the existing trail so the full trajectory
    // stays visible; for live sessions clear to avoid stale geometry.
    if (m_isLive) m_trail.Clear();
    m_attFilter    = AttitudeFilter{m_cfg.bridge.attitude_slerp_window_ms};
    m_decim        = DecimationController{m_cfg.bridge.decimation};
    m_lastSnapshot = Clock::now();
    // Preserve m_tsFirst so the timeline start stays anchored.
    m_tsLatest = 0; m_tsEmit = 0;
    // m_tsSessionStart is intentionally preserved across seeks.

    // Convert seek timestamp (us) to ms for AssignAtTimestampMs.
    // seekTs == 0 means "from the very beginning" (OFFSET_BEGINNING).
    if (seekTs > 0)
        m_feedCount = m_kafka.SubscribeToSessionFromTimestampMs(
                          m_activeMeta, seekTs / 1000LL, 3000);
    else
        m_feedCount = m_kafka.SubscribeToSession(m_activeMeta);

    StartKafkaThread();
}

// -- Timer (uWS event-loop thread) --------------------------------------------

void ZoomBridge::OnTimer()
{
    // 1. Process pending session switch.
    if (!m_pendingSessionId.empty()) {
        const std::string id = std::move(m_pendingSessionId);
        m_pendingSessionId.clear();
        SwitchToSession(id);
        return;
    }

    // 2. Process pending seek.
    if (m_pendingSeekTs.has_value()) {
        const int64_t ts = m_pendingSeekTs.value();
        m_pendingSeekTs.reset();
        // Preserve m_paused -- do NOT auto-resume.
        m_seekTargetPosition = ts;   // assembler will stop updating at this ts
        m_offlineLoadDone    = false;
        ProcessSeek(ts);
        return;
    }

    // 3. Handle pending pause toggle.
    if (m_pendingPause.has_value()) {
        m_paused = m_pendingPause.value();
        m_pendingPause.reset();
        fprintf(stderr, "[ZoomBridge] Playback %s\n", m_paused ? "paused" : "resumed");
    }

    // 4. Merge discovery refresh.
    {
        bool dirty = false;
        std::map<std::string, kv8::SessionMeta> fresh;
        std::map<std::string, bool>             freshLiveness;
        {
            std::lock_guard<std::mutex> lock(m_sessionsMtx);
            if (m_sessionsDirty) {
                dirty = true;
                fresh         = std::move(m_freshSessions);
                freshLiveness = std::move(m_freshLiveness);
                m_sessionsDirty = false;
            }
        }
        if (dirty && !fresh.empty()) {
            bool changed = (fresh.size() != m_allSessions.size());
            if (!changed) {
                for (auto& [k, v] : fresh)
                    if (!m_allSessions.count(k)) { changed = true; break; }
            }
            // Always refresh liveness even if session set unchanged.
            m_sessionLiveness = std::move(freshLiveness);
            if (changed) {
                m_allSessions = std::move(fresh);
                m_ws.BroadcastText(
                    Serializer::SessionList(BuildSessionInfoList(), m_activeSessionId));
            } else {
                // Same sessions but liveness may have changed -- re-broadcast.
                m_ws.BroadcastText(
                    Serializer::SessionList(BuildSessionInfoList(), m_activeSessionId));
            }
        }
    }

    if (m_ws.ConnectionCount() == 0 || m_activeSessionId.empty()) return;

    const int64_t now_us = NowUs();
    DrainNav(now_us);
    DrainAtt(now_us);
    DrainMot(now_us);
    DrainWx(now_us);

    // 5. Auto-pause for offline sessions (or paused seeks) once all data has
    //    been consumed.  Triggers after 500 ms of ring-buffer silence.
    if (!m_offlineLoadDone && m_tsLatest > 0
        && (!m_isLive || m_paused)
        && Clock::now() - m_lastFrameTime > std::chrono::milliseconds(500))
    {
        m_offlineLoadDone = true;

        // Determine the timeline position.
        if (m_seekTargetPosition > 0)
            m_tsEmit = m_seekTargetPosition;
        else
            m_tsEmit = m_tsLatest;
        m_seekTargetPosition = 0;

        if (!m_paused) m_paused = true;

        // Broadcast a final snapshot + trail + timeline so the view updates.
        if (m_assembler.HasMinimumData())
            m_ws.BroadcastText(Serializer::Snapshot(m_assembler.Latest()));
        if (m_trail.Size() > 0) {
            auto pts = m_trail.Simplify(2.0);
            if (!pts.empty())
                m_ws.BroadcastText(Serializer::Trail(pts));
        }
        {
            const int64_t tsStart   = (m_tsSessionStart > 0) ? m_tsSessionStart : m_tsFirst;
            const int64_t tsEnd     = m_tsLatest;
            const Serializer::TimelineInfo ti{
                tsStart, tsEnd, m_tsEmit, m_isLive, m_paused
            };
            m_ws.BroadcastText(Serializer::Timeline(ti));
            m_lastTimeline = Clock::now();
        }
        m_lastSnapshot = Clock::now();
        fprintf(stderr, "[ZoomBridge] Offline load done -- auto-paused at %lld\n",
                (long long)m_tsEmit);
    }

    // 6. Periodic snapshot + trail update (keeps live trail drawn).
    const auto snapshotInterval = std::chrono::seconds(m_cfg.bridge.snapshot_interval_s);
    if (Clock::now() - m_lastSnapshot >= snapshotInterval) {
        if (m_assembler.HasMinimumData())
            m_ws.BroadcastText(Serializer::Snapshot(m_assembler.Latest()));
        if (m_trail.Size() > 0) {
            auto pts = m_trail.Simplify(2.0);
            if (!pts.empty())
                m_ws.BroadcastText(Serializer::Trail(pts));
        }
        m_lastSnapshot = Clock::now();
    }

    // 7. Periodic timeline broadcast (every 1 second).
    // ts_start anchors to the known session start (from FILETIME metadata), or
    // falls back to the first received frame if metadata is unavailable.
    if (m_tsLatest > 0 && Clock::now() - m_lastTimeline >= std::chrono::seconds(1)) {
        const int64_t tsStart   = (m_tsSessionStart > 0) ? m_tsSessionStart : m_tsFirst;
        const int64_t tsEnd     = m_tsLatest;
        const int64_t tsCurrent = (m_tsEmit > 0) ? m_tsEmit : m_tsLatest;
        const Serializer::TimelineInfo ti{
            tsStart, tsEnd, tsCurrent, m_isLive, m_paused
        };
        m_ws.BroadcastText(Serializer::Timeline(ti));
        m_lastTimeline = Clock::now();
    }
}

// -- Drain ring buffers -------------------------------------------------------

void ZoomBridge::DrainNav(int64_t now_us)
{
    NavFrame f;
    while (m_kafka.NavRing().pop(f)) {
        m_lastFrameTime = Clock::now();
        if (f.ts_us > 0) {
            if (m_tsFirst == 0) m_tsFirst = f.ts_us;
            if (f.ts_us > m_tsLatest) m_tsLatest = f.ts_us;
        }

        m_trail.Append({f.ts_us, f.data.lat_deg, f.data.lon_deg, f.data.alt_m});

        // When paused with a seek target, only update assembler up to that
        // timestamp so the snapshot reflects the state at the target position.
        const bool updateAssembler = !m_paused
            || m_seekTargetPosition <= 0
            || f.ts_us <= m_seekTargetPosition;
        if (updateAssembler)
            m_assembler.UpdateNav(f);

        if (!m_paused && m_decim.ShouldEmitNav(f, now_us)) {
            m_tsEmit = f.ts_us;
            m_ws.BroadcastText(Serializer::Nav(m_assembler.Latest()));
        }
    }
}

void ZoomBridge::DrainAtt(int64_t now_us)
{
    AttFrame f;
    while (m_kafka.AttRing().pop(f)) {
        m_lastFrameTime = Clock::now();

        const bool updateAssembler = !m_paused
            || m_seekTargetPosition <= 0
            || f.ts_us <= m_seekTargetPosition;
        if (updateAssembler) {
            m_attFilter.AddSample(f);
            double qw, qx, qy, qz;
            m_attFilter.GetFiltered(f.ts_us, qw, qx, qy, qz);
            m_assembler.UpdateAtt(f.ts_us, qw, qx, qy, qz, f);
        }

        if (!m_paused && m_decim.ShouldEmitAtt(f, now_us)) {
            if (f.ts_us > m_tsEmit) m_tsEmit = f.ts_us;
            m_ws.BroadcastText(Serializer::Att(m_assembler.Latest()));
        }
    }
}

void ZoomBridge::DrainMot(int64_t now_us)
{
    MotFrame f;
    while (m_kafka.MotRing().pop(f)) {
        m_assembler.UpdateMot(f);

        if (!m_paused && m_decim.ShouldEmitMot(f, now_us))
            m_ws.BroadcastText(Serializer::Mot(m_assembler.Latest()));
    }
}

void ZoomBridge::DrainWx(int64_t now_us)
{
    WxFrame f;
    while (m_kafka.WxRing().pop(f)) {
        m_assembler.UpdateWx(f);

        if (!m_paused && m_decim.ShouldEmitWx(f, now_us))
            m_ws.BroadcastText(Serializer::Wx(m_assembler.Latest()));
    }
}

// -- WebSocket callbacks ------------------------------------------------------

void ZoomBridge::OnClientOpen(WsServer::WsType* /*ws*/)
{
    m_ws.BroadcastText(
        Serializer::SessionList(BuildSessionInfoList(), m_activeSessionId));

    if (!m_activeSessionId.empty()) {
        m_ws.BroadcastText(
            Serializer::Meta(m_cfg.kafka.channel, m_feedCount, m_decim.GetZoom()));

        if (m_assembler.HasMinimumData()) {
            m_ws.BroadcastText(Serializer::Snapshot(m_assembler.Latest()));
            auto pts = m_trail.Simplify(2.0);
            if (!pts.empty())
                m_ws.BroadcastText(Serializer::Trail(pts));
        }

        if (m_tsLatest > 0) {
            const int64_t tsStart   = (m_tsSessionStart > 0) ? m_tsSessionStart : m_tsFirst;
            const int64_t tsEnd     = m_tsLatest;
            const int64_t tsCurrent = (m_tsEmit > 0) ? m_tsEmit : m_tsLatest;
            const Serializer::TimelineInfo ti{
                tsStart, tsEnd, tsCurrent, m_isLive, m_paused
            };
            m_ws.BroadcastText(Serializer::Timeline(ti));
        }
    }
}

void ZoomBridge::OnClientMessage(WsServer::WsType* /*ws*/, std::string_view msg)
{
    try {
        auto j = nlohmann::json::parse(msg);
        const std::string type = j.value("type", "");

        if (type == "camera") {
            int z = j.value("zoom", m_decim.GetZoom());
            m_decim.SetZoom(z);
        } else if (type == "select_session") {
            const std::string id = j.value("id", "");
            if (!id.empty())
                m_pendingSessionId = id;
        } else if (type == "seek") {
            const int64_t ts = j.value("ts_us", int64_t{0});
            m_pendingSeekTs = ts;
        } else if (type == "pause") {
            m_pendingPause = true;
        } else if (type == "play") {
            m_pendingPause = false;
        }
    } catch (...) {
        // Malformed message: silently ignore.
    }
}

void ZoomBridge::OnClientClose(WsServer::WsType* /*ws*/)
{
    // Nothing to clean up per-client.
}

} // namespace kv8zoom
