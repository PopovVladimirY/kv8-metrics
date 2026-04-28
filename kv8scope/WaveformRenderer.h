// kv8scope -- Kv8 Software Oscilloscope
// WaveformRenderer.h -- ImPlot-based waveform chart rendering.
//
// Manages per-counter data buffers (append-only master arrays of
// timestamps and values), drains new samples from SpscRingBuffer
// each frame, and draws live-updating waveforms via ImPlot.
//
// Supports auto-scroll for live sessions, colorblind-safe palette,
// wall-clock time formatting (HH:MM:SS.mmm), and per-counter
// visibility toggles.

#pragma once

#include "AnnotationStore.h"
#include "SpscRingBuffer.h"
#include "StatsEngine.h"

#include "imgui.h"
#include "implot.h"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

class ConfigStore;

/// Waveform visualization mode selected via the toolbar [S]/[R]/[C] buttons.
enum class VizMode
{
    Simple    = 0,  ///< Default: avg line (or raw when N <= W pixels).
    Range     = 1,  ///< Min/max fill region + midpoint line when N > W.
    Cyclogram = 2,  ///< Zero-order-hold staircase.
};

class WaveformRenderer
{
public:
    explicit WaveformRenderer(const ConfigStore* pConfig);
    ~WaveformRenderer() = default;

    // Non-copyable, non-movable.
    WaveformRenderer(const WaveformRenderer&)            = delete;
    WaveformRenderer& operator=(const WaveformRenderer&) = delete;
    WaveformRenderer(WaveformRenderer&&)                 = delete;
    WaveformRenderer& operator=(WaveformRenderer&&)      = delete;

    // ---- Counter registration -------------------------------------------

    /// Register a counter so the renderer allocates master buffers for it.
    /// @param dwHash      Group hash the counter belongs to.
    /// @param wCounterID  Counter ID within the group.
    /// @param sName       Human-readable counter name (used as plot legend).
    /// @param dYMin       Registry-defined minimum value (Y-axis initial range).
    /// @param dYMax       Registry-defined maximum value (Y-axis initial range).
    void RegisterCounter(uint32_t dwHash, uint16_t wCounterID,
                         const std::string& sName,
                         double dYMin = 0.0, double dYMax = 0.0);

    /// Register a counter only if it is not already known.
    /// Returns true if the counter was newly registered, false if it
    /// already existed (no-op in that case).
    bool RegisterCounterIfNew(uint32_t dwHash, uint16_t wCounterID,
                              const std::string& sName,
                              double dYMin = 0.0, double dYMax = 0.0);

    // ---- Per-frame data flow --------------------------------------------

    /// Drain new samples from a SpscRingBuffer into the master buffer.
    /// Called once per (dwHash, wCounterID) per frame from ScopeWindow.
    void DrainRingBuffer(uint32_t dwHash, uint16_t wCounterID,
                         SpscRingBuffer<TelemetrySample>* pRing);

    // ---- Rendering ------------------------------------------------------

    /// Render the ImPlot chart into the available region.
    ///
    /// @param size         Available region size (from ImGui::GetContentRegionAvail()).
    /// @param bAutoScroll  If true, X-axis tracks the live edge.
    /// @param dTimeWindow  Visible time window in seconds (e.g. 15.0).
    ///
    /// @return true if the user panned or zoomed (caller should pause
    ///         auto-scroll).
    bool Render(const ImVec2& size, bool bAutoScroll, double dTimeWindow);

    // ---- Visualization mode ---------------------------------------------

    /// Set the global default mode applied to newly registered counters
    /// and used as the toolbar reference for highlighting.
    void    SetVizMode(VizMode m) { m_eVizMode = m; }
    VizMode GetVizMode()    const { return m_eVizMode; }

    /// Pan the X-axis so that dTimestamp is centred in the visible window.
    /// dTimestamp is in session-relative seconds (subtract GetSessionOrigin()
    /// from any absolute Unix-epoch timestamp before calling).
    /// The current window width is preserved.  The caller (ScopeWindow)
    /// must observe ConsumeNavRequested() and pause auto-scroll, otherwise
    /// the view immediately snaps back to the live edge.
    void NavigateTo(double dTimestamp);

