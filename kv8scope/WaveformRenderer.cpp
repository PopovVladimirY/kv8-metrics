// kv8scope -- Kv8 Software Oscilloscope
// WaveformRenderer.cpp -- ImPlot-based waveform chart rendering.

#include "WaveformRenderer.h"
#include "ConfigStore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

// ---------------------------------------------------------------------------
// Colorblind-safe palette (Wong, 2011 -- adapted for dark background)
// ---------------------------------------------------------------------------
// Original Wong palette: #E69F00, #56B4E9, #009E73, #F0E442,
//                        #0072B2, #D55E00, #CC79A7, #000000
// Last entry replaced with #e6edf3 (primary text) for dark themes.

const ImVec4 WaveformRenderer::s_palette[kPaletteSize] = {
    ImVec4(0.902f, 0.624f, 0.000f, 1.0f),  // #E69F00  orange
    ImVec4(0.337f, 0.706f, 0.914f, 1.0f),  // #56B4E9  sky blue
    ImVec4(0.000f, 0.620f, 0.451f, 1.0f),  // #009E73  bluish green
    ImVec4(0.941f, 0.894f, 0.259f, 1.0f),  // #F0E442  yellow
    ImVec4(0.000f, 0.447f, 0.698f, 1.0f),  // #0072B2  blue
    ImVec4(0.835f, 0.369f, 0.000f, 1.0f),  // #D55E00  vermillion
    ImVec4(0.800f, 0.475f, 0.655f, 1.0f),  // #CC79A7  reddish purple
    ImVec4(0.902f, 0.929f, 0.953f, 1.0f),  // #e6edf3  light (replaces black)
};

