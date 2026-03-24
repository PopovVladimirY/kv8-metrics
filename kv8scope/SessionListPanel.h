// kv8scope -- Kv8 Software Oscilloscope
// SessionListPanel.h -- Dockable session tree with table columns,
//                       context menu, and detail/delete popups.

#pragma once

#include "ThemeManager.h"
#include "SessionManager.h"

#include <kv8/Kv8Types.h>

#include <string>
#include <vector>

class ConfigStore;
class SessionManager;

// ---------------------------------------------------------------------------
// SessionEntry -- one discovered session kept in display order
// ---------------------------------------------------------------------------

struct SessionEntry
{
    std::string      sChannel;
    std::string      sPrefix;
    kv8::SessionMeta meta;
    SessionLiveness  eLiveness     = SessionLiveness::Unknown;
    bool             bSelected     = false;
    bool             bOpenRequested = false;
};

// ---------------------------------------------------------------------------
// SessionListPanel
// ---------------------------------------------------------------------------

class SessionListPanel
{
public:
    SessionListPanel();
    ~SessionListPanel() = default;

    // Non-copyable
    SessionListPanel(const SessionListPanel&)            = delete;
    SessionListPanel& operator=(const SessionListPanel&) = delete;

    /// Main render call -- draws the "Sessions" ImGui window.
    void Render();

    // ---- Data mutation (called by App after draining SessionManager) ------

    void AddSession(const std::string& sChannel,
                    const std::string& sPrefix,
                    const kv8::SessionMeta& meta);

    void RemoveSession(const std::string& sPrefix);

    /// Update the stored SessionMeta for an already-known session in-place.
    /// Called when MetaUpdated fires (e.g. UDT schemas registered after first
    /// discovery).  Does nothing if the prefix is not found.
    void UpdateSessionMeta(const std::string& sPrefix,
                           const kv8::SessionMeta& meta);

    void SetSessionLiveness(const std::string& sPrefix, SessionLiveness eLiveness);

    /// Legacy helper: maps bool to Live / Offline for callers that haven't
    /// been updated yet.  Prefer SetSessionLiveness().
    void SetSessionOnline(const std::string& sPrefix, bool bOnline);

    /// Clear every session (e.g. on broker reconnect).
    void Clear();

    // ---- Queries for App -------------------------------------------------

    /// Returns true if any session has bOpenRequested set.
    /// Clears the flag after returning.  outLiveness reflects the session's
    /// current liveness state.
    bool ConsumeOpenRequest(std::string& outPrefix,
                            kv8::SessionMeta& outMeta,
                            SessionLiveness& outLiveness);

    /// Access sessions (read-only).
    const std::vector<SessionEntry>& Sessions() const { return m_sessions; }
    size_t SessionCount() const { return m_sessions.size(); }

    /// Set the connection state string shown when the list is empty.
    void SetConnectionHint(const std::string& sHint) { m_sConnHint = sHint; }

    /// Provide pointers so the panel can issue delete commands.
    void SetBackends(SessionManager* pSM, ConfigStore* pCfg)
    {
        m_pSessionMgr = pSM;
        m_pConfig     = pCfg;
    }

    /// Provide a stable pointer to the active theme palette.
    /// Pass &pThemeManager->Colors().  The pointer must outlive this object.
    void SetTheme(const ThemeColors* pColors) { m_pThemeColors = pColors; }

private:
    void RenderSessionRow(size_t idx);
    void RenderContextMenu(size_t idx);
    void RenderInspectPopup();
    void RenderDeleteSessionPopup();
    void RenderDeleteChannelPopup();

    /// Parse a human-readable start time from the session ID string.
    static std::string ParseStartTime(const std::string& sSessionID);

    /// Count total counters across all groups.
    static int CountCounters(const kv8::SessionMeta& meta);

    /// Convert a Kafka-sanitized channel name to human-readable form
    /// (dots back to slashes, e.g. "kv8.test" -> "kv8/test").
    static std::string HumanChannel(const std::string& sChannel);

    std::vector<SessionEntry> m_sessions;
    std::string               m_sConnHint = "Connecting to broker...";

    // Popup state
    bool             m_bShowInspect        = false;
    bool             m_bShowDeleteSession  = false;
    bool             m_bShowDeleteChannel  = false;
    size_t           m_inspectIdx          = 0;
    std::vector<std::string> m_deleteSessionPrefixes; // 1 or more sessions to delete
    std::string      m_deleteChannelName;

    // Selection anchor for Shift+click range selection.
    size_t           m_lastClickedIdx = static_cast<size_t>(-1);

    // Back-pointers (non-owning)
    SessionManager*      m_pSessionMgr   = nullptr;
    ConfigStore*         m_pConfig       = nullptr;
    const ThemeColors*   m_pThemeColors  = nullptr;
};