    /// Same as NavigateTo() but also resizes the visible X span to
    /// dSpanSec seconds, centred on dTimestamp.  Used by LogPanel's
    /// auto-resolve mode to zoom in just enough so a selected high-
    /// severity log entry no longer aggregates with its neighbours on
    /// the timeline.  dSpanSec <= 0 falls back to NavigateTo() behaviour.
    void NavigateToWithSpan(double dTimestamp, double dSpanSec);

    /// True if NavigateTo() was called since the last consume.  Used by
    /// ScopeWindow to drop out of live auto-scroll when any panel
    /// (annotations, log, future) requests a programmatic seek.
    bool ConsumeNavRequested()
    {
        bool b = m_bNavRequested;
        m_bNavRequested = false;
        return b;
    }

    /// Consume the most recent log-marker click on the waveform.
    /// Returns true exactly once after the user clicked an event marker;
    /// qwTsNsOut receives the absolute Unix-epoch ns timestamp of the
    /// clicked entry.  LogPanel uses this to scroll its table to the
    /// matching row -- the two views stay synchronised in both directions.
    bool ConsumeLogMarkerClick(uint64_t& qwTsNsOut)
    {
        if (!m_bLogMarkerClicked) return false;
        m_bLogMarkerClicked = false;
        qwTsNsOut = m_qwLogMarkerClickTsNs;
        return true;
    }

    /// Set / get the mode for a single counter.
    void    SetCounterVizMode(uint32_t dwHash, uint16_t wCounterID, VizMode m);
    VizMode GetCounterVizMode(uint32_t dwHash, uint16_t wCounterID) const;

    /// Batch: set m_eVizMode (new-counter default) + every existing counter.
    void    SetAllCountersVizMode(VizMode m);

    // ---- Visibility control ---------------------------------------------

    void SetCounterVisible(uint32_t dwHash, uint16_t wCounterID,
                           bool bVisible);
    void ShowAll();
    void HideAll();

    // ---- Queries --------------------------------------------------------

    /// Return the palette-assigned trace color for a registered counter.
    /// Returns opaque white if the counter is not registered.
    ImVec4 GetCounterColor(uint32_t dwHash, uint16_t wCounterID) const;

    // ---- Color control --------------------------------------------------

    /// Override the trace color for a counter.  The palette-assigned color
    /// is preserved in colorOrig so it can be restored via ResetCounterColor.
    void SetCounterColor(uint32_t dwHash, uint16_t wCounterID,
                         const ImVec4& color);

    /// Return the original palette-assigned color (ignoring any override).
    ImVec4 GetPaletteColor(uint32_t dwHash, uint16_t wCounterID) const;

    // ---- Per-counter Y-axis range override ------------------------------

    /// Override the Y-axis range for one counter.  Triggers a recompute of
    /// the aggregate Y limits used by the plot.
    void SetCounterYRange(uint32_t dwHash, uint16_t wCounterID,
                          double dMin, double dMax);

    /// Remove the Y-axis override and revert to the registry-defined range.
    void ResetCounterYRange(uint32_t dwHash, uint16_t wCounterID);

    // ---- Highlight control ----------------------------------------------

    /// Highlight one counter (drawn last with increased line width).
    /// Clears the previous highlight automatically.
    void SetHighlighted(uint32_t dwHash, uint16_t wCounterID);

    /// Clear any existing highlight.
    void ClearHighlighted();

    /// Call once per frame after Render().
    /// Returns true if the plot area was left-clicked.
    /// If a trace was within 16 px of the click, dwHashOut/wIDOut are non-zero.
    /// If no trace was hit, both are 0 (empty-space click -- clears highlight).
    bool ConsumeLastPlotClick(uint32_t& dwHashOut, uint16_t& wIDOut);

    // ---- Statistics query -----------------------------------------------

    /// Statistics for one counter over a caller-defined time window.
    struct CounterStats
    {
        int64_t nTotal         = 0;      ///< Total sample count (full session).
        int64_t nWindow        = 0;      ///< Sample count inside [dXMin, dXMax].
        double  dMin           = 0.0;    ///< Visible-window minimum.
        double  dMax           = 0.0;    ///< Visible-window maximum.
        double  dAvg           = 0.0;    ///< Visible-window mean.
        double  dMedian        = 0.0;    ///< Visible-window median (P2 estimate).
        bool    bHasWindowData = false;  ///< True when nWindow > 0.
    };

