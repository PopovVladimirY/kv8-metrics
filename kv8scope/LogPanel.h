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

    // Selection state -- the timestamp (Unix-epoch ns) of the highlighted
    // entry, set either by clicking a row or by clicking the matching
    // marker on the waveform.  Zero means "no selection".  When non-zero
    // and m_bScrollToSelection is true, the next Render scrolls the table
    // so the selected row is visible (consumed once per selection change).
    uint64_t m_qwSelectedTsNs       = 0;
    bool     m_bScrollToSelection   = false;

    // Last waveform window observed; used in Sync mode to scroll the table
    // to the newest in-window entry whenever the X range changes.
    uint64_t m_qwLastWindowMaxNs    = 0;
};