// ---------------------------------------------------------------------------
// Contrast helpers (WCAG relative luminance)
// ---------------------------------------------------------------------------
// Returns the WCAG-2 relative luminance of a linear-light value.
static float SRGBToLinear(float c)
{
    return (c <= 0.04045f) ? c / 12.92f
                           : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

static float Luminance(const ImVec4& c)
{
    return 0.2126f * SRGBToLinear(c.x)
         + 0.7152f * SRGBToLinear(c.y)
         + 0.0722f * SRGBToLinear(c.z);
}

static float ContrastRatio(float L1, float L2)
{
    const float hi = (L1 > L2) ? L1 : L2;
    const float lo = (L1 > L2) ? L2 : L1;
    return (hi + 0.05f) / (lo + 0.05f);
}

// Ensure fg has at least minRatio contrast against bg.
// Lerps fg toward white (dark bg) or black (light bg) until the ratio is met.
// Alpha is preserved unchanged.
static ImVec4 EnsureContrast(ImVec4 fg, const ImVec4& bg, float minRatio = 3.0f)
{
    const float Lbg = Luminance(bg);
    if (ContrastRatio(Luminance(fg), Lbg) >= minRatio)
        return fg;

    // Push toward white for dark backgrounds, toward black for light ones.
    const bool  bDarkBg = (Lbg < 0.18f);
    const float tx = bDarkBg ? 1.0f : 0.0f;
    const float ty = bDarkBg ? 1.0f : 0.0f;
    const float tz = bDarkBg ? 1.0f : 0.0f;

    // Binary search for the minimum blend that achieves the required ratio.
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 10; ++i)
    {
        const float  mid = (lo + hi) * 0.5f;
        const ImVec4 c(fg.x + (tx - fg.x) * mid,
                       fg.y + (ty - fg.y) * mid,
                       fg.z + (tz - fg.z) * mid,
                       fg.w);
        if (ContrastRatio(Luminance(c), Lbg) >= minRatio)
            hi = mid;
        else
            lo = mid;
    }
    return ImVec4(fg.x + (tx - fg.x) * hi,
                  fg.y + (ty - fg.y) * hi,
                  fg.z + (tz - fg.z) * hi,
                  fg.w);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

WaveformRenderer::WaveformRenderer(const ConfigStore* pConfig)
    : m_pConfig(pConfig)
{
    // Pre-allocate the drain scratch buffer.  4096 samples per drain
    // call is a reasonable batch size; the buffer will grow if needed.
    m_drainBuf.resize(4096);

    // All annotation types are visible by default.
    for (int i = 0; i < AnnotationStore::kTypeCount; ++i)
        m_bAnnTypeVisible[i] = true;
}

// ---------------------------------------------------------------------------
// Counter registration
// ---------------------------------------------------------------------------

void WaveformRenderer::RegisterCounter(uint32_t dwHash, uint16_t wCounterID,
                                       const std::string& sName,
                                       double dYMin, double dYMax)
{
    uint64_t key = MakeKey(dwHash, wCounterID);
    if (m_counters.find(key) != m_counters.end())
        return;   // already registered

    CounterData cd;
    cd.sName     = sName;
    cd.color     = s_palette[m_iNextColor % kPaletteSize];
    cd.colorOrig = cd.color;
    cd.bVisible  = true;
    cd.dYMin     = dYMin;
    cd.dYMax     = dYMax;
    cd.dYMinOvr  = kNaN;
    cd.dYMaxOvr  = kNaN;
    cd.eVizMode  = m_eVizMode;  // inherit current global default

    // Expand the aggregate Y range used to initialise the Y axis.
    // m_dYBase is the lowest registry min across all counters (pinned bottom).
    // m_dYTop  is the highest registry max (mutable visible top).
    if (dYMin < dYMax)
    {
        if (!m_bYRangeSet)
        {
            m_dYBase      = dYMin;
            m_dYTop       = dYMax;
            m_dYOrigRange = dYMax - dYMin;
            m_bYRangeSet  = true;
        }
        else
        {
            if (dYMin < m_dYBase) m_dYBase = dYMin;
            if (dYMax > m_dYTop)
            {
                m_dYTop       = dYMax;
                m_dYOrigRange = m_dYTop - m_dYBase;
            }
        }
    }

    // Reserve a generous initial capacity to reduce early reallocations.
    cd.vTimestamps.reserve(65536);
    cd.vValues.reserve(65536);

    m_counters.emplace(key, std::move(cd));
    ++m_iNextColor;
}

bool WaveformRenderer::RegisterCounterIfNew(
    uint32_t dwHash, uint16_t wCounterID,
    const std::string& sName, double dYMin, double dYMax)
{
    uint64_t key = MakeKey(dwHash, wCounterID);
    if (m_counters.find(key) != m_counters.end())
        return false;  // already registered
    RegisterCounter(dwHash, wCounterID, sName, dYMin, dYMax);
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame ring buffer drain
// ---------------------------------------------------------------------------

void WaveformRenderer::DrainRingBuffer(uint32_t dwHash, uint16_t wCounterID,
                                       SpscRingBuffer<TelemetrySample>* pRing)
{
    if (!pRing)
        return;

    uint64_t key = MakeKey(dwHash, wCounterID);
    auto it = m_counters.find(key);
    if (it == m_counters.end())
        return;

    CounterData& cd = it->second;

    // Drain in batches until the ring is empty.
    for (;;)
    {
        size_t n = pRing->PopBatch(m_drainBuf.data(), m_drainBuf.size());
        if (n == 0)
            break;

        // On the very first batch, lock the session origin to the earliest
        // absolute timestamp in the batch so that all stored values are
        // session-relative offsets near zero.  This preserves double
        // precision down to ~0.1 ns even for sessions spanning many days.
        if (!m_bHasAnyData)
        {
            double dEarlyAbs = m_drainBuf[0].dTimestamp;
            for (size_t i = 1; i < n; ++i)
                if (m_drainBuf[i].dTimestamp < dEarlyAbs)
                    dEarlyAbs = m_drainBuf[i].dTimestamp;
            if (m_dSessionOrigin <= 0.0)
                m_dSessionOrigin = dEarlyAbs;
        }

        for (size_t i = 0; i < n; ++i)
        {
            const double dRel = m_drainBuf[i].dTimestamp - m_dSessionOrigin;
            cd.vTimestamps.push_back(dRel);
            cd.vValues.push_back(m_drainBuf[i].dValue);

            if (dRel > m_dLatestTimestamp)
                m_dLatestTimestamp = dRel;

            if (!m_bHasAnyData || dRel < m_dEarliestTimestamp)
                m_dEarliestTimestamp = dRel;

            m_bHasAnyData = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Visibility control
// ---------------------------------------------------------------------------

void WaveformRenderer::SetCounterVisible(uint32_t dwHash, uint16_t wCounterID,
                                         bool bVisible)
{
    uint64_t key = MakeKey(dwHash, wCounterID);
    auto it = m_counters.find(key);
    if (it != m_counters.end())
        it->second.bVisible = bVisible;
}

void WaveformRenderer::ShowAll()
{
    for (auto& kv : m_counters)
        kv.second.bVisible = true;
}

void WaveformRenderer::HideAll()
{
    for (auto& kv : m_counters)
        kv.second.bVisible = false;
}

ImVec4 WaveformRenderer::GetCounterColor(uint32_t dwHash,
                                          uint16_t wCounterID) const
{
    auto it = m_counters.find(MakeKey(dwHash, wCounterID));
    if (it == m_counters.end())
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    return it->second.color;
}

// ---------------------------------------------------------------------------
// Color control
// ---------------------------------------------------------------------------

void WaveformRenderer::SetCounterColor(uint32_t dwHash, uint16_t wCounterID,
                                        const ImVec4& color)
{
    auto it = m_counters.find(MakeKey(dwHash, wCounterID));
    if (it != m_counters.end())
        it->second.color = color;
}

ImVec4 WaveformRenderer::GetPaletteColor(uint32_t dwHash,
                                          uint16_t wCounterID) const
{
    auto it = m_counters.find(MakeKey(dwHash, wCounterID));
    if (it == m_counters.end())
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    return it->second.colorOrig;
}

// ---------------------------------------------------------------------------
// Y-axis range override
// ---------------------------------------------------------------------------

void WaveformRenderer::SetCounterYRange(uint32_t dwHash, uint16_t wCounterID,
                                         double dMin, double dMax)
{
    auto it = m_counters.find(MakeKey(dwHash, wCounterID));
    if (it == m_counters.end())
        return;
    it->second.dYMinOvr = dMin;
    it->second.dYMaxOvr = dMax;
    RecomputeYRange();
}

void WaveformRenderer::ResetCounterYRange(uint32_t dwHash, uint16_t wCounterID)
{
    auto it = m_counters.find(MakeKey(dwHash, wCounterID));
    if (it == m_counters.end())
        return;
    it->second.dYMinOvr = kNaN;
    it->second.dYMaxOvr = kNaN;
    RecomputeYRange();
}

void WaveformRenderer::RecomputeYRange()
{
    bool bFirst = true;
    for (const auto& [key, cd] : m_counters)
    {
        double effMin = (!std::isnan(cd.dYMinOvr)) ? cd.dYMinOvr : cd.dYMin;
        double effMax = (!std::isnan(cd.dYMaxOvr)) ? cd.dYMaxOvr : cd.dYMax;
        if (effMin >= effMax)
            continue;
        if (bFirst)
        {
            m_dYBase      = effMin;
            m_dYTop       = effMax;
            m_dYOrigRange = effMax - effMin;
            m_bYRangeSet  = true;
            bFirst        = false;
        }
        else
        {
            if (effMin < m_dYBase) m_dYBase = effMin;
            if (effMax > m_dYTop)
            {
                m_dYTop       = effMax;
                m_dYOrigRange = m_dYTop - m_dYBase;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// BuildPanelAssignment -- assign visible counters to panels (called each frame)
//
// With N visible counters:
//   N <= kMaxPanels : one counter per panel.
//   N  > kMaxPanels : greedy Y-range overlap binning into kMaxPanels buckets.
//
// The assignment is stable across frames (m_panels array is reused).
// Per-panel Y state (dYTop from Shift+scroll) is preserved when the same
// counter stays in the same panel slot.
// ---------------------------------------------------------------------------

void WaveformRenderer::BuildPanelAssignment()
{
    // ---- Collect visible counters sorted by effective dYMin. -----------
    struct VisEntry
    {
        uint64_t key;
        double   yMin;
        double   yMax;
        const char* pName; // points into CounterData::sName
    };

    static constexpr int kMaxVisible = 256;
    VisEntry vis[kMaxVisible];
    int      nVis = 0;

    for (const auto& [key, cd] : m_counters)
    {
        if (!cd.bVisible)
            continue;
        if (nVis >= kMaxVisible)
            break;

        double yMin = std::isnan(cd.dYMinOvr) ? cd.dYMin : cd.dYMinOvr;
        double yMax = std::isnan(cd.dYMaxOvr) ? cd.dYMax : cd.dYMaxOvr;
        vis[nVis++] = { key, yMin, yMax, cd.sName.c_str() };
    }

    if (nVis == 0)
    {
        m_nActivePanels = 0;
        return;
    }

    // Sort by yMin ascending so the greedy pass runs in a natural order.
    std::sort(vis, vis + nVis, [](const VisEntry& a, const VisEntry& b)
    {
        return a.yMin < b.yMin;
    });

    const int nPanels = (nVis <= kMaxPanels) ? nVis : kMaxPanels;

    // ---- Save old primary keys and key counts so we can detect changes. ----
    uint64_t oldPrimary[kMaxPanels];
    int      oldNKeys[kMaxPanels];
    for (int p = 0; p < nPanels; ++p)
    {
        oldPrimary[p] = m_panels[p].primaryKey;
        oldNKeys[p]   = m_panels[p].nKeys;
    }

    // Reset key lists for this frame.
    for (int p = 0; p < nPanels; ++p)
        m_panels[p].nKeys = 0;

    // ---- Per-counter panel index. -------------------------------------
    int assign[kMaxVisible];

    if (nVis <= kMaxPanels)
    {
        // One counter per panel: panel index == counter index.
        for (int i = 0; i < nVis; ++i)
            assign[i] = i;
    }
    else
    {
        // Contiguous-run partitioning on the yMin-sorted counter list.
        //
        // Slice the sorted array into exactly nPanels contiguous runs.
        // Counters adjacent in yMin order have the most similar Y ranges,
        // so they naturally share the same panel.
        //
        // Guarantees:
        //   - Every panel gets floor(nVis/nPanels) or ceil(nVis/nPanels) counters.
        //   - No panel is ever empty while another is overcrowded.
        //   - Similar-range counters are grouped (not scattered).
        const int nBase  = nVis / nPanels; // minimum counters per panel
        const int nExtra = nVis % nPanels; // first nExtra panels get one more

        int ci = 0;
        for (int p = 0; p < nPanels; ++p)
        {
            const int nThisPanel = nBase + (p < nExtra ? 1 : 0);
            for (int k = 0; k < nThisPanel; ++k)
                assign[ci++] = p;
        }
    }

    // ---- Populate panels from the assignment. -------------------------
    for (int i = 0; i < nVis; ++i)
    {
        const int p = assign[i];
        PanelState& ps = m_panels[p];
        if (ps.nKeys < kMaxCountersPerPanel)
            ps.aKeys[ps.nKeys++] = vis[i].key;
    }

    // ---- Update per-panel Y ranges. -----------------------------------
    // If the primary counter changed, reset to the natural data range.
    // Otherwise preserve the user-adjusted dYTop (Shift+scroll zoom state).
    for (int p = 0; p < nPanels; ++p)
    {
        PanelState& ps = m_panels[p];
        if (ps.nKeys == 0)
            continue;

        // Compute the union Y range across all counters in this panel.
        double yUnionMin =  std::numeric_limits<double>::max();
        double yUnionMax = -std::numeric_limits<double>::max();
        bool   bAnyRange = false;

        for (int ki = 0; ki < ps.nKeys; ++ki)
        {
            auto itC = m_counters.find(ps.aKeys[ki]);
            if (itC == m_counters.end()) continue;
            const CounterData& cd = itC->second;
            const double yMin = std::isnan(cd.dYMinOvr) ? cd.dYMin : cd.dYMinOvr;
            const double yMax = std::isnan(cd.dYMaxOvr) ? cd.dYMax : cd.dYMaxOvr;
            if (yMin < yMax)
            {
                if (yMin < yUnionMin) yUnionMin = yMin;
                if (yMax > yUnionMax) yUnionMax = yMax;
                bAnyRange = true;
            }
        }

        const uint64_t newPrimary = ps.aKeys[0];
        // Composition changed if the primary counter differs OR the number of
        // counters in this panel changed (e.g. a counter moved in/out after a
        // visibility toggle).  Either event requires a Y-range reset so stale
        // limits from the old panel membership are not carried forward.
        const bool     bChanged   = (newPrimary != oldPrimary[p])
                                 || !ps.bYRangeSet
                                 || (ps.nKeys != oldNKeys[p]);

        if (bAnyRange)
        {
            ps.dYBase     = yUnionMin;
            ps.dYOrigRange = yUnionMax - yUnionMin;
            ps.bYRangeSet  = true;

            if (bChanged)
            {
                ps.bForceReset = true;
                // Restore the Y top that was last seen for the primary counter
                // so that simple visibility toggles don't lose user zoom state.
                // Guard: dYUserTop must not exceed yUnionMax of the NEW panel
                // composition -- a stale value from a previous wider panel
                // (e.g. battery was co-displayed with motor RPM 0-14000) would
                // otherwise propagate an incorrect scale to the solo panel.
                auto itPrim = m_counters.find(newPrimary);
                if (itPrim != m_counters.end()
                    && !std::isnan(itPrim->second.dYUserTop)
                    && itPrim->second.dYUserTop >  yUnionMin
                    && itPrim->second.dYUserTop <= yUnionMax * 1.1)
                    ps.dYTop = itPrim->second.dYUserTop;
                else
                    ps.dYTop = yUnionMax;
            }
            else
            {
                // Same counters: preserve user's Y-zoom top.
                // Grow the natural maximum to cover any new samples but do
                // not shrink a user-expanded view.
                if (yUnionMax > ps.dYTop)
                    ps.dYTop = yUnionMax;
            }
        }
        else
        {
            ps.bYRangeSet = false;
        }

        ps.primaryKey = newPrimary;
    }

    m_nActivePanels = nPanels;
}

// ---------------------------------------------------------------------------
// Highlight control
// ---------------------------------------------------------------------------

void WaveformRenderer::SetHighlighted(uint32_t dwHash, uint16_t wCounterID)
{
    uint64_t newKey = MakeKey(dwHash, wCounterID);
    for (auto& [key, cd] : m_counters)
        cd.bHighlighted = (key == newKey);
    m_nHighlightedKey = newKey;
}

void WaveformRenderer::ClearHighlighted()
{
    for (auto& [key, cd] : m_counters)
        cd.bHighlighted = false;
    m_nHighlightedKey = 0;
}

bool WaveformRenderer::ConsumeLastPlotClick(uint32_t& dwHashOut,
                                             uint16_t& wIDOut)
{
    if (!m_bPlotClicked)
        return false;
    m_bPlotClicked = false;
    dwHashOut = static_cast<uint32_t>(m_nClickedKey >> 16);
    wIDOut    = static_cast<uint16_t>(m_nClickedKey & 0xFFFFu);
    return true;
}

// ---------------------------------------------------------------------------
// Statistics query
// ---------------------------------------------------------------------------

WaveformRenderer::CounterStats WaveformRenderer::QueryStats(
    uint32_t dwHash, uint16_t wCounterID,
    double dXMin, double dXMax) const
{
    CounterStats s;
    auto it = m_counters.find(MakeKey(dwHash, wCounterID));
    if (it == m_counters.end())
        return s;

    const CounterData& cd = it->second;
    s.nTotal = static_cast<int64_t>(cd.vValues.size());
    if (s.nTotal == 0 || dXMax < dXMin)
        return s;

    // Find first sample >= dXMin via binary search.
    auto itBegin = std::lower_bound(cd.vTimestamps.cbegin(),
                                    cd.vTimestamps.cend(), dXMin);
    if (itBegin == cd.vTimestamps.cend())
        return s;  // all data is before the visible window

    ptrdiff_t iStart = itBegin - cd.vTimestamps.cbegin();
    double dMin = std::numeric_limits<double>::max();
    double dMax = std::numeric_limits<double>::lowest();
    double dSum = 0.0;
    int64_t n   = 0;

    // P2 median: reset on every call so the estimate reflects exactly the
    // current visible window.  QueryStats() is called at 15 Hz from the
    // counter tree (throttled by m_iStatsTick in CounterTree), so a full
    // O(W) re-scan is affordable.
    cd.p2Median.Reset();
    cd.dP2XMin = dXMin;
    cd.dP2XMax = dXMax;

    for (ptrdiff_t i = iStart;
         i < static_cast<ptrdiff_t>(cd.vTimestamps.size()); ++i)
    {
        if (cd.vTimestamps[i] > dXMax)
            break;
        const double v = cd.vValues[i];
        if (v < dMin) dMin = v;
        if (v > dMax) dMax = v;
        dSum += v;
        ++n;
        cd.p2Median.Feed(v);
    }

    if (n > 0)
    {
        s.nWindow        = n;
        s.dMin           = dMin;
        s.dMax           = dMax;
        s.dAvg           = dSum / static_cast<double>(n);
        s.dMedian        = cd.p2Median.Get();
        s.bHasWindowData = true;
    }
    return s;
}

// ---------------------------------------------------------------------------
// NavigateTo -- pan the X-axis to centre on the given timestamp.
//
// The current window width is preserved.  The pending flag is consumed
// in Render() before the next BeginPlot call.
// ---------------------------------------------------------------------------

void WaveformRenderer::NavigateTo(double dTimestamp)
{
    m_bNavPending = true;
    m_dNavTarget  = dTimestamp;
}

// ---------------------------------------------------------------------------
// SnapToNearestSample -- find the visible sample timestamp closest to dTime.
//
// Searches all visible counters.  For ties (two equidistant samples from
// different counters) the earlier timestamp is returned, which corresponds
// to the first/earliest sample of a dense cluster.
// ---------------------------------------------------------------------------

double WaveformRenderer::SnapToNearestSample(double dTime) const
{
    double dBestTime = dTime;
    double dBestDist = std::numeric_limits<double>::max();

    for (const auto& [key, cd] : m_counters)
    {
        if (!cd.bVisible || cd.vTimestamps.empty())
            continue;

        // Find the first sample >= dTime via binary search.
        auto it = std::lower_bound(cd.vTimestamps.cbegin(),
                                   cd.vTimestamps.cend(), dTime);

        // Check the found sample and the one immediately before it.
        for (int off = -1; off <= 0; ++off)
        {
            auto jt = it;
            if (off < 0)
            {
                if (jt == cd.vTimestamps.cbegin()) continue;
                --jt;
            }
            if (jt == cd.vTimestamps.cend()) continue;

            const double ts   = *jt;
            const double dist = std::fabs(ts - dTime);

            // Strictly closer, OR equidistant but earlier (prefer earliest
            // sample so dense clusters produce the first/left-most marker).
            if (dist < dBestDist ||
                (dist == dBestDist && ts < dBestTime))
            {
                dBestDist = dist;
                dBestTime = ts;
            }
        }
    }

    return dBestTime;
}

// ---------------------------------------------------------------------------
// RenderOverviewStrip -- miniature full-session waveform strip (section 6.7.1)
//
// Draws directly onto the current window's ImDrawList.
// Returns the timestamp the user clicked/dragged to, or NaN if no
// interaction occurred this frame.
// ---------------------------------------------------------------------------

double WaveformRenderer::RenderOverviewStrip(const ImVec2& size)
{
    // If there is no data yet, consume the layout space and return.
    if (!HasData())
    {
        ImGui::Dummy(size);
        return kNaN;
    }

    const double dDataMin = m_dEarliestTimestamp;
    const double dDataMax = m_dLatestTimestamp;
    const double dRange   = dDataMax - dDataMin;

    if (dRange <= 0.0)
    {
        ImGui::Dummy(size);
        return kNaN;
    }

    // Resolve strip dimensions.
    const float fW = (size.x > 0.0f) ? size.x
                                      : ImGui::GetContentRegionAvail().x;
    const float fH = size.y;

    if (fW < 1.0f || fH < 1.0f)
    {
        ImGui::Dummy(ImVec2(fW, fH));
        return kNaN;
    }

    const ImVec2        pos = ImGui::GetCursorScreenPos();
    ImDrawList*         dl  = ImGui::GetWindowDrawList();

    // --- Background ------------------------------------------------------
    dl->AddRectFilled(pos,
                      ImVec2(pos.x + fW, pos.y + fH),
                      IM_COL32(28, 28, 28, 255));

    // --- Decimated traces ------------------------------------------------
    const int nBuckets = static_cast<int>(fW);

    // Reuse per-frame scratch to avoid heap allocation on the hot path.
    // These vectors are declared in the class for the main plot; here we
    // size them locally because the strip uses a different bucket count.
    std::vector<double> bucketSum;
    std::vector<int>    bucketCnt;
    bucketSum.resize(static_cast<size_t>(nBuckets));
    bucketCnt.resize(static_cast<size_t>(nBuckets));

    for (const auto& [key, cd] : m_counters)
    {
        if (!cd.bVisible || cd.vTimestamps.empty())
            continue;

        // Resolve Y range for normalisation.
        double dYMin = std::isnan(cd.dYMinOvr) ? cd.dYMin : cd.dYMinOvr;
        double dYMax = std::isnan(cd.dYMaxOvr) ? cd.dYMax : cd.dYMaxOvr;
        if (dYMax <= dYMin) dYMax = dYMin + 1.0;

        // Zero the scratch buckets.
        std::fill(bucketSum.begin(), bucketSum.end(), 0.0);
        std::fill(bucketCnt.begin(), bucketCnt.end(), 0);

        const int N = static_cast<int>(cd.vTimestamps.size());
        for (int i = 0; i < N; ++i)
        {
            int bx = static_cast<int>((cd.vTimestamps[i] - dDataMin)
                                      / dRange * nBuckets);
            if (bx < 0)        bx = 0;
            if (bx >= nBuckets) bx = nBuckets - 1;
            bucketSum[bx] += cd.vValues[i];
            bucketCnt[bx]++;
        }

        // Build polyline from non-empty buckets.
        // Stack-allocate a small vector via reserve to stay allocation-
        // friendly; nBuckets is at most a few thousand pixels.
        std::vector<ImVec2> pts;
        pts.reserve(static_cast<size_t>(nBuckets));

        for (int bx = 0; bx < nBuckets; ++bx)
        {
            if (bucketCnt[bx] == 0) continue;
            const double avg  = bucketSum[bx] / bucketCnt[bx];
            float norm  = static_cast<float>((avg - dYMin) / (dYMax - dYMin));
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;
            pts.push_back(ImVec2(pos.x + static_cast<float>(bx),
                                 pos.y + fH * (1.0f - norm)));
        }

        if (pts.size() >= 2)
        {
            // Render at reduced alpha so the viewport rect stays prominent.
            const ImVec4& c = cd.color;
            const ImU32 col = IM_COL32(
                static_cast<int>(c.x * 210.0f),
                static_cast<int>(c.y * 210.0f),
                static_cast<int>(c.z * 210.0f),
                190);
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                            col, ImDrawFlags_None, 1.0f);
        }
    }

    // --- Viewport highlight rect ----------------------------------------
    {
        float fVpX0 = pos.x + static_cast<float>(
            (m_dXLimitMin - dDataMin) / dRange * fW);
        float fVpX1 = pos.x + static_cast<float>(
            (m_dXLimitMax - dDataMin) / dRange * fW);

        // Clamp to strip bounds.
        if (fVpX0 < pos.x)        fVpX0 = pos.x;
        if (fVpX1 > pos.x + fW)   fVpX1 = pos.x + fW;

        if (fVpX1 > fVpX0)
        {
            dl->AddRectFilled(ImVec2(fVpX0, pos.y),
                              ImVec2(fVpX1, pos.y + fH),
                              IM_COL32(255, 255, 255, 38));
            dl->AddRect(ImVec2(fVpX0, pos.y),
                        ImVec2(fVpX1, pos.y + fH),
                        IM_COL32(255, 255, 255, 170),
                        0.0f, 0, 1.5f);
        }
    }

    // --- Outer border ---------------------------------------------------
    dl->AddRect(pos,
                ImVec2(pos.x + fW, pos.y + fH),
                IM_COL32(72, 72, 72, 255));

    // --- Mouse interaction (click or drag) ------------------------------
    // Use InvisibleButton so ImGui tracks hover/active state correctly
    // even when the mouse is held and dragged outside the strip.
    ImGui::InvisibleButton("##OverviewStrip", ImVec2(fW, fH));

    double dNavTarget = kNaN;

    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            float fMx = ImGui::GetMousePos().x - pos.x;
            if (fMx < 0.0f) fMx = 0.0f;
            if (fMx > fW)   fMx = fW;
            dNavTarget = dDataMin + static_cast<double>(fMx / fW) * dRange;
        }
    }

    return dNavTarget;
}

// ---------------------------------------------------------------------------
// ExportCSV -- write visible counters in time range to a CSV file
// ---------------------------------------------------------------------------

bool WaveformRenderer::ExportCSV(double dXMin, double dXMax,
                                  const std::string& sPath) const
{
    if (sPath.empty())
        return false;

    // Collect all rows from visible counters in [dXMin, dXMax].
    // Long format: (timestamp_s, counter_name, value).
    struct Row
    {
        double      ts;
        const char* name;  // points into CounterData::sName (stable)
        double      val;
    };

    std::vector<Row> rows;
    rows.reserve(65536);

    for (const auto& [key, cd] : m_counters)
    {
        if (!cd.bVisible || cd.vTimestamps.empty())
            continue;

        // Binary-search the range.
        auto itLo = std::lower_bound(cd.vTimestamps.cbegin(),
                                     cd.vTimestamps.cend(), dXMin);
        auto itHi = std::upper_bound(cd.vTimestamps.cbegin(),
                                     cd.vTimestamps.cend(), dXMax);

        for (auto it = itLo; it != itHi; ++it)
        {
            const size_t i = static_cast<size_t>(it - cd.vTimestamps.cbegin());
            rows.push_back({cd.vTimestamps[i] + m_dSessionOrigin, cd.sName.c_str(), cd.vValues[i]});
        }
    }

    // Sort by timestamp ascending, then by counter name for ties.
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
    {
        if (a.ts != b.ts)
            return a.ts < b.ts;
        return std::strcmp(a.name, b.name) < 0;
    });

    // Write the file.
#ifdef _WIN32
    FILE* fp = nullptr;
    fopen_s(&fp, sPath.c_str(), "w");
#else
    FILE* fp = std::fopen(sPath.c_str(), "w");
#endif
    if (!fp)
        return false;

    // Header
    std::fprintf(fp, "timestamp_iso,timestamp_s,counter,value\n");

    char timeBuf[64];
    for (const Row& r : rows)
    {
        // Format ISO timestamp: YYYY-MM-DDTHH:MM:SS.mmmZ
        time_t tSec  = static_cast<time_t>(r.ts);
        double dFrac = r.ts - static_cast<double>(tSec);
        if (dFrac < 0.0) { dFrac += 1.0; --tSec; }
        int iMs = static_cast<int>(dFrac * 1000.0) % 1000;
        struct tm tmBuf;
#ifdef _WIN32
        gmtime_s(&tmBuf, &tSec);
#else
        gmtime_r(&tSec, &tmBuf);
#endif
        std::snprintf(timeBuf, sizeof(timeBuf),
                      "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                      tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec, iMs);

        // Quote the counter name in case it contains commas.
        std::fprintf(fp, "%s,%.9g,\"%s\",%.9g\n",
                     timeBuf, r.ts, r.name, r.val);
    }

    std::fclose(fp);
    return true;
}

// ---------------------------------------------------------------------------
// RenderAnnotationsInPlot -- draw annotation markers inside the plot.
//
// Each annotation is rendered as:
//   1. A dashed vertical line spanning the full plot height in the type color.
//   2. A small flag label box at the top of the line with the title.
//   3. A tooltip on hover showing full details.
//
// Must be called inside a BeginPlot / EndPlot block.
// ---------------------------------------------------------------------------

void WaveformRenderer::RenderAnnotationsInPlot()
{
    if (!m_pAnnotations)
        return;

    ImDrawList* pDL   = ImPlot::GetPlotDrawList();
    const ImVec2 pMin = ImPlot::GetPlotPos();
    const ImVec2 pMax = ImVec2(pMin.x + ImPlot::GetPlotSize().x,
                               pMin.y + ImPlot::GetPlotSize().y);

    for (const Annotation& ann : m_pAnnotations->GetAll())
    {
        // Skip soft-deleted annotations unless "show deleted" is on.
        if (ann.bDeleted && !m_bShowDeletedAnnotations)
            continue;

        // Per-type visibility check.
        const int ti = static_cast<int>(ann.eType);
        if (ti < 0 || ti >= AnnotationStore::kTypeCount)
            continue;
        if (!m_bAnnTypeVisible[ti])
            continue;

        // Skip annotations outside the visible X range.
        // ann.dTimestamp is absolute Unix epoch; convert to session-relative.
        const double dAnnRel = ann.dTimestamp - m_dSessionOrigin;
        if (dAnnRel < m_dXLimitMin || dAnnRel > m_dXLimitMax)
            continue;

        // Map annotation timestamp to screen X.
        ImVec2 screenPt = ImPlot::PlotToPixels(dAnnRel, 0.0);
        const float fX  = screenPt.x;

        // Deleted annotations are rendered with heavy transparency so they
        // are clearly distinguishable from active annotations.
        const float fBaseAlpha = ann.bDeleted ? 0.30f : 1.0f;

        // Type color (full alpha for line/border, dimmed fill and line alpha).
        const ImVec4 c4  = AnnotationStore::TypeColor(ann.eType);
        const ImU32  col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(c4.x, c4.y, c4.z, fBaseAlpha));
        const ImU32  colDim = IM_COL32(
            static_cast<ImU8>(c4.x * 255.0f * 0.6f * fBaseAlpha),
            static_cast<ImU8>(c4.y * 255.0f * 0.6f * fBaseAlpha),
            static_cast<ImU8>(c4.z * 255.0f * 0.6f * fBaseAlpha),
            static_cast<ImU8>(100.0f * fBaseAlpha));

        // Dashed vertical line: alternate filled / empty segments.
        static constexpr float kDash  = 5.0f;
        static constexpr float kGap   = 4.0f;
        for (float fy = pMin.y; fy < pMax.y; fy += kDash + kGap)
        {
            const float fyEnd = (fy + kDash < pMax.y) ? (fy + kDash) : pMax.y;
            pDL->AddLine(ImVec2(fX, fy), ImVec2(fX, fyEnd), colDim, 1.5f);
        }

        // Flag label box at the top, anchored to the vertical line.
        // Height is derived from the actual font metrics so it fits at any size.
        static constexpr float kPadX   = 4.0f;
        static constexpr float kPadY   = 3.0f;

        // Truncate the title to 20 characters to keep flags compact.
        char truncBuf[24];
        const char* pTitle = ann.sTitle.c_str();
        if (ann.sTitle.size() > 20)
        {
            std::snprintf(truncBuf, sizeof(truncBuf), "%.19s~",
                          ann.sTitle.c_str());
            pTitle = truncBuf;
        }

        const ImVec2 textSz  = ImGui::CalcTextSize(pTitle);
        const float  fFlagH  = textSz.y + kPadY * 2.0f;   // fits any font size
        const float  fFlagW  = textSz.x + kPadX * 2.0f;
        const float  fFlagT  = pMin.y + 2.0f;
        const float  fFlagB  = fFlagT + fFlagH;
        const float  fFlagL  = fX;
        const float  fFlagR  = fFlagL + fFlagW;

        // Semi-transparent fill, solid border.
        const ImU32 colFill = IM_COL32(
            static_cast<ImU8>(c4.x * 255.0f * 0.20f),
            static_cast<ImU8>(c4.y * 255.0f * 0.20f),
            static_cast<ImU8>(c4.z * 255.0f * 0.20f),
            210);

        pDL->AddRectFilled(ImVec2(fFlagL, fFlagT), ImVec2(fFlagR, fFlagB),
                           colFill, 2.0f);
        pDL->AddRect(ImVec2(fFlagL, fFlagT), ImVec2(fFlagR, fFlagB),
                     col, 2.0f, 0, 1.0f);
        pDL->AddText(ImVec2(fFlagL + kPadX, fFlagT + kPadY), col, pTitle);

        // Tooltip: shown when the mouse is anywhere on the flag/line rect.
        if (ImGui::IsMouseHoveringRect(ImVec2(fFlagL - 4.0f, fFlagT),
                                       ImVec2(fFlagR + 4.0f, pMax.y)))
        {
            ImGui::SetNextWindowBgAlpha(0.75f);
            ImGui::BeginTooltip();

            ImGui::TextColored(c4, "[%s]", AnnotationStore::TypeName(ann.eType));
            ImGui::SameLine();
            ImGui::Text("%s", ann.sTitle.c_str());

            if (!ann.sDescription.empty())
            {
                ImGui::Separator();
                ImGui::TextWrapped("%s", ann.sDescription.c_str());
            }

            ImGui::Separator();
            ImGui::TextDisabled("At: %s  |  By: %s",
                                ann.sCreatedAt.c_str(),
                                ann.sCreatedBy.c_str());

            ImGui::EndTooltip();
        }
    }
}

// ---------------------------------------------------------------------------
// Custom time-axis formatter -- HH:MM:SS.mmm
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FormatRelTime -- formats a relative time offset (in seconds) using the
// most appropriate unit so ticks on the top X2 axis stay readable at any
// zoom level.
// ---------------------------------------------------------------------------
int WaveformRenderer::FormatRelTime(double dValue, char* pBuf, int iSize,
                                    void* /*pUserData*/)
{
    if (iSize <= 0) return 0;
    const double dAbs = dValue < 0.0 ? -dValue : dValue;
    const char*  sign = (dValue < -1e-15) ? "-" : "";
    int n;
    if      (dAbs < 1e-6)    n = std::snprintf(pBuf, (size_t)iSize, "%s%.0fns", sign, dAbs * 1e9);
    else if (dAbs < 1e-3)    n = std::snprintf(pBuf, (size_t)iSize, "%s%.1fus", sign, dAbs * 1e6);
    else if (dAbs < 1.0)     n = std::snprintf(pBuf, (size_t)iSize, "%s%.1fms", sign, dAbs * 1e3);
    else if (dAbs < 60.0)    n = std::snprintf(pBuf, (size_t)iSize, "%s%.2fs",  sign, dAbs);
    else if (dAbs < 3600.0)  n = std::snprintf(pBuf, (size_t)iSize, "%s%.1fm",  sign, dAbs / 60.0);
    else if (dAbs < 86400.0) n = std::snprintf(pBuf, (size_t)iSize, "%s%.2fh",  sign, dAbs / 3600.0);
    else                     n = std::snprintf(pBuf, (size_t)iSize, "%s%.2fd",  sign, dAbs / 86400.0);
    return (n > 0 && n < iSize) ? n : 0;
}

int WaveformRenderer::FormatTime(double dValue, char* pBuf, int iSize,
                                 void* pUserData)
{
    if (iSize <= 0)
        return 0;

    // dValue is session-relative seconds.  pUserData points to the
    // session origin (Unix epoch seconds) so we can recover absolute time.
    const double dOrigin = pUserData ? *static_cast<const double*>(pUserData) : 0.0;
    dValue += dOrigin;

    // dValue is now Unix epoch seconds (double).
    time_t tSec = static_cast<time_t>(dValue);
    double dFrac = dValue - static_cast<double>(tSec);
    if (dFrac < 0.0) { dFrac += 1.0; --tSec; }
    int iMs = static_cast<int>(dFrac * 1000.0) % 1000;

    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &tSec);
#else
    localtime_r(&tSec, &tmBuf);
#endif

    int n = std::snprintf(pBuf, static_cast<size_t>(iSize),
                          "%02d:%02d:%02d.%03d",
                          tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec, iMs);
    return (n > 0 && n < iSize) ? n : 0;
}

// ---------------------------------------------------------------------------
// Per-counter VizMode API
// ---------------------------------------------------------------------------

void WaveformRenderer::SetCounterVizMode(uint32_t dwHash, uint16_t wID, VizMode m)
{
    auto it = m_counters.find(MakeKey(dwHash, wID));
    if (it != m_counters.end())
        it->second.eVizMode = m;
}

VizMode WaveformRenderer::GetCounterVizMode(uint32_t dwHash, uint16_t wID) const
{
    auto it = m_counters.find(MakeKey(dwHash, wID));
    return (it != m_counters.end()) ? it->second.eVizMode : m_eVizMode;
}

void WaveformRenderer::SetAllCountersVizMode(VizMode m)
{
    m_eVizMode = m;
    for (auto& [key, cd] : m_counters)
        cd.eVizMode = m;
}

// ---------------------------------------------------------------------------
// Decimate -- avg per pixel column
// ---------------------------------------------------------------------------

void WaveformRenderer::Decimate(const double* pTS, const double* pVS,
                                int N, int W)
{
    m_scratchX.clear();
    m_scratchY.clear();

    if (N <= 0 || W <= 0)
        return;

    const double xMin = pTS[0];
    const double xMax = pTS[N - 1];
    const double span = xMax - xMin;

    if (span <= 0.0)
    {
        // All samples at the same timestamp: one point.
        m_scratchX.push_back(xMin);
        m_scratchY.push_back(pVS[0]);
        return;
    }

    const double dx = span / static_cast<double>(W);

    int j = 0;
    for (int col = 0; col < W; ++col)
    {
        const double xL = xMin + col * dx;
        const double xR = xL + dx;
        double sum = 0.0;
        int    cnt = 0;

        while (j < N && pTS[j] < xR)
        {
            if (pTS[j] >= xL)
            {
                sum += pVS[j];
                ++cnt;
            }
            ++j;
        }

        if (cnt > 0)
        {
            m_scratchX.push_back(xL + dx * 0.5);
            m_scratchY.push_back(sum / cnt);
        }
    }
}

// ---------------------------------------------------------------------------
// DecimateRange -- min/avg/max per pixel column
// ---------------------------------------------------------------------------

void WaveformRenderer::DecimateRange(const double* pTS, const double* pVS,
                                     int N, int W)
{
    m_scratchX.clear();
    m_scratchY.clear();
    m_scratchYLo.clear();
    m_scratchYHi.clear();

    if (N <= 0 || W <= 0)
        return;

    const double xMin = pTS[0];
    const double xMax = pTS[N - 1];
    const double span = xMax - xMin;

    if (span <= 0.0)
    {
        m_scratchX.push_back(xMin);
        m_scratchY.push_back(pVS[0]);
        m_scratchYLo.push_back(pVS[0]);
        m_scratchYHi.push_back(pVS[0]);
        return;
    }

    const double dx = span / static_cast<double>(W);

    int j = 0;
    for (int col = 0; col < W; ++col)
    {
        const double xL = xMin + col * dx;
        const double xR = xL + dx;
        double sum  = 0.0;
        double vLo  =  std::numeric_limits<double>::max();
        double vHi  = -std::numeric_limits<double>::max();
        int    cnt  = 0;

        while (j < N && pTS[j] < xR)
        {
            if (pTS[j] >= xL)
            {
                sum += pVS[j];
                if (pVS[j] < vLo) vLo = pVS[j];
                if (pVS[j] > vHi) vHi = pVS[j];
                ++cnt;
            }
            ++j;
        }

        if (cnt > 0)
        {
            m_scratchX.push_back(xL + dx * 0.5);
            m_scratchY.push_back(sum / cnt);
            m_scratchYLo.push_back(vLo);
            m_scratchYHi.push_back(vHi);
        }
    }
}

// ---------------------------------------------------------------------------
// PlotCounterInMode -- dispatch per-counter plot call based on VizMode
//
// Each sample is represented as a dot:
//   sparse (N <= W):  hollow circle/square -- one data point per dot.
//   dense  (N  > W):  solid  square       -- multiple data points per dot.
// Range / Cyclogram mode always draws a min/max fill when dense.
// Cyclogram uses PlotStairs on the midpoint line (staircase connection).
// ---------------------------------------------------------------------------

void WaveformRenderer::PlotCounterInMode(const CounterData& cd,
                                         const ImPlotSpec& baseSpec)
{
    const size_t   nTotal = cd.vTimestamps.size();
    if (nTotal == 0)
        return;

    const double* pTS = cd.vTimestamps.data();
    const double* pVS = cd.vValues.data();

    // Visible index range [iBegin, iEnd).
    size_t iBegin = static_cast<size_t>(
        std::lower_bound(pTS, pTS + nTotal, m_dXLimitMin) - pTS);
    size_t iEnd = static_cast<size_t>(
        std::upper_bound(pTS, pTS + nTotal, m_dXLimitMax) - pTS);

    // Extend one sample on each side so partial lines at the edges render.
    if (iBegin > 0)       --iBegin;
    if (iEnd   < nTotal)  ++iEnd;

    const int N = static_cast<int>(iEnd - iBegin);
    if (N <= 0)
        return;

    // Pixel budget: number of plot columns available this frame.
    const int W = std::max(1, static_cast<int>(m_plotSize.x));

    const double* pTSv = pTS + iBegin;
    const double* pVSv = pVS + iBegin;

    // Dense = more samples than horizontal pixels; activate decimation.
    const bool bDense = (N > W);

    // Dot geometry: hollow circle for sparse, solid square for dense.
    // Squares are faster to rasterize; size chosen to be visible but subtle.
    static constexpr float kDotSparse = 2.5f;  // hollow circle radius (px)
    static constexpr float kDotDense  = 2.0f;  // solid square radius  (px)

    // Build a dot-only spec (no connecting line).
    // MarkerFillColor transparent => hollow; opaque => solid (dense).
    auto MakeDotSpec = [&](bool bSolid) -> ImPlotSpec
    {
        ImPlotSpec s = baseSpec;
        s.Flags          |= ImPlotItemFlags_NoLegend;
        s.LineWeight      = 1.0f;   // marker edge weight
        s.Marker          = bSolid ? ImPlotMarker_Square : ImPlotMarker_Circle;
        s.MarkerSize      = bSolid ? kDotDense : kDotSparse;
        s.MarkerFillColor = bSolid
            ? cd.color
            : ImVec4(cd.color.x, cd.color.y, cd.color.z, 0.0f); // hollow
        s.MarkerLineColor = cd.color;
        return s;
    };

    // ---- Range and Cyclogram -------------------------------------------
    if (cd.eVizMode == VizMode::Range || cd.eVizMode == VizMode::Cyclogram)
    {
        if (bDense)
        {
            // Decimate to W columns: get min, avg, max per column.
            DecimateRange(pTSv, pVSv, N, W);
            const int M = static_cast<int>(m_scratchX.size());
            if (M == 0)
                return;

            // Min/max fill: subdued fill + thin border in trace color.
            // This is the primary legend entry for the counter.
            ImPlotSpec specFill = baseSpec;
            specFill.FillColor  = ImVec4(cd.color.x, cd.color.y, cd.color.z, 0.20f);
            specFill.LineColor  = ImVec4(cd.color.x, cd.color.y, cd.color.z, 0.35f);
            specFill.LineWeight = 0.5f;
            ImPlot::PlotShaded(cd.sName.c_str(),
                               m_scratchX.data(),
                               m_scratchYLo.data(),
                               m_scratchYHi.data(),
                               M, specFill);

            // Midpoint line / staircase on top of the fill.
            ImPlotSpec specMid = baseSpec;
            specMid.Flags |= ImPlotItemFlags_NoLegend;
            if (cd.eVizMode == VizMode::Cyclogram)
                ImPlot::PlotStairs(cd.sName.c_str(),
                                   m_scratchX.data(), m_scratchY.data(),
                                   M, specMid);
            else
                ImPlot::PlotLine(cd.sName.c_str(),
                                 m_scratchX.data(), m_scratchY.data(),
                                 M, specMid);
            // No dot markers when a range bar is present at the same X column:
            // the bar already encodes the data spread and a dot would be redundant
            // / misleading (it would show only the avg midpoint, not min/max).
        }
        else
        {
            // Sparse: draw raw line or stairs + hollow circle per sample.
            ImPlotSpec specLine = baseSpec;
            if (cd.eVizMode == VizMode::Cyclogram)
                ImPlot::PlotStairs(cd.sName.c_str(), pTSv, pVSv, N, specLine);
            else
                ImPlot::PlotLine(cd.sName.c_str(), pTSv, pVSv, N, specLine);

            ImPlot::PlotScatter(cd.sName.c_str(), pTSv, pVSv, N,
                                MakeDotSpec(false));
        }
    }
    // ---- Simple mode ---------------------------------------------------
    else
    {
        if (bDense)
        {
            Decimate(pTSv, pVSv, N, W);
            const int M = static_cast<int>(m_scratchX.size());
            if (M == 0)
                return;

            ImPlot::PlotLine(cd.sName.c_str(),
                             m_scratchX.data(), m_scratchY.data(),
                             M, baseSpec);

            // Solid square markers at decimated positions.
            ImPlot::PlotScatter(cd.sName.c_str(),
                                m_scratchX.data(), m_scratchY.data(),
                                M, MakeDotSpec(true));
        }
        else
        {
            // Sparse: raw line + hollow circle per sample.
            ImPlot::PlotLine(cd.sName.c_str(), pTSv, pVSv, N, baseSpec);
            ImPlot::PlotScatter(cd.sName.c_str(), pTSv, pVSv, N,
                                MakeDotSpec(false));
        }
    }
}

// ---------------------------------------------------------------------------
// Render -- multi-panel stacked ImPlot drawing
// ---------------------------------------------------------------------------
//
// Each visible counter gets its own panel (max kMaxPanels = 8).
// When more counters are visible than panels, they are binned into panels
// by Y-range overlap (see BuildPanelAssignment).  All panels share a
// synchronised X axis via ImPlotSubplotFlags_LinkAllX.
//
// Scroll-wheel behaviour:
//   Plain scroll       -> zoom X around cursor, Y locked.
//   Shift+scroll       -> zoom Y in the hovered panel only, X frozen.
//
// Programmatic navigation (NavigateTo) and auto-scroll set limits only in
// panel 0; ImPlot's LinkAllX propagates them to all other panels within
// the same frame.
// ---------------------------------------------------------------------------

bool WaveformRenderer::Render(const ImVec2& size, bool bAutoScroll,
                              double dTimeWindow)
{
    bool bUserInteracted = false;

    ImGuiIO& io = ImGui::GetIO();
    const bool bShiftHeld = io.KeyShift;

    // ---- 1. Assign visible counters to panels. --------------------------
    // Save the panel count from the previous frame so we can detect a
    // structural change (visibility toggle) and re-apply the X limits.
    const int nPrevActivePanels = m_nActivePanels;
    BuildPanelAssignment();
    if (m_nActivePanels == 0)
        return false;

    // ---- 2. Consume scroll wheel before ImPlot sees it. ----------------
    float fWheel = 0.0f;
    if (m_bAnyPlotHoveredLastFrame && io.MouseWheel != 0.0f)
    {
        fWheel        = io.MouseWheel;
        io.MouseWheel = 0.0f;
    }

    // zoom factor: < 1 = zoom-in (scroll-up), > 1 = zoom-out (scroll-down).
    const double kZoomFactor = (fWheel > 0.0f) ? 0.8 : 1.25;

    // ---- 3. Compute forced X range for this frame. ---------------------
    // Applies when the user scrolled the X axis or NavigateTo was called.
    bool   bForceX     = false;
    double dForcedXMin = 0.0;
    double dForcedXMax = 1.0;

    if (fWheel != 0.0f && !bShiftHeld)
    {
        if (bAutoScroll)
        {
            // Zoom while live: shrink/grow the live window span.
            // The view continues tracking the live edge -- no pan, no bUserInteracted.
            if (m_dLiveSpan <= 0.0)
                m_dLiveSpan = (dTimeWindow > 0.0) ? dTimeWindow : kMaxLiveSpan;
            m_dLiveSpan *= kZoomFactor;
            // Clamp: must not exceed 15 s; must stay wide enough to render.
            if (m_dLiveSpan > kMaxLiveSpan) m_dLiveSpan = kMaxLiveSpan;
            if (m_dLiveSpan < 0.02)         m_dLiveSpan = 0.02;
        }
        else
        {
            // Plain scroll (non-live): zoom X around cursor position.
            //
            // Bug fix: deep zoom-in can push m_dXLimitMax − m_dXLimitMin to zero
            // because Unix-epoch timestamps (~1.7 × 10⁹ s) exhaust double precision
            // at roughly 380 ns resolution.  Clamping the effective range to
            // kMinXSpan ensures zoom-out can always escape that collapsed state.
            double dXRange = m_dXLimitMax - m_dXLimitMin;
            if (dXRange < kMinXSpan) dXRange = kMinXSpan;

            float fNormX = 0.5f;
            const int hp = m_nHoveredPanelLastFrame;
            if (hp >= 0 && hp < m_nActivePanels && m_aPanelSize[hp].x > 0.0f)
                fNormX = std::max(0.0f, std::min(1.0f,
                         (io.MousePos.x - m_aPanelPos[hp].x) / m_aPanelSize[hp].x));

            const double dCursorX = m_dXLimitMin + (double)fNormX * dXRange;
            dForcedXMin = dCursorX - kZoomFactor * (double)fNormX        * dXRange;
            dForcedXMax = dCursorX + kZoomFactor * (1.0 - (double)fNormX) * dXRange;

            // Clamp zoom-in: never allow the visible span to drop below kMinXSpan.
            if (dForcedXMax - dForcedXMin < kMinXSpan)
            {
                const double dCenter = (dForcedXMin + dForcedXMax) * 0.5;
                dForcedXMin = dCenter - kMinXSpan * 0.5;
                dForcedXMax = dCenter + kMinXSpan * 0.5;
            }

            bForceX = true;
            bUserInteracted = true;
        }
    }
    else if (m_bNavPending)
    {
        m_bNavPending = false;
        double dHalfWin = (m_dXLimitMax - m_dXLimitMin) * 0.5;
        if (dHalfWin <= 0.0) dHalfWin = 7.5;
        dForcedXMin = m_dNavTarget - dHalfWin;
        dForcedXMax = m_dNavTarget + dHalfWin;
        bForceX = true;
    }

    // When forcing X we also freeze it for non-forced frames so Y-only
    // scroll does not accidentally let X drift.
    if (fWheel != 0.0f && bShiftHeld && !bForceX && m_dXLimitMax > m_dXLimitMin)
    {
        bForceX     = true;
        dForcedXMin = m_dXLimitMin;
        dForcedXMax = m_dXLimitMax;
    }

    // When the panel structure changes (counter visibility toggled), ImPlot
    // may reset the linked X axis because BeginSubplots receives a different
    // row count.  Re-apply the last known X limits so the view is preserved.
    if (!bForceX && !bAutoScroll
        && m_nActivePanels != nPrevActivePanels
        && m_bInitialViewApplied
        && m_dXLimitMax > m_dXLimitMin)
    {
        bForceX     = true;
        dForcedXMin = m_dXLimitMin;
        dForcedXMax = m_dXLimitMax;
    }

    // ---- 4. Compute forced Y for the hovered panel (Shift+scroll). -----
    const int  hp          = m_nHoveredPanelLastFrame;
    const bool bForceYThis = (fWheel != 0.0f && bShiftHeld
                              && hp >= 0 && hp < m_nActivePanels);
    if (bForceYThis)
    {
        PanelState& ps = m_panels[hp];
        if (ps.bYRangeSet)
        {
            const double dRange = ps.dYTop - ps.dYBase;
            if (dRange > 0.0)
            {
                double dNewRange = dRange * kZoomFactor;
                if (ps.dYOrigRange > 0.0 && dNewRange < ps.dYOrigRange * 0.01)
                    dNewRange = ps.dYOrigRange * 0.01;
                ps.dYTop = ps.dYBase + dNewRange;
                // Persist Y top in the primary counter.
                if (ps.nKeys > 0)
                {
                    auto itPrim = m_counters.find(ps.aKeys[0]);
                    if (itPrim != m_counters.end())
                        itPrim->second.dYUserTop = ps.dYTop;
                }
            }
        }
    }

    // ---- 5. Reset per-frame flags. -------------------------------------
    m_bAnyPlotHoveredLastFrame = false;
    m_bPlotClicked             = false;
    m_bAnnotationReq           = false;
    bool bXCaptured            = false;

    // Save previous crossbar state for drawing hairlines in non-hovered panels.
    const bool  bCrossbarPrev = m_bCrossbarActive;
    const float fCrossbarXPrev = m_fCrossbarScreenX;
    m_bCrossbarActive = false;     // will be re-set if a panel is hovered

    // ---- 6. Crossbar accumulation. -------------------------------------
    const bool bCrossBarOn = m_pConfig && m_pConfig->Get().bCrossBar;

    struct CrossInfo
    {
        ImVec4      color;
        const char* pName;
        double      dVal;
        double      dLo;
        double      dHi;
        int         nInCol;
        VizMode     eMode;
    };
    static constexpr int kMaxHits = 32;
    CrossInfo crossHits[kMaxHits];
    int       nCrossHits = 0;
    double    dCrossTime = 0.0;

    // ---- 7. Begin subplots (N rows, 1 col, link all X axes). -----------
    const ImPlotSubplotFlags kSubFlags =
        ImPlotSubplotFlags_NoTitle  |
        ImPlotSubplotFlags_NoMenus  |
        ImPlotSubplotFlags_NoResize |
        ImPlotSubplotFlags_LinkAllX;

    // Equalize inner plot canvas heights across all panels.
    // Non-bottom panels have NoTickLabels on X, so their bottom padding is
    // minimal.  The bottom panel carries tick labels + "Time" axis label,
    // consuming roughly 2 * (font_size + LabelPadding) extra pixels.
    // Boosting the bottom row's ratio by that amount makes all inner canvas
    // heights identical.
    float        aRowRatios[8] = {};   // matches m_panels[8] limit
    float*       pRowRatios    = nullptr;
    if (m_nActivePanels > 1 && size.y > 0.0f)
    {
        const float fFontH = ImGui::GetFontSize();
        const float fPad   = ImPlot::GetStyle().LabelPadding.y;
        const float fExtra = 2.0f * (fFontH + fPad);   // tick labels + axis label
        const float fBoost = fExtra * (float)m_nActivePanels / size.y;
        for (int i = 0; i < m_nActivePanels; ++i)
            aRowRatios[i] = 1.0f;
        aRowRatios[m_nActivePanels - 1] += fBoost;  // bottom: X1 tick labels
        aRowRatios[0]                   += fBoost;  // top: X2 relative-time labels
        pRowRatios = aRowRatios;
    }

    // Reduce the vertical gap between panels from the default (10 px) to 3 px
    // so each panel gets the maximum canvas height available.
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding,
                         ImVec2(10.0f, ImGui::GetFontSize() * 0.5f));
    if (ImPlot::BeginSubplots("##WF", m_nActivePanels, 1, size, kSubFlags, pRowRatios))
    {
        // Capture the current plot background once for contrast checks below.
        // ImPlotCol_PlotBg is set by the theme; reading it here is free.
        const ImVec4 plotBg = ImPlot::GetStyle().Colors[ImPlotCol_PlotBg];

        for (int p = 0; p < m_nActivePanels; ++p)
        {
            const bool  bIsBottom = (p == m_nActivePanels - 1);
            PanelState& ps        = m_panels[p];

            // ---- 8a. BeginPlot for this panel. -------------------------
            // Pass (-1,-1) so BeginSubplots controls the actual size.
            if (!ImPlot::BeginPlot("", ImVec2(-1.0f, -1.0f),
                                   ImPlotFlags_NoTitle | ImPlotFlags_NoMenus |
                                   ImPlotFlags_NoLegend))
                continue;

            // ---- 8b. Y-axis setup. -------------------------------------
            // Choose label: single-counter panel shows the counter name;
            // Counter names are drawn as small overlay text (top-left of the
            // plot canvas) rather than as a vertical Y-axis label.

            if (ps.bYRangeSet)
            {
                const double dInitMargin = ps.dYOrigRange * 0.05;
                ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_Lock);

                if (bForceYThis && p == hp)
                    ImPlot::SetupAxisLimits(ImAxis_Y1,
                                            ps.dYBase, ps.dYTop,
                                            ImPlotCond_Always);
                else
                {
                    // Use Always when the primary counter changed this frame
                    // so ImPlot replaces its stale cached limits immediately.
                    const ImPlotCond cond = ps.bForceReset
                                           ? ImPlotCond_Always
                                           : ImPlotCond_Once;
                    ps.bForceReset = false;
                    ImPlot::SetupAxisLimits(ImAxis_Y1,
                                            ps.dYBase, ps.dYTop + dInitMargin,
                                            cond);
                }

                // Cap tick count so adjacent-panel Y labels don't overlap
                // when many panels are stacked in a compact window.
                const double dAxisHi = (bForceYThis && p == hp)
                                       ? ps.dYTop
                                       : ps.dYTop + dInitMargin;
                ImPlot::SetupAxisTicks(ImAxis_Y1, ps.dYBase, dAxisHi, 3);
            }
            else
            {
                ImPlot::SetupAxis(ImAxis_Y1, nullptr,
                                  ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Lock);
            }

            // ---- 8c. X-axis setup. -------------------------------------
            // Tick labels only on the bottom panel; all others are blank
            // so the shared X label is not repeated.
            {
                ImPlotAxisFlags xFlags = 0;
                if (!bIsBottom)
                    xFlags |= ImPlotAxisFlags_NoTickLabels;
                ImPlot::SetupAxis(ImAxis_X1,
                                  bIsBottom ? "Time" : nullptr,
                                  xFlags);
                ImPlot::SetupAxisFormat(ImAxis_X1,
                                        &WaveformRenderer::FormatTime,
                                        static_cast<void*>(&m_dSessionOrigin));
            }

            // ---- 8c2. X2 axis (relative time, top of first panel only). ---
            // X2 rendered with ImPlotAxisFlags_Opposite sits at the top edge.
            // Its range is pinned to [0, view_span] every frame so each tick
            // shows the time offset from the visible window's left edge.
            // m_dXLimitMin/Max hold the previous frame's actual X1 limits
            // (one-frame lag, imperceptible) and are updated in step 8m.
            if (p == 0)
            {
                const double dViewSpan = m_dXLimitMax - m_dXLimitMin;
                if (dViewSpan > 0.0)
                {
                    ImPlot::SetupAxis(ImAxis_X2, nullptr,
                                      ImPlotAxisFlags_Opposite   |
                                      ImPlotAxisFlags_NoGridLines |
                                      ImPlotAxisFlags_NoMenus     |
                                      ImPlotAxisFlags_NoSideSwitch);
                    ImPlot::SetupAxisFormat(ImAxis_X2,
                                            &WaveformRenderer::FormatRelTime,
                                            nullptr);
                    ImPlot::SetupAxisLimits(ImAxis_X2, 0.0, dViewSpan,
                                            ImPlotCond_Always);
                }
            }

            // ---- 8d. Forced X / auto-scroll / initial view. ------------
            // Applied in panel 0 only; LinkAllX propagates to panels 1..N-1
            // within the same frame.
            if (p == 0)
            {
                if (bForceX)
                {
                    ImPlot::SetupAxisLimits(ImAxis_X1,
                                            dForcedXMin, dForcedXMax,
                                            ImPlotCond_Always);
                }
                else if (bAutoScroll && m_bHasAnyData)
                {
                    // Initialise live span from config on first live frame.
                    if (m_dLiveSpan <= 0.0)
                        m_dLiveSpan = (dTimeWindow > 0.0) ? dTimeWindow : kMaxLiveSpan;

                    // Use wall-clock time as the live right edge so the
                    // view advances smoothly every frame, not only when
                    // new Kafka samples arrive.  Convert to session-relative.
                    const double dNowAbs = std::chrono::duration<double>(
                        std::chrono::system_clock::now()
                            .time_since_epoch()).count();
                    const double dNow    = dNowAbs - m_dSessionOrigin;
                    const double dXMax = (dNow > m_dLatestTimestamp)
                                         ? dNow : m_dLatestTimestamp;
                    const double dXMin = dXMax - m_dLiveSpan;
                    ImPlot::SetupAxisLimits(ImAxis_X1, dXMin, dXMax,
                                            ImPlotCond_Always);
                    m_bInitialViewApplied = true;
                }
                else if (!m_bInitialViewApplied && m_bHasAnyData)
                {
                    const double dXMin = m_dEarliestTimestamp;
                    const double dActual = (m_dLatestTimestamp > dXMin)
                                         ? (m_dLatestTimestamp - dXMin)
                                         : dTimeWindow;
                    const double dWin = (dActual < dTimeWindow) ? dActual : dTimeWindow;
                    ImPlot::SetupAxisLimits(ImAxis_X1, dXMin, dXMin + dWin,
                                            ImPlotCond_Always);
                    m_bInitialViewApplied = true;
                }
            }

            // ---- 8f. Capture panel rect (after all Setup calls). ----------
            // GetPlotPos/GetPlotSize are setup-locking; must be called after
            // all SetupAxis / SetupAxisLimits calls are done.
            m_plotPos  = ImPlot::GetPlotPos();
            m_plotSize = ImPlot::GetPlotSize();
            m_aPanelPos[p]  = m_plotPos;
            m_aPanelSize[p] = m_plotSize;

            // ---- 8f. Background tint for the highlighted counter. ------
            if (m_nHighlightedKey != 0)
            {
                for (int ki = 0; ki < ps.nKeys; ++ki)
                {
                    if (ps.aKeys[ki] != m_nHighlightedKey)
                        continue;
                    auto itHL = m_counters.find(m_nHighlightedKey);
                    if (itHL != m_counters.end() && itHL->second.bVisible)
                    {
                        ImVec4 c4   = itHL->second.color;
                        c4.w        = 0.10f;
                        const ImVec2 pMin = ImPlot::GetPlotPos();
                        const ImVec2 pMax = ImVec2(pMin.x + ImPlot::GetPlotSize().x,
                                                   pMin.y + ImPlot::GetPlotSize().y);
                        ImPlot::GetPlotDrawList()->AddRectFilled(
                            pMin, pMax, ImGui::ColorConvertFloat4ToU32(c4));
                    }
                    break;
                }
            }

            // ---- 8g. Counter name labels (top-left corner overlay). -----
            // Small horizontal text in the counter's graph color, stacked
            // downward when the panel contains multiple counters.
            {
                ImDrawList* pLabelDL = ImPlot::GetPlotDrawList();
                const float fSmall   = ImGui::GetFontSize() * 0.82f;
                const float fLineH   = fSmall + 2.0f;
                float       fLY      = m_plotPos.y + 3.0f;
                const float fLX      = m_plotPos.x + 4.0f;

                for (int ki = 0; ki < ps.nKeys; ++ki)
                {
                    auto itL = m_counters.find(ps.aKeys[ki]);
                    if (itL == m_counters.end()) continue;
                    const CounterData& cdL = itL->second;
                    if (!cdL.bVisible) continue;
                    const ImVec4 rawLabel(cdL.color.x, cdL.color.y,
                                         cdL.color.z, 0.85f);
                    const ImU32 cLabel = ImGui::ColorConvertFloat4ToU32(
                        EnsureContrast(rawLabel, plotBg));
                    pLabelDL->AddText(ImGui::GetFont(), fSmall,
                                      ImVec2(fLX, fLY), cLabel,
                                      cdL.sName.c_str());
                    fLY += fLineH;
                }
            }

            // ---- 8h. Plot counters (2 passes for highlight z-order). ---
            for (int pass = 0; pass < 2; ++pass)
            {
                for (int ki = 0; ki < ps.nKeys; ++ki)
                {
                    auto it = m_counters.find(ps.aKeys[ki]);
                    if (it == m_counters.end())
                        continue;
                    const CounterData& cd = it->second;
                    if (!cd.bVisible || cd.vTimestamps.empty())
                        continue;
                    if (cd.bHighlighted != (pass == 1))
                        continue;

                    ImPlotSpec spec;
                    spec.LineColor  = EnsureContrast(cd.color, plotBg);
                    spec.LineWeight = cd.bHighlighted ? 2.5f : 1.0f;
                    PlotCounterInMode(cd, spec);
                }
            }

            // ---- 8h. Annotation markers in every panel. ----------------
            if (m_pAnnotations && m_bAnnotationsVisible)
                RenderAnnotationsInPlot();

            // ---- 8i. Ctrl+click: request new annotation. ---------------
            if (ImPlot::IsPlotHovered() && io.KeyCtrl &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                ImPlotPoint mp       = ImPlot::GetPlotMousePos();
                m_bAnnotationReq     = true;
                m_dAnnotationReqTime = mp.x;
            }

            // ---- 8j. Hit-test: detect plain left-click on a trace. -----
            if (ImPlot::IsPlotHovered() && !io.KeyCtrl &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                ImPlotPoint mp       = ImPlot::GetPlotMousePos();
                uint64_t    bestKey  = 0;
                float       bestDSq  = 16.0f * 16.0f;

                for (int ki = 0; ki < ps.nKeys; ++ki)
                {
                    auto it = m_counters.find(ps.aKeys[ki]);
                    if (it == m_counters.end()) continue;
                    const CounterData& cd = it->second;
                    if (!cd.bVisible || cd.vTimestamps.empty()) continue;

                    auto tsIt = std::lower_bound(cd.vTimestamps.cbegin(),
                                                 cd.vTimestamps.cend(), mp.x);
                    for (int off = -1; off <= 0; ++off)
                    {
                        auto jt = tsIt;
                        if (off < 0)
                        {
                            if (jt == cd.vTimestamps.cbegin()) continue;
                            --jt;
                        }
                        if (jt == cd.vTimestamps.cend()) continue;

                        const ptrdiff_t idx = jt - cd.vTimestamps.cbegin();
                        ImVec2 screenPt = ImPlot::PlotToPixels(
                            cd.vTimestamps[static_cast<size_t>(idx)],
                            cd.vValues[static_cast<size_t>(idx)]);
                        const float dx  = io.MousePos.x - screenPt.x;
                        const float dy  = io.MousePos.y - screenPt.y;
                        const float dSq = dx*dx + dy*dy;
                        if (dSq < bestDSq)
                        {
                            bestDSq = dSq;
                            bestKey = ps.aKeys[ki];
                        }
                    }
                }
                m_bPlotClicked = true;
                m_nClickedKey  = bestKey;
            }

            // ---- 8k. Crossbar. -----------------------------------------
            if (bCrossBarOn)
            {
                const ImVec2 pMin2 = ImPlot::GetPlotPos();
                const ImVec2 pMax2 = ImVec2(pMin2.x + ImPlot::GetPlotSize().x,
                                            pMin2.y + ImPlot::GetPlotSize().y);
                ImDrawList* pDL = ImPlot::GetPlotDrawList();

                if (ImPlot::IsPlotHovered())
                {
                    // ---- Hovered panel: full crossbar. -----------------
                    ImPlotPoint mp = ImPlot::GetPlotMousePos();
                    dCrossTime         = mp.x;
                    m_fCrossbarScreenX = io.MousePos.x;
                    m_bCrossbarActive  = true;

                    // Vertical hairline.
                    pDL->AddLine(ImVec2(m_fCrossbarScreenX, pMin2.y),
                                 ImVec2(m_fCrossbarScreenX, pMax2.y),
                                 IM_COL32(180, 180, 180, 70), 1.0f);

                    // Sample half-width in data coords for column gather.
                    const double dHalfPx = (m_plotSize.x > 0.0f)
                        ? 0.5 * (m_dXLimitMax - m_dXLimitMin) / m_plotSize.x
                        : 0.0;

                    // Collect per-counter value at cursor X and draw markers.
                    for (int ki = 0; ki < ps.nKeys; ++ki)
                    {
                        if (nCrossHits >= kMaxHits) break;
                        auto it = m_counters.find(ps.aKeys[ki]);
                        if (it == m_counters.end()) continue;
                        const CounterData& cd = it->second;
                        if (!cd.bVisible || cd.vTimestamps.empty()) continue;

                        CrossInfo& h = crossHits[nCrossHits];
                        h.color = cd.color;
                        h.pName = cd.sName.c_str();
                        h.eMode = cd.eVizMode;

                        // Gather samples in the cursor pixel column.
                        const double dColL = mp.x - dHalfPx;
                        const double dColR = mp.x + dHalfPx;
                        auto itLo = std::lower_bound(cd.vTimestamps.cbegin(),
                                                     cd.vTimestamps.cend(), dColL);
                        auto itHi = std::upper_bound(cd.vTimestamps.cbegin(),
                                                     cd.vTimestamps.cend(), dColR);
                        const int nCol = static_cast<int>(itHi - itLo);

                        if (nCol > 0)
                        {
                            double vLo  =  std::numeric_limits<double>::max();
                            double vHi  = -std::numeric_limits<double>::max();
                            double vSum = 0.0;
                            for (auto it2 = itLo; it2 != itHi; ++it2)
                            {
                                const double v = cd.vValues[static_cast<size_t>(
                                    it2 - cd.vTimestamps.cbegin())];
                                if (v < vLo) vLo = v;
                                if (v > vHi) vHi = v;
                                vSum += v;
                            }
                            h.dVal   = vSum / nCol;
                            h.dLo    = vLo;
                            h.dHi    = vHi;
                            h.nInCol = nCol;
                        }
                        else
                        {
                            // Nearest-neighbour fallback.
                            auto tsIt = std::lower_bound(cd.vTimestamps.cbegin(),
                                                         cd.vTimestamps.cend(), mp.x);
                            double dNearVal = 0.0;
                            bool   bFound   = false;
                            double dBestD   = std::numeric_limits<double>::max();
                            for (int off = -1; off <= 0; ++off)
                            {
                                auto jt = tsIt;
                                if (off < 0)
                                {
                                    if (jt == cd.vTimestamps.cbegin()) continue;
                                    --jt;
                                }
                                if (jt == cd.vTimestamps.cend()) continue;
                                const size_t idx = static_cast<size_t>(
                                    jt - cd.vTimestamps.cbegin());
                                const double d = std::fabs(cd.vTimestamps[idx] - mp.x);
                                if (d < dBestD)
                                {
                                    dBestD   = d;
                                    dNearVal = cd.vValues[idx];
                                    bFound   = true;
                                }
                            }
                            if (!bFound) continue;
                            h.dVal   = dNearVal;
                            h.dLo    = dNearVal;
                            h.dHi    = dNearVal;
                            h.nInCol = 1;
                        }
                        ++nCrossHits;

                        // Draw dot or range bracket at the value.
                        const ImU32 cH = ImGui::ColorConvertFloat4ToU32(
                            ImVec4(h.color.x, h.color.y, h.color.z, 0.90f));
                        const bool bRangeMarker =
                            (h.nInCol > 1 && h.dHi > h.dLo &&
                             (h.eMode == VizMode::Range ||
                              h.eMode == VizMode::Cyclogram));
                        const float fMX = m_fCrossbarScreenX;

                        if (bRangeMarker)
                        {
                            const ImVec2 ptLo = ImPlot::PlotToPixels(mp.x, h.dLo);
                            const ImVec2 ptHi = ImPlot::PlotToPixels(mp.x, h.dHi);
                            const float  fTW  = 4.5f;
                            pDL->AddLine(ImVec2(fMX, ptLo.y), ImVec2(fMX, ptHi.y), cH, 2.5f);
                            pDL->AddLine(ImVec2(fMX-fTW, ptHi.y), ImVec2(fMX+fTW, ptHi.y), cH, 1.5f);
                            pDL->AddLine(ImVec2(fMX-fTW, ptLo.y), ImVec2(fMX+fTW, ptLo.y), cH, 1.5f);
                            pDL->AddCircleFilled(ImPlot::PlotToPixels(mp.x, h.dVal), 3.5f, cH);
                        }
                        else
                        {
                            const ImVec2 ptVal = ImPlot::PlotToPixels(mp.x, h.dVal);
                            pDL->AddCircleFilled(ptVal, 3.5f, cH);
                            pDL->AddLine(ImVec2(fMX-7.0f, ptVal.y),
                                         ImVec2(fMX+7.0f, ptVal.y), cH, 1.0f);
                        }
                    }
                }
                else if (bCrossbarPrev)
                {
                    // Non-hovered panel: ghost hairline at the saved position
                    // from the previous frame (1-frame lag, imperceptible at 60fps).
                    if (fCrossbarXPrev >= pMin2.x && fCrossbarXPrev <= pMax2.x)
                    {
                        pDL->AddLine(ImVec2(fCrossbarXPrev, pMin2.y),
                                     ImVec2(fCrossbarXPrev, pMax2.y),
                                     IM_COL32(180, 180, 180, 40), 1.0f);
                    }
                }
            }

            // ---- 8l. No-data annotation (first panel only). ------------
            if (p == 0 && !HasData())
            {
                ImPlot::Annotation(0.0, 0.0,
                                   ImVec4(0.55f, 0.58f, 0.62f, 1.0f),
                                   ImVec2(0, 0), true, "Waiting for data...");
            }

            // ---- 8m. Capture axis limits. -------------------------------
            {
                ImPlotRect lim = ImPlot::GetPlotLimits(IMPLOT_AUTO, IMPLOT_AUTO);

                // X limits: capture once from the first panel (all panels share
                // the same X range via LinkAllX).
                if (!bXCaptured && lim.X.Size() > 0.0)
                {
                    m_dXLimitMin = lim.X.Min;
                    m_dXLimitMax = lim.X.Max;
                    bXCaptured   = true;
                }

                // Y-top tracking: update from ImPlot when not forced this frame
                // so the Shift+scroll zoom state persists across frames.
                if (ps.bYRangeSet && !bForceYThis && lim.Y.Size() > 0.0)
                {
                    ps.dYTop = lim.Y.Max;
                    // Persist Y top in the primary counter so it survives
                    // panel slot reassignment (e.g. visibility toggle).
                    if (ps.nKeys > 0)
                    {
                        auto itPrim = m_counters.find(ps.aKeys[0]);
                        if (itPrim != m_counters.end())
                            itPrim->second.dYUserTop = ps.dYTop;
                    }
                }
            }

            // ---- 8n. Hover and drag tracking. --------------------------
            if (ImPlot::IsPlotHovered())
            {
                m_bAnyPlotHoveredLastFrame = true;
                m_nHoveredPanelLastFrame   = p;
            }
            if (ImPlot::IsPlotHovered() &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                bUserInteracted = true;
            }

            ImPlot::EndPlot();
        } // end panel loop

        ImPlot::EndSubplots();
    }
    ImPlot::PopStyleVar();  // ImPlotStyleVar_PlotPadding

    // ---- 9. Crossbar tooltip (rendered outside all plot blocks). -------
    // Collected above from the hovered panel; shown here so it renders on
    // top of the subplot grid with the correct ImGui window stacking order.
    if (m_bCrossbarActive && nCrossHits > 0)
    {
        char timeBuf[32];
        WaveformRenderer::FormatTime(dCrossTime, timeBuf,
                                     static_cast<int>(sizeof(timeBuf)),
                                     static_cast<void*>(&m_dSessionOrigin));
        ImGui::SetNextWindowBgAlpha(0.60f);
        ImGui::BeginTooltip();
        ImGui::TextDisabled("t = %s", timeBuf);
        ImGui::Separator();
        for (int i = 0; i < nCrossHits; ++i)
        {
            const CrossInfo& h = crossHits[i];
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImVec4(h.color.x, h.color.y, h.color.z, 1.0f));
            const bool bRng = (h.nInCol > 1 && h.dHi > h.dLo &&
                (h.eMode == VizMode::Range || h.eMode == VizMode::Cyclogram));
            if (bRng)
                ImGui::Text("%-24s  avg %.5g  [%.5g .. %.5g]",
                            h.pName, h.dVal, h.dLo, h.dHi);
            else
                ImGui::Text("%-24s  %.5g", h.pName, h.dVal);
            ImGui::PopStyleColor();
        }
        ImGui::EndTooltip();
    }

    return bUserInteracted;
}