    /// Compute statistics for one counter over the visible time window.
    /// @param dwHash      Group hash.
    /// @param wCounterID  Counter ID.
    /// @param dXMin       Left edge of visible window (Unix epoch seconds).
    /// @param dXMax       Right edge of visible window.
    CounterStats QueryStats(uint32_t dwHash, uint16_t wCounterID,
                            double dXMin, double dXMax) const;

    /// Return the current visible X-axis limits (updated by each Render call).
    void GetVisibleXLimits(double& dXMinOut, double& dXMaxOut) const
    {
        dXMinOut = m_dXLimitMin;
        dXMaxOut = m_dXLimitMax;
    }

    /// Current live-mode window span in seconds (respects user zoom).
    double GetLiveSpan() const { return m_dLiveSpan > 0.0 ? m_dLiveSpan : kMaxLiveSpan; }

    /// Latest wall-clock timestamp across all counters (for auto-scroll
    /// edge and online detection).
    double GetLatestTimestamp() const { return m_dLatestTimestamp; }

    /// Earliest wall-clock timestamp across all counters (for offline
    /// session full-range display).
    double GetEarliestTimestamp() const { return m_dEarliestTimestamp; }

    /// True if any counter has received at least one sample.
    bool HasData() const { return m_bHasAnyData; }

    /// Unix epoch seconds of the first sample -- all internal plot
    /// timestamps are stored as (absolute - origin) offsets.
    double GetSessionOrigin() const { return m_dSessionOrigin; }

    // ---- Data export ----------------------------------------------------

    /// Export all visible counters in [dXMin, dXMax] to a CSV file.
    ///
    /// CSV format (long/tidy):
    ///   timestamp_iso,timestamp_s,counter,value
    /// Rows are sorted by timestamp_s ascending.
    ///
    /// @param dXMin   Start of range (Unix epoch seconds).  Pass
    ///                GetEarliestTimestamp() for the full duration.
    /// @param dXMax   End of range.  Pass GetLatestTimestamp() for
    ///                the full duration.
    /// @param sPath   Absolute or relative path for the output file.
    /// @return true on success, false if the file could not be opened.
    bool ExportCSV(double dXMin, double dXMax,
                   const std::string& sPath) const;

    /// Snap a query time to the nearest visible sample timestamp.
    /// When two samples are equidistant the earlier one is preferred.
    /// Returns dTime unchanged if no visible counter has any data.
    double SnapToNearestSample(double dTime) const;

    // ---- Overview strip (section 6.7.1) ---------------------------------

    /// Render a miniature overview strip of the full session below the
    /// caller's current cursor position.
    ///
    /// The strip draws all visible traces decimated to pixel-column
    /// resolution, plus a semi-transparent highlight rectangle that marks
    /// the currently visible viewport.  Clicking or dragging anywhere on
    /// the strip returns the corresponding timestamp so the caller can
    /// pan there via NavigateTo().
    ///
    /// @param size  Desired width x height.  Pass (0, h) to use the full
    ///              available content width.
    /// @return      The timestamp the user clicked/dragged to, or NaN if
    ///              there was no mouse interaction this frame.
    double RenderOverviewStrip(const ImVec2& size);

    // ---- Annotation support -------------------------------------------

    /// Attach the session's annotation store.  WaveformRenderer does not
    /// own the store; ScopeWindow owns it and passes the raw pointer here.
    void SetAnnotationStore(AnnotationStore* p) { m_pAnnotations = p; }

    /// Master visibility toggle for all annotations.
    void SetAnnotationsVisible(bool b) { m_bAnnotationsVisible = b; }
    bool GetAnnotationsVisible()  const { return m_bAnnotationsVisible; }

    /// Show soft-deleted annotations with transparency.
    void SetShowDeletedAnnotations(bool b) { m_bShowDeletedAnnotations = b; }
    bool GetShowDeletedAnnotations() const { return m_bShowDeletedAnnotations; }

    /// Per-type visibility.  Does not affect the master toggle.
    void SetAnnotationTypeVisible(AnnotationType t, bool b)
    {
        int i = static_cast<int>(t);
        if (i >= 0 && i < AnnotationStore::kTypeCount)
            m_bAnnTypeVisible[i] = b;
    }
    bool GetAnnotationTypeVisible(AnnotationType t) const
    {
        int i = static_cast<int>(t);
        return (i >= 0 && i < AnnotationStore::kTypeCount)
               ? m_bAnnTypeVisible[i] : true;
    }

