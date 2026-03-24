// kv8scope -- Kv8 Software Oscilloscope
// StatsPanel.h -- Collapsible statistics panel (P4.4).
//
// Renders a dockable ImGui window ("Statistics") that shows per-counter
// statistics with both visible-window and full-duration columns side-by-side.
// Toggled with Ctrl+I from ScopeWindow.

#pragma once

#include <kv8/Kv8Types.h>
#include "imgui.h"

#include <cstdint>
#include <string>
#include <vector>

class WaveformRenderer;
class StatsEngine;

class StatsPanel
{
public:
    StatsPanel() = default;

    // Non-copyable, non-movable.
    StatsPanel(const StatsPanel&)            = delete;
    StatsPanel& operator=(const StatsPanel&) = delete;
    StatsPanel(StatsPanel&&)                 = delete;
    StatsPanel& operator=(StatsPanel&&)      = delete;

    /// Initialise from session metadata.  Must be called before Render().
    void Init(const kv8::SessionMeta& meta);

    /// Incrementally add rows for counters in @p meta not already present.
    void AddCounters(const kv8::SessionMeta& meta);

    /// Render the panel.  Should be called unconditionally every frame;
    /// the panel manages its own visibility state (m_bVisible).
    ///
    /// @param pWaveform  Used for visible-window stats and the X-axis limits.
    /// @param pStats     Used for full-duration running accumulators.
    void Render(WaveformRenderer* pWaveform, StatsEngine* pStats);

    /// Toggle visibility.  Called by ScopeWindow on Ctrl+I.
    void ToggleVisible() { m_bVisible = !m_bVisible; }

    bool IsVisible() const { return m_bVisible; }

private:
    struct CounterRow
    {
        uint32_t    dwHash;
        uint16_t    wID;
        std::string sGroupName;
        std::string sName;
    };

    // Flat list of all counters (populated by Init).
    std::vector<CounterRow> m_rows;

    bool m_bVisible = false;

    // Filter text for the name search box.
    char m_szFilter[128] = {};

    // Sort: column index (-1 = none) and direction.
    int  m_iSortCol = -1;
    bool m_bSortAsc = true;
};
