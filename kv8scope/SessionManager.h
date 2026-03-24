// kv8scope -- Kv8 Software Oscilloscope
// SessionManager.h -- Background Kafka discovery thread.
//
// Runs a std::thread that periodically calls ListChannels() and
// DiscoverSessions() via IKv8Consumer, pushing SessionEvent structs
// into a mutex-guarded queue consumed by the main (render) thread.

#pragma once

#include <kv8/IKv8Consumer.h>
#include <kv8/Kv8Types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// SessionLiveness -- state machine for session liveness detection
// ---------------------------------------------------------------------------

/// Liveness state of a discovered session.
///
/// Transitions:
///   Unknown      -- initial state before any detection has run
///   Live         -- heartbeat or data received within T_live seconds
///   GoingOffline -- no heartbeat/data for T_live..T_dead seconds
///   Offline      -- no heartbeat/data for more than T_dead seconds
///   Historical   -- tombstone written in registry (session was deleted)
enum class SessionLiveness
{
    Unknown,
    Live,
    GoingOffline,
    Offline,
    Historical
};

/// Output of one liveness probe for a session.
struct SessionLivenessInfo
{
    SessionLiveness eState         = SessionLiveness::Unknown;
    int64_t         tsLastSampleMs = 0;  ///< Kafka timestamp of newest data message (0 = none).
    int64_t         tsHeartbeatMs  = 0;  ///< tsUnixMs from newest HbRecord (0 = no hb topic).
    bool            bHasHeartbeat  = false; ///< True if a .hb topic was found.
    bool            bRegistryClosed = false; ///< True if a SHUTDOWN HbRecord was received.
};

// ---------------------------------------------------------------------------
// SessionEvent -- thread -> main-thread message
// ---------------------------------------------------------------------------

struct SessionEvent
{
    enum Type
    {
        Appeared,          // a new session was discovered
        Disappeared,       // a session is no longer present
        StatusChanged,     // liveness state transition
        Connected,         // first successful broker contact
        ConnectionError,   // broker unreachable or other error
        MetaUpdated        // an existing session gained new virtual fields
    };

    Type                    eType       = Connected;
    std::string             sChannel;            // channel prefix
    std::string             sSessionPrefix;      // "<channel>.<sessionID>"
    kv8::SessionMeta        meta;                // valid for Appeared, MetaUpdated
    SessionLivenessInfo     liveness;            // valid for StatusChanged
    std::string             sMessage;            // valid for ConnectionError
};

// ---------------------------------------------------------------------------
// SessionManager
// ---------------------------------------------------------------------------

class ConfigStore;

class SessionManager
{
public:
    explicit SessionManager(const ConfigStore& config);
    ~SessionManager();

    // Non-copyable, non-movable
    SessionManager(const SessionManager&)            = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    SessionManager(SessionManager&&)                 = delete;
    SessionManager& operator=(SessionManager&&)      = delete;

    /// Start the background discovery thread.  Safe to call only once.
    void Start();

    /// Signal the thread to stop and join it.  Idempotent.
    void Stop();

    /// Stop, clear discovered state, and start again.
    /// Used when connection settings change.
    void Restart();

    /// Drain all pending events into @p out (main thread only).
    void DrainEvents(std::vector<SessionEvent>& out);

    /// True after Start() and before Stop().
    bool IsRunning() const { return m_bRunning.load(std::memory_order_relaxed); }

    /// Delete a session from Kafka (marks deleted + removes topics).
    /// Creates a temporary consumer; safe to call from the main thread.
    /// Returns true on success.
    bool DeleteSession(const std::string& sChannel,
                       const kv8::SessionMeta& meta);

    /// Delete an entire channel from Kafka.
    /// Creates a temporary consumer; safe to call from the main thread.
    /// Returns true on success.
    bool DeleteChannel(const std::string& sChannel);

private:
    void ThreadFunc();
    bool RunOnePollCycle();  // returns false when the consumer must be reset

    const ConfigStore&         m_config;

    std::thread                m_thread;
    std::atomic<bool>          m_bStopRequested{false};
    std::atomic<bool>          m_bRunning{false};

    // Kafka consumer persisted across poll cycles to avoid per-cycle
    // connection setup overhead.  Owned exclusively by the worker thread.
    std::unique_ptr<kv8::IKv8Consumer> m_pConsumer;

    // Event queue -- written by worker, drained by main thread.
    std::mutex                 m_evMutex;
    std::vector<SessionEvent>  m_events;

    // Discovery state kept across poll cycles.
    // Key = session prefix, value = SessionMeta.
    std::map<std::string, kv8::SessionMeta>  m_knownSessions;

    // Online detection: per-session liveness state from the previous poll
    // cycle.  Key = session prefix.  Used to emit StatusChanged only on
    // state transitions, not on every cycle.
    std::map<std::string, SessionLiveness>       m_prevLiveness;

    // Track virtual-field count per session across poll cycles.
    // When a session's count increases (UDT schemas registered after first
    // discovery), a MetaUpdated event is fired so open ScopeWindows can
    // reinitialise their consumer and UI with the full schema.
    std::map<std::string, size_t>                m_prevVirtualFieldCount;

    // Stabilization timer for MetaUpdated debouncing (Fix 1 -- KV8_UDT_STALL).
    // When nVirt increases, the deadline is reset.  MetaUpdated fires only
    // once the deadline expires with no further nVirt growth.
    std::map<std::string, std::chrono::steady_clock::time_point> m_metaStabilizeDeadline;
    std::map<std::string, kv8::SessionMeta>                      m_metaStabilizeMeta;

    // Track whether we have ever successfully connected.
    bool m_bEverConnected = false;
};