    /// Call once per frame after Render().
    /// Returns true if the user Ctrl+left-clicked on the plot to request
    /// a new annotation.  dTimestampOut receives the clicked time.
    bool ConsumeAnnotationRequest(double& dTimestampOut)
    {
        if (!m_bAnnotationReq)
            return false;
        m_bAnnotationReq = false;
        dTimestampOut    = m_dAnnotationReqTime;
        return true;
    }

    // ---- Trace-log marker support (Phase L4) --------------------------

    /// Attach the session's log store.  WaveformRenderer does not own the
    /// store; ScopeWindow owns it and passes the raw pointer here.
    void SetLogStore(class LogStore* p) { m_pLogStore = p; }

    /// Master visibility toggle for trace-log event markers.  Default on.
    void SetLogMarkersVisible(bool b) { m_bLogMarkersVisible = b; }
    bool GetLogMarkersVisible() const { return m_bLogMarkersVisible; }

    /// Push the currently selected log entry's timestamp (Unix-epoch ns)
    /// from LogPanel.  When non-zero, every plot draws a dashed vertical
    /// cursor at that time and the matching event marker (if any) is
    /// emphasised with a halo so the two views stay visually linked.
    /// Pass 0 to clear.
    void SetSelectedLogTsNs(uint64_t qwTsNs) { m_qwSelectedLogTsNs = qwTsNs; }

private:
    static constexpr double kNaN        = std::numeric_limits<double>::quiet_NaN();
    static constexpr double kMaxLiveSpan = 15.0;  // maximum live window in seconds
    static constexpr double kMinXSpan    = 1e-9;   // minimum X zoom span (1 ns) – safe because
                                                   // internal timestamps are session-relative

    // ---- Multi-panel constants ------------------------------------------

    static constexpr int kMaxPanels           = 8;
    static constexpr int kMaxCountersPerPanel = 64;

    // ---- Per-panel state ------------------------------------------------

    // Each active panel owns a subset of the visible counters and maintains
    // its own independent Y-axis range.  The X axis is shared across all
    // panels via ImPlotSubplotFlags_LinkAllX.
    struct PanelState
    {
        // Y range tracking.
        //   dYBase      -- fixed bottom (pinned at registration min).
        //   dYTop       -- current top; grows/shrinks with Shift+scroll.
        //   dYOrigRange -- initial span; used to clamp zoom-in.
        //   bYRangeSet  -- false until at least one counter with valid range
        //                  is assigned to this panel.
        double   dYBase      = 0.0;
        double   dYTop       = 1.0;
        double   dYOrigRange = 1.0;
        bool     bYRangeSet  = false;

        // Identity: the key of aKeys[0] from the previous frame.
        // When the primary counter changes, Y range is reset to natural bounds.
        uint64_t primaryKey = 0;

        // Set to true by BuildPanelAssignment when the primary counter changes
        // so the render applies the new limits unconditionally (ImPlotCond_Always)
        // for one frame, overriding ImPlot's cached axis state.
        bool     bForceReset = false;

        // Counter keys assigned to this panel this frame.
        uint64_t aKeys[kMaxCountersPerPanel];
        int      nKeys = 0;
    };

    /// Per-counter master data + display state.
    struct CounterData
    {
        std::string          sName;
        std::vector<double>  vTimestamps;   // append-only, sorted
        std::vector<double>  vValues;       // parallel with vTimestamps
        ImVec4               color;         // current trace color (may be overridden)
        ImVec4               colorOrig;     // palette-assigned color (for reset)
        bool                 bVisible     = true;
        bool                 bHighlighted = false;
        double               dYMin        = 0.0; ///< Registry-defined Y min
        double               dYMax        = 0.0; ///< Registry-defined Y max
        double               dYMinOvr     = kNaN; ///< Custom Y min (NaN = use registry)
        double               dYMaxOvr     = kNaN; ///< Custom Y max (NaN = use registry)
        double               dYUserTop    = kNaN; ///< Last Y-top seen on screen; survives panel reassignment.

        VizMode              eVizMode     = VizMode::Simple; ///< Per-counter viz mode.

