// kv8scope -- Kv8 Software Oscilloscope
// LogPanel.h -- Dockable trace-log viewer (Phase L4).
//
// Renders the entries owned by a LogStore as a sortable, filterable table
// with severity badges and a per-row click-to-seek action.  Toggled with
// Ctrl+L from ScopeWindow.

#pragma once

#include "imgui.h"

#include <cstdint>
#include <string>

class LogStore;
class WaveformRenderer;

class LogPanel
{
public:
    LogPanel() = default;

    // Non-copyable, non-movable.
    LogPanel(const LogPanel&)            = delete;
    LogPanel& operator=(const LogPanel&) = delete;
    LogPanel(LogPanel&&)                 = delete;
    LogPanel& operator=(LogPanel&&)      = delete;

    /// Bind the panel to a LogStore.  Lifetime is owned by the caller.
    void SetLogStore(LogStore* pStore) { m_pStore = pStore; }

    /// Provide a human-readable session label used to seed default
    /// export filenames (sanitised for the filesystem at use-site).
    /// Optional; pass an empty string or skip the call to fall back to
    /// "trace_log".
    void SetSessionLabel(std::string s) { m_sSessionLabel = std::move(s); }

    /// Tell the panel whether the parent session is currently live.
    /// Offline sessions never receive new entries, so the Follow toggle
    /// is greyed out and treated as off regardless of its stored value.
    void SetSessionLive(bool bLive) { m_bSessionLive = bLive; }

    /// Render the panel.  Call unconditionally every frame.
    /// @param pWaveform Used to read the visible X range for highlighting,
    ///                  and to seek the timeline on row click.
    void Render(WaveformRenderer* pWaveform);

    /// Toggle visibility (Ctrl+L from ScopeWindow).
    void ToggleVisible() { m_bVisible = !m_bVisible; }

    bool IsVisible() const { return m_bVisible; }

    /// Display colour for one severity level.
    static ImVec4 LevelColor(int level);
    /// Subtle row-background tint for one severity level.
    static ImVec4 LevelRowBgColor(int level);
    /// Compact 5-character label for one severity level.
    static const char* LevelLabel(int level);

private:
    LogStore* m_pStore = nullptr;
    bool      m_bVisible = true;

    // Toolbar state.
    char m_szFilter[128] = {};   // Mirrored into LogStore::SetTextFilter()
    bool m_bFollow      = true;  // Auto-scroll table to bottom on new entries
    bool m_bSync        = false; // Two-way scope <-> log selection sync
    bool m_bAutoResolve = false; // Auto-zoom telemetry to resolve a
                                 // selected WARN+ entry from its neighbours
                                 // (does nothing for DEBUG/INFO).
    bool m_bSessionLive = true;  // Pushed by ScopeWindow each frame; offline
                                 // sessions force-disable Follow.

    // Selection state -- the timestamp (Unix-epoch ns) of the highlighted
    // entry, set either by clicking a row or by clicking the matching
    // marker on the waveform.  Zero means "no selection".  When non-zero
    // and m_bScrollToSelection is true, the next Render scrolls the table
    // so the selected row is visible (consumed once per selection change).
    uint64_t m_qwSelectedTsNs       = 0;
    bool     m_bScrollToSelection   = false;

    // Auto-resolve memo: the last selection timestamp we already zoomed
    // for.  Prevents the renderer from being fed a fresh nav request on
    // every frame while the same selection stays active.
    uint64_t m_qwLastResolvedTsNs   = 0;

    // Last waveform window observed; used in Sync mode to scroll the table
    // to the newest in-window entry whenever the X range changes.
    uint64_t m_qwLastWindowMaxNs    = 0;

    // ── Export-to-CSV dialog state ────────────────────────────────────────
    bool m_bShowExportDialog        = false;
    int  m_iExportRange             = 0;     // 0 = all, 1 = visible window
    bool m_bExportApplyFilter       = true;  // honour level mask + text filter
    char m_szExportPath[1024]       = {};
    char m_szExportStatus[256]      = {};
    // True until the user manually edits the path.  While true, the
    // dialog regenerates a suggested filename every frame from the
    // session label and the chosen range so it always matches the
    // current selection.
    bool m_bExportPathAuto          = true;

    // Session label provided by the host (e.g. ScopeWindow).  Empty
    // means "no host hint -- use a generic prefix".
    std::string m_sSessionLabel;
};
