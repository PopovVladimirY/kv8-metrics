// kv8scope -- Kv8 Software Oscilloscope
// ConfigStore.h -- JSON-backed application configuration.

#pragma once

#include "Constants.h"
#include <kv8/Kv8Constants.h>

#include <map>
#include <string>
#include <vector>

struct ScopeConfig
{
    // Connection -- defaults pulled from <kv8/Kv8Constants.h> so that
    // producer (kv8log runtime) and consumer (kv8scope) cannot drift.
    std::string sBrokers          = kv8::KV8_DEFAULT_BROKERS;
    std::string sSecurityProtocol = kv8::KV8_DEFAULT_SECURITY_PROTO;
    std::string sSaslMechanism    = kv8::KV8_DEFAULT_SASL_MECHANISM;
    std::string sUsername          = kv8::KV8_DEFAULT_USER;
    std::string sPassword          = kv8::KV8_DEFAULT_PASSWORD;
    int         iSessionPollMs    = kv8::KV8_SESSION_POLL_MS;
    int         iMetaStabilizeMs  = kv8::KV8_META_STABILIZE_MS;

    // UI
    std::string sTheme            = "night_sky";
    std::string sFontFace         = "sans_serif";
    int         iFontSize         = 22;
    double      dDefaultTimeWin   = kv8scope::DEFAULT_TIME_WINDOW_S;
    double      dRealtimeTimeWin  = kv8scope::REALTIME_TIME_WINDOW_S;
    int         iMaxPointsPerTrace = kv8scope::MAX_POINTS_PER_TRACE;
    std::string sVizMode          = "range";
    bool        bCrossBar         = true;  // show crosshair + value tooltip

    // Runtime
    std::vector<std::string> recentSessions;

    // Per-session per-counter visualization mode overrides.
    // Outer key: session prefix ("<channel>.<sessionID>").
    // Inner key: counter name.
    // Value: "simple" | "range" | "cyclogram".
    std::map<std::string, std::map<std::string, std::string>> counterVizModes;

    // Per-table column widths persisted across sessions.
    // Key: table ID string (e.g. "##CounterTable_12", "##SessionTable").
    // Value: ordered vector of column widths in pixels.
    std::map<std::string, std::vector<float>> tableColumnWidths;

    // Session liveness detection thresholds.
    // These values are read by SessionManager; changing them takes effect
    // on the next poll cycle after Save() is called.
    struct LivenessConfig
    {
        /// Seconds since the last heartbeat before classifying GoingOffline.
        int iHeartbeatLiveS      = kv8::KV8_LIVENESS_OFFLINE_THRESHOLD_S;
        /// Seconds since the last heartbeat before classifying Offline.
        int iHeartbeatDeadS      = kv8::KV8_LIVENESS_DEAD_THRESHOLD_S;
        /// Seconds since the latest data topic message before GoingOffline
        /// (used when no .hb topic exists).
        int iDataLiveS           = kv8::KV8_LIVENESS_OFFLINE_THRESHOLD_S;
        /// Seconds since the latest data topic message before Offline.
        int iDataDeadS           = kv8::KV8_LIVENESS_DEAD_THRESHOLD_S;
        /// Heartbeat interval in milliseconds (informational; not enforced here).
        int iHeartbeatIntervalMs = kv8::KV8_HEARTBEAT_INTERVAL_MS;
    } liveness;
};

class ConfigStore
{
public:
    explicit ConfigStore(const std::string& sPath);
    ~ConfigStore() = default;

    // Non-copyable
    ConfigStore(const ConfigStore&)            = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    // Load from disk.  Returns true on success, false if file was missing
    // (defaults are kept).
    bool Load();

    // Save to disk.  Creates the file if it does not exist.
    // Returns true on success.
    bool Save();

    // Read-only access.
    const ScopeConfig& Get() const { return m_config; }

    // Mutable access -- caller must call MarkDirty() after changes.
    ScopeConfig& GetMut() { return m_config; }

    void MarkDirty() { m_bDirty = true; }
    bool IsDirty() const { return m_bDirty; }

    const std::string& GetPath() const { return m_sPath; }

private:
    std::string m_sPath;
    ScopeConfig m_config;
    bool        m_bDirty = false;
};