        // P2 median cache for QueryStats() -- reset and refed on every call.
        // Declared mutable so QueryStats (logically const) can update the cache.
        mutable P2Median  p2Median;
        mutable double    dP2XMin = 0.0;
        mutable double    dP2XMax = 0.0;
    };

    /// Composite map key from group hash + counter ID (matches
    /// ConsumerThread::MakeKey).
    static uint64_t MakeKey(uint32_t dwHash, uint16_t wID)
    {
        return (static_cast<uint64_t>(dwHash) << 16) | wID;
    }

    /// ImPlot custom tick formatter -- renders Unix epoch seconds as
    /// HH:MM:SS.mmm wall-clock text.
    static int FormatTime(double dValue, char* pBuf, int iSize,
                          void* pUserData);

    /// ImPlot custom tick formatter for the secondary top X2 axis.
    /// Renders a relative time offset (seconds) into the most appropriate
    /// human-readable unit: ns, us, ms, s, m, h, or d.
    static int FormatRelTime(double dValue, char* pBuf, int iSize,
                             void* pUserData);

    // ---- State ----------------------------------------------------------
    const ConfigStore* m_pConfig = nullptr;

    /// Per-counter data keyed by MakeKey(dwHash, wCounterID).
    std::map<uint64_t, CounterData> m_counters;

    // Session-relative (seconds from m_dSessionOrigin).
    double  m_dLatestTimestamp   = 0.0;
    double  m_dEarliestTimestamp = 0.0;

    // Unix epoch seconds of the first sample ever received.  All vTimestamps
    // values and XLimit min/max are stored as (absolute - origin) so that
    // double precision is preserved down to nanosecond intervals even for
    // sessions spanning multiple days.
    double  m_dSessionOrigin  = 0.0;
    bool    m_bHasAnyData     = false;

    int     m_iNextColor = 0;   ///< Round-robin palette index.

    // Scratch buffer for draining ring buffers (avoids per-frame alloc).
    std::vector<TelemetrySample> m_drainBuf;

    // Scratch buffers for decimated plot output (reused every frame).
    std::vector<double> m_scratchX;    ///< Decimated x midpoints.
    std::vector<double> m_scratchY;    ///< Decimated avg (Simple/Cyclogram) or midpoint (Range).
    std::vector<double> m_scratchYLo;  ///< Decimated min (Range mode).
    std::vector<double> m_scratchYHi;  ///< Decimated max (Range mode).

    // X-axis state: limits tracked each frame so X-zoom and overview strip
    // have a valid base.  Updated from any panel's GetPlotLimits() each frame.
    double  m_dXLimitMin =  0.0;
    double  m_dXLimitMax =  1.0;

    // Pending programmatic X navigation (set by NavigateTo, consumed in Render).
    bool    m_bNavPending  = false;
    double  m_dNavTarget   = 0.0;
    double  m_dNavTargetSpan = 0.0;   // > 0 forces a specific span width
    bool    m_bNavRequested = false;  // sticky flag for ConsumeNavRequested()

    // Last log-marker click on the waveform (consumed by LogPanel).
    bool     m_bLogMarkerClicked    = false;
    uint64_t m_qwLogMarkerClickTsNs = 0;

    // X-axis: true once the initial view has been placed with real data.
    bool    m_bInitialViewApplied = false;

    // Live-mode window span (seconds). Adjusted by scroll-zoom while live.
    // 0 = not yet initialised; takes the value from dTimeWindow on first use.
    double  m_dLiveSpan = 0.0;

    // ---- Multi-panel state -----------------------------------------------

    PanelState m_panels[kMaxPanels];  ///< Per-panel Y + key assignment.
    int        m_nActivePanels = 0;   ///< Number of panels rendered this frame.

    // Per-panel screen rects (captured from ImPlot::GetPlotPos/Size each frame).
    ImVec2  m_aPanelPos[kMaxPanels];
    ImVec2  m_aPanelSize[kMaxPanels];

    // Current panel screen rect forwarded to PlotCounterInMode for decimation.
    // Updated to the active panel at the start of each panel's BeginPlot block.
    ImVec2  m_plotPos  = ImVec2(0, 0);
    ImVec2  m_plotSize = ImVec2(1, 1);

    // True when any panel was hovered last frame; used to intercept MouseWheel.
    bool    m_bAnyPlotHoveredLastFrame = false;
    // Which panel was hovered last frame (-1 = none); used for Y-zoom routing.
    int     m_nHoveredPanelLastFrame   = -1;

