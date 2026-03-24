// kv8scope -- Kv8 Software Oscilloscope
// CounterTree.h -- Counter / feed tree panel for ScopeWindow.
//
// Renders a multi-column ImGui table organised as a two-level tree:
//   Channel group (collapsible) > counter rows.
//
// Each counter row exposes:
//   V  -- visibility checkbox; toggling it drives WaveformRenderer directly.
//   E  -- enabled checkbox (live sessions only; Kafka plumbing added in P3.4).
//   [swatch] Name  -- 12x12 px color swatch + counter name text.
//   N              -- full-duration total sample count (StatsEngine).
//   Nv             -- visible-window sample count.
//   Vmin/Vmax/Vavg/Vmed -- visible-window statistics.
//   Min/Max/Avg    -- full-duration statistics (StatsEngine).
//
// Statistics columns display "--" in P3.

#pragma once

#include <kv8/Kv8Types.h>

#include "ConfigStore.h"
#include "StatsEngine.h"
#include "WaveformRenderer.h"
#include "imgui.h"

#include <cstdint>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

class WaveformRenderer;
// StatsEngine forward declaration no longer needed; included via StatsEngine.h.

// ---------------------------------------------------------------------------
// CounterDisplayState
// ---------------------------------------------------------------------------

/// Per-counter display state owned by CounterTree.
struct CounterDisplayState
{
    bool   bVisible     = true;   ///< Trace visible in waveform area.
    bool   bEnabled     = true;   ///< Data collection on (online only, P3.4).
    ImVec4 traceColor   = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); ///< Current trace color.
    ImVec4 traceColorOrig = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); ///< Palette-assigned (for reset).
    double dYMin        = std::numeric_limits<double>::quiet_NaN(); ///< Custom Y min (NaN = registry).
    double dYMax        = std::numeric_limits<double>::quiet_NaN(); ///< Custom Y max (NaN = registry).
    bool   bHighlighted = false;  ///< Bidirectional highlight link (P3.7).
    int    iOrder       = 0;      ///< Display order within group.
    VizMode eVizMode    = VizMode::Range;  ///< Per-counter visualization mode.
};

// ---------------------------------------------------------------------------
// CounterTree
// ---------------------------------------------------------------------------

class CounterTree
{
public:
    CounterTree() = default;

    // Non-copyable, non-movable.
    CounterTree(const CounterTree&)            = delete;
    CounterTree& operator=(const CounterTree&) = delete;
    CounterTree(CounterTree&&)                 = delete;
    CounterTree& operator=(CounterTree&&)      = delete;

    /// Initialise from session metadata.
    /// Fetches palette-assigned trace colors from @p pWaveform; that object
    /// must have had RegisterCounter called for every counter in @p meta
    /// before this function is called.
    ///
    /// @param meta      Session metadata (groups, counters).
    /// @param bOnline   true for live sessions -- shows the E column.
    /// @param pWaveform WaveformRenderer for initial color fetch.
    /// @param pConfig   ConfigStore for persisting per-counter viz modes.
    ///                  May be nullptr (persistence disabled).
    /// @param sPrefix   Session prefix key used as outer JSON key.
    void Init(const kv8::SessionMeta& meta,
              bool bOnline,
              WaveformRenderer* pWaveform,
              ConfigStore* pConfig = nullptr,
              const std::string& sPrefix = "");

    /// Incrementally add counters from @p meta that are not already known.
    /// Existing groups / counters are left unchanged (data preserved).
    /// New entries are appended and sorted into the existing tree.
    void AddCounters(const kv8::SessionMeta& meta,
                     WaveformRenderer* pWaveform);

    /// Render the counter table.  Call every frame inside a suitable child
    /// region.  Drives @p pWaveform visibility state whenever V is toggled.
    ///
    /// @param pWaveform  WaveformRenderer for visible-window stats queries.
    /// @param pStats     StatsEngine for full-duration accumulator reads.
    ///                   May be nullptr (stats will fall back to nTotal from
    ///                   WaveformRenderer and show "--" for full-duration values).
    void Render(WaveformRenderer* pWaveform, StatsEngine* pStats = nullptr);

    /// Bulk visibility helpers (wired to toolbar Show All / Hide All).
    void ShowAll(WaveformRenderer* pWaveform);
    void HideAll(WaveformRenderer* pWaveform);

    /// Set the visualization mode for ALL counters and persist to JSON.
    /// Also updates m_eVizMode (new-counter default) on the WaveformRenderer.
    void BatchSetVizMode(VizMode m, WaveformRenderer* pWaveform);

    /// Highlight a specific counter (clears previous highlight).
    void SetHighlighted(uint32_t dwHash, uint16_t wCounterID,
                        WaveformRenderer* pWaveform);

    /// Clear the current highlight.
    void ClearHighlighted(WaveformRenderer* pWaveform);

