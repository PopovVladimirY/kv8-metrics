////////////////////////////////////////////////////////////////////////////////
// kv8zoom/ZoomBridge.h
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "AttitudeFilter.h"
#include "Config.h"
#include "DecimationController.h"
#include "FrameAssembler.h"
#include "KafkaReader.h"
#include "PathSimplifier.h"
#include "Serializer.h"
#include "WsServer.h"

#include <kv8/Kv8Types.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kv8zoom {

/// Top-level orchestrator that wires together the Kafka consumer, decimation,
/// attitude filtering, path simplification, and the WebSocket server.
///
/// Threading model:
///   m_kafkaThread: runs KafkaReader::PollOnce() in a tight loop, pushing
///                  decoded frames into SPSC ring buffers.
///
///   m_discoveryThread: wakes every 20 s, calls KafkaReader::QuickDiscover()
///                      using a fresh temporary consumer (no shared state with
///                      m_kafkaThread), and stores new sessions in m_freshSessions
///                      (mutex-protected). The uWS timer merges and broadcasts.
///
///   uWS event-loop thread (the thread calling Run()): a periodic timer
///   drains the ring buffers, updates the assembler, filters and serialises
///   frames, and broadcasts them to connected clients.
///
///   Session switches and seeks are initiated by client messages and processed
///   safely on the uWS timer tick (single-threaded).
class ZoomBridge
{
public:
    explicit ZoomBridge(const Config& cfg);
    ~ZoomBridge();

    /// Block the calling thread until Stop() is called.
    void Run();

    /// Signal a graceful shutdown. Thread-safe.
    void Stop();

private:
    void KafkaThreadFunc();
    void OnTimer();
    void OnClientOpen(WsServer::WsType* ws);
    void OnClientMessage(WsServer::WsType* ws, std::string_view msg);
    void OnClientClose(WsServer::WsType* ws);

    // Drain ring buffers and forward filtered frames.
    void DrainNav(int64_t now_us);
    void DrainAtt(int64_t now_us);
    void DrainMot(int64_t now_us);
    void DrainWx(int64_t now_us);

    // Switch the active session (stops/resets/resubscribes/restarts kafka thread).
    void SwitchToSession(const std::string& sessionId);

    // Seek within the current session: stops kafka, resets, resubscribes from
    // OFFSET_BEGINNING, fast-forwards silently until ts_us >= seekTs.
    // seekTs == 0: stream from the very beginning (no fast-forward).
    void ProcessSeek(int64_t seekTs);

    // Start (or restart) the kafka consumer thread.
    void StartKafkaThread();

    // Start the background session-discovery refresh thread.
    void StartDiscoveryThread();
    void DiscoveryThreadFunc();

    // Build a SessionInfo list from m_allSessions for serialisation.
    std::vector<SessionInfo> BuildSessionInfoList() const;

    Config               m_cfg;
    KafkaReader          m_kafka;
    DecimationController m_decim;
    AttitudeFilter       m_attFilter;
    PathSimplifier       m_trail;
    FrameAssembler       m_assembler;
    WsServer             m_ws;

    std::thread          m_kafkaThread;
    std::thread          m_discoveryThread;
    std::atomic<bool>    m_stop{false};

    // All sessions discovered at startup (and periodically refreshed).
    std::map<std::string, kv8::SessionMeta> m_allSessions;

    // Meta of the currently active session (cached for re-seek without map lookup).
    kv8::SessionMeta     m_activeMeta;

    // ID of the currently active session ("" = none selected).
    std::string          m_activeSessionId;

    // Set by OnClientMessage; consumed and cleared by OnTimer.
    std::string          m_pendingSessionId;

    int                  m_feedCount{0};

    // -- Session discovery refresh (background thread -> uWS timer) -----------
    std::mutex           m_sessionsMtx;
    std::map<std::string, kv8::SessionMeta> m_freshSessions;
    std::map<std::string, bool>             m_freshLiveness;
    bool                 m_sessionsDirty{false};

    // Runtime session liveness (uWS thread only, no mutex needed).
    std::map<std::string, bool> m_sessionLiveness;

    // -- Timeline / replay ----------------------------------------------------
    // Known session start from Kafka topic FILETIME metadata (us since epoch).
    // Set when SwitchToSession resolves metadata; 0 when unknown.
    int64_t              m_tsSessionStart{0};
    // Timestamps (microseconds) of first/latest received frame and last emitted.
    int64_t              m_tsFirst{0};
    int64_t              m_tsLatest{0};
    int64_t              m_tsEmit{0};
    // True when the active session is live (updated on session switch).
    bool                 m_isLive{false};
    // Pending seek set by OnClientMessage, consumed by OnTimer.
    std::optional<int64_t> m_pendingSeekTs;

    // -- Playback control (set by OnClientMessage, consumed by OnTimer) ---------
    bool                 m_paused{false};
    std::optional<bool>  m_pendingPause;

    // -- Offline auto-pause ---------------------------------------------------
    // Timestamp of the last frame popped from any ring buffer (steady clock).
    std::chrono::steady_clock::time_point m_lastFrameTime{};
    // True once an offline session has fully loaded and been auto-paused.
    bool                 m_offlineLoadDone{false};
    // When > 0 the user seeked while paused; assembler updates stop at this ts.
    int64_t              m_seekTargetPosition{0};

    // -- Periodic broadcast cadence -------------------------------------------
    std::chrono::steady_clock::time_point m_lastSnapshot;
    std::chrono::steady_clock::time_point m_lastTimeline;
};

} // namespace kv8zoom