    // Crossbar persistence: hairline is drawn in ALL panels at the saved
    // screen X even when the mouse is not hovering that panel.
    bool    m_bCrossbarActive  = false; ///< Set when any panel was hovered with crossbar.
    float   m_fCrossbarScreenX = 0.0f;  ///< Saved hairline screen X (pixels).

    // ---- Legacy global Y-axis state (used by RecomputeYRange only) -------
    // Rendering uses per-panel Y in m_panels[].  These members remain to keep
    // SetCounterYRange / ResetCounterYRange functioning.
    double  m_dYBase      = 0.0;
    double  m_dYTop       = 1.0;
    double  m_dYOrigRange = 1.0;
    bool    m_bYRangeSet  = false;

    // ---- Highlighted / click state --------------------------------------
    uint64_t m_nHighlightedKey  = 0;     ///< 0 = none highlighted
    bool     m_bPlotClicked     = false; ///< true for one frame after click
    uint64_t m_nClickedKey      = 0;     ///< key of clicked trace (0 = empty)

    // ---- Annotation state -----------------------------------------------
    AnnotationStore* m_pAnnotations            = nullptr;
    bool             m_bAnnotationsVisible      = true;
    bool             m_bShowDeletedAnnotations  = false;
    bool             m_bAnnTypeVisible[AnnotationStore::kTypeCount];
    bool             m_bAnnotationReq           = false;
    double           m_dAnnotationReqTime       = 0.0;

    // ---- Trace-log marker state (Phase L4) -----------------------------
    class LogStore*  m_pLogStore                = nullptr;
    bool             m_bLogMarkersVisible       = true;
    // Pushed by LogPanel each frame: timestamp (Unix-epoch ns) of the
    // currently selected log entry, or 0 when no row is selected.  Used
    // to draw a dashed vertical cursor + halo on the matching marker.
    uint64_t         m_qwSelectedLogTsNs        = 0;

    // ---- Private helpers ------------------------------------------------

    /// Assign visible counters to panels (called at the top of each Render() frame).
    /// Fills m_panels[] and sets m_nActivePanels.
    void BuildPanelAssignment();

    /// Recompute legacy global Y range from all registered counters.
    /// Called by SetCounterYRange / ResetCounterYRange.
    void RecomputeYRange();

    /// Render one counter using its per-counter VizMode.
    /// Must be called inside a BeginPlot / EndPlot block.
    void PlotCounterInMode(const CounterData& cd, const ImPlotSpec& baseSpec);

    /// Draw annotation markers (dashed vertical lines + flag labels).
    /// Must be called inside a BeginPlot / EndPlot block.
    void RenderAnnotationsInPlot();

    /// Draw trace-log event markers (filled triangles at plot bottom)
    /// for WARN/ERROR/FATAL entries.  Must be called inside a BeginPlot
    /// / EndPlot block.  Phase L4.
    void RenderLogMarkersInPlot();

    /// Draw a dashed vertical cursor at m_qwSelectedLogTsNs (Unix-epoch
    /// ns) so the selected log entry remains visible on every plot even
    /// when m_bLogMarkersVisible is off or the entry is DEBUG/INFO (which
    /// have no triangle marker).  No-op when no row is selected or the
    /// timestamp is outside the visible X window.  Must be called inside
    /// a BeginPlot / EndPlot block.
    void RenderSelectedLogCursorInPlot();

    /// Decimate [pTS,pVS,N] into at most W pixel-column avg samples.
    /// Results written into m_scratchX (x midpoints) and m_scratchY (avg).
    void Decimate(const double* pTS, const double* pVS, int N, int W);

    /// Decimate [pTS,pVS,N] into at most W pixel-column min/avg/max samples.
    /// Results written into m_scratchX, m_scratchYLo (min), m_scratchY (avg),
    /// and m_scratchYHi (max).
    void DecimateRange(const double* pTS, const double* pVS, int N, int W);

    // ---- Visualization mode ---------------------------------------------
    VizMode m_eVizMode = VizMode::Range;

    // ---- Colorblind-safe palette (Wong, adapted for dark background) ----
    static constexpr int kPaletteSize = 8;
    static const ImVec4  s_palette[kPaletteSize];
};