    /// Update the E-column visibility at runtime (e.g. when liveness
    /// changes between Live/GoingOffline and Offline/Historical).
    void SetOnline(bool bOnline) { m_bOnline = bOnline; }

    /// Programmatically update a counter's enabled state (e.g. when a
    /// ._ctl Kafka command arrives from another scope instance or the producer).
    /// No-op if (dwHash, wCounterID) is not registered.
    void SetCounterEnabled(uint32_t dwHash, uint16_t wCounterID, bool bEnabled);

    /// Register a callback that is invoked whenever the user toggles the
    /// E (enabled) checkbox for a counter.
    ///
    /// The callback receives (dwGroupHash, wCounterID, bEnabled).
    /// Pass an empty std::function to unregister.
    void SetEnableChangedCallback(
        std::function<void(uint32_t dwHash, uint16_t wID, bool bEnabled)> cb)
    {
        m_onEnableChanged = std::move(cb);
    }

    /// Read-only access to a counter's display state.
    /// Returns nullptr if (dwHash, wCounterID) is not registered.
    const CounterDisplayState* GetState(uint32_t dwHash,
                                        uint16_t wCounterID) const;

private:
    // ---- Internal types -------------------------------------------------

    struct CounterEntry
    {
        uint16_t    wID;
        std::string sName;
        std::string sDisplayName; ///< Human-readable leaf label (from schema "d" key); empty = use last sName segment
        double      dYMinReg = 0.0;  ///< Registry-defined Y min (for popup display).
        double      dYMaxReg = 0.0;  ///< Registry-defined Y max (for popup display).
    };

    struct GroupEntry
    {
        uint32_t                  dwHash;
        std::string               sName;
        std::vector<CounterEntry> counters;
    };

    // ---- Helpers --------------------------------------------------------

    static uint64_t MakeKey(uint32_t dwHash, uint16_t wID)
    { return (static_cast<uint64_t>(dwHash) << 16) | wID; }

    // Extract components back from a composite key.
    static uint32_t KeyHash(uint64_t key) { return static_cast<uint32_t>(key >> 16); }
    static uint16_t KeyID  (uint64_t key) { return static_cast<uint16_t>(key & 0xFFFF); }

    // ---- State ----------------------------------------------------------

    bool                    m_bOnline = false;

    // Config store for persisting per-counter viz mode overrides.
    ConfigStore* m_pConfig = nullptr;
    std::string  m_sPrefix;         ///< Session prefix key in counterVizModes JSON.

    /// Groups in insertion order (mirrors hashToGroup iteration order).
    std::vector<GroupEntry> m_groups;

    /// Per-counter display state.  Key = MakeKey(dwHash, wCounterID).
    std::map<uint64_t, CounterDisplayState> m_state;

    // ---- Highlight state ------------------------------------------------

    uint64_t m_nHighlightedKey = 0;  ///< 0 = none

    // ---- Stats throttle (P4.3) ------------------------------------------
    // Stats columns are expensive (O(W) scan with P2 median) so they are
    // refreshed at 15 Hz (every 4 render frames) and cached between ticks.

    int  m_iStatsTick = 3;  ///< Incremented every Render(); initialized to 3 so the
                            ///< first refresh happens on frame 1 (not frame 4).

    /// Cached visible-window stats per counter.  Stale between refresh ticks.
    std::map<uint64_t, WaveformRenderer::CounterStats> m_statsCache;

    /// Cached full-duration stats per counter (from StatsEngine).
    struct FullStatsCached
    {
        uint64_t n    = 0;
        double   dMin = 0.0;
        double   dMax = 0.0;
        double   dAvg = 0.0;
        bool     bValid = false;
    };
    std::map<uint64_t, FullStatsCached> m_fullStatsCache;

    // ---- Context menu state (P3.5 / P3.6) ------------------------------
    // Set when the user right-clicks a counter row; read next frame when
    // the popup is rendered.

    int    m_iCtxGroup   = -1;   ///< Group index of the row with the active menu.
    int    m_iCtxCounter = -1;   ///< Counter index within that group.

    // Values being edited inside the Y-range modal.
    double m_dPopupYMin  = 0.0;
    double m_dPopupYMax  = 0.0;
    bool   m_bYRangeOpen  = false; ///< Open the Y-range modal next frame.
    bool   m_bColorOpen   = false; ///< Open the color modal next frame.
    bool   m_bCtxPending  = false; ///< Right-click detected inside table; open popup after EndTable().

    // Color value being edited in the color modal.
    float  m_popupCol[3] = {1.0f, 1.0f, 1.0f};

    // Callback fired on E-checkbox toggle (nullptr = no-op).
    std::function<void(uint32_t, uint16_t, bool)> m_onEnableChanged;

    // ---- Private helpers ------------------------------------------------

    /// Persist the viz mode of one counter to ConfigStore and save immediately.
    void SaveCounterVizMode(const std::string& sCounterName, VizMode m);
};
