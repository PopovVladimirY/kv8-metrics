// kv8scope -- Kv8 Software Oscilloscope
// ConfigStore.h -- JSON-backed application configuration.

#pragma once

#include <map>
#include <string>
#include <vector>

struct ScopeConfig
{
    // Connection
    std::string sBrokers          = "localhost:19092";
    std::string sSecurityProtocol = "sasl_plaintext";
    std::string sSaslMechanism    = "PLAIN";
    std::string sUsername          = "kv8producer";
    std::string sPassword          = "kv8secret";
    int         iSessionPollMs    = 2000;
    int         iMetaStabilizeMs  = 4000;  // debounce for MetaUpdated (must be > iSessionPollMs)

    // UI
    std::string sTheme            = "night_sky";
    std::string sFontFace         = "sans_serif";
    int         iFontSize         = 22;
    double      dDefaultTimeWin   = 15.0;
    double      dRealtimeTimeWin  = 15.0;
    int         iMaxPointsPerTrace = 16000000;
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
        int iHeartbeatLiveS      = 7;
        /// Seconds since the last heartbeat before classifying Offline.
        int iHeartbeatDeadS      = 30;
        /// Seconds since the latest data topic message before GoingOffline
        /// (used when no .hb topic exists).
        int iDataLiveS           = 7;
        /// Seconds since the latest data topic message before Offline.
        int iDataDeadS           = 30;
        /// Heartbeat interval in milliseconds (informational; not enforced here).
        int iHeartbeatIntervalMs = 3000;
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
