// kv8scope -- Kv8 Software Oscilloscope
// CounterTree.cpp -- Counter / feed tree panel implementation.

#include "CounterTree.h"
#include "FontManager.h"
#include "StatsEngine.h"
#include "WaveformRenderer.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

/// Format a sample count with K / M suffix into buf[bufSize].
static void FormatCount(int64_t n, char* buf, int bufSize)
{
    if (n <= 0)
    {
        std::snprintf(buf, static_cast<size_t>(bufSize), "--");
    }
    else if (n < 1000)
    {
        std::snprintf(buf, static_cast<size_t>(bufSize), "%lld",
                      static_cast<long long>(n));
    }
    else if (n < 1000000)
    {
        std::snprintf(buf, static_cast<size_t>(bufSize), "%.1fK",
                      static_cast<double>(n) / 1000.0);
    }
    else
    {
        std::snprintf(buf, static_cast<size_t>(bufSize), "%.2fM",
                      static_cast<double>(n) / 1000000.0);
    }
}

/// Format a double without exponential notation, fitting a narrow stats column.
/// Adapts decimal places to the magnitude of the value.
static void FormatVal(double v, char* buf, int bufSize)
{
    const double av = (v < 0.0 ? -v : v);
    if (av == 0.0)
        std::snprintf(buf, static_cast<size_t>(bufSize), "0");
    else if (av < 0.01)
        std::snprintf(buf, static_cast<size_t>(bufSize), "%.5f", v);
    else if (av < 1.0)
        std::snprintf(buf, static_cast<size_t>(bufSize), "%.4f", v);
    else if (av < 10.0)
        std::snprintf(buf, static_cast<size_t>(bufSize), "%.3f", v);
    else if (av < 100.0)
        std::snprintf(buf, static_cast<size_t>(bufSize), "%.2f", v);
    else if (av < 10000.0)
        std::snprintf(buf, static_cast<size_t>(bufSize), "%.1f", v);
    else
        std::snprintf(buf, static_cast<size_t>(bufSize), "%.0f", v);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void CounterTree::Init(const kv8::SessionMeta& meta,
                       bool bOnline,
                       WaveformRenderer* pWaveform,
                       ConfigStore* pConfig,
                       const std::string& sPrefix)
{
    m_bOnline = bOnline;
    m_pConfig = pConfig;
    m_sPrefix = sPrefix;
    m_groups.clear();
    m_state.clear();

    for (const auto& [dwHash, sGroupName] : meta.hashToGroup)
    {
        auto cit = meta.hashToCounters.find(dwHash);
        if (cit == meta.hashToCounters.end() || cit->second.empty())
            continue;

        GroupEntry ge;
        ge.dwHash = dwHash;
        ge.sName  = sGroupName;

        int iOrder = 0;
        for (const auto& cm : cit->second)
        {
            // Skip UDT feed descriptors -- they are not scalar traces.
            // Virtual scalar fields (bIsUdtVirtualField) are included normally.
            if (cm.bIsUdtFeed)
                continue;

            CounterEntry ce;
            ce.wID          = cm.wCounterID;
            ce.sName        = cm.sName;
            ce.sDisplayName = cm.sDisplayName;
            ce.dYMinReg = cm.dbMin;
            ce.dYMaxReg = cm.dbMax;
            ge.counters.push_back(ce);

            CounterDisplayState st;
            st.bVisible      = true;
            st.bEnabled      = (cm.wFlags & 1u) != 0;
            st.traceColor    = pWaveform
                ? pWaveform->GetCounterColor(dwHash, cm.wCounterID)
                : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            st.traceColorOrig = st.traceColor;
            st.iOrder = iOrder++;

            m_state[MakeKey(dwHash, cm.wCounterID)] = st;
        }

        // Sort counters within this group by ascending Y-min so the order
        // matches the top-to-bottom panel order produced by WaveformRenderer
        // (which also sorts by yMin ascending in BuildPanelAssignment).
        std::sort(ge.counters.begin(), ge.counters.end(),
                  [](const CounterEntry& a, const CounterEntry& b)
                  { return a.dYMinReg < b.dYMinReg; });

        m_groups.push_back(std::move(ge));
    }

    // Sort groups themselves by the minimum dYMinReg across their counters
    // so the overall top-to-bottom order in the stats panel mirrors the graphs.
    std::sort(m_groups.begin(), m_groups.end(),
              [](const GroupEntry& a, const GroupEntry& b)
              {
                  double aMin = a.counters.empty() ? 0.0 : a.counters.front().dYMinReg;
                  double bMin = b.counters.empty() ? 0.0 : b.counters.front().dYMinReg;
                  return aMin < bMin;
              });

    // Apply saved per-counter viz modes from config.
    if (m_pConfig && !m_sPrefix.empty())
    {
        const auto& allModes = m_pConfig->Get().counterVizModes;
        auto sit = allModes.find(m_sPrefix);
        if (sit != allModes.end())
        {
            for (const auto& grp : m_groups)
            {
                for (const auto& ce : grp.counters)
                {
                    auto mit = sit->second.find(ce.sName);
                    if (mit == sit->second.end())
                        continue;

                    VizMode vm = VizMode::Simple;
                    if (mit->second == "range")      vm = VizMode::Range;
                    else if (mit->second == "cyclogram") vm = VizMode::Cyclogram;

                    uint64_t k = MakeKey(grp.dwHash, ce.wID);
                    auto stit = m_state.find(k);
                    if (stit != m_state.end())
                        stit->second.eVizMode = vm;
                    if (pWaveform)
                        pWaveform->SetCounterVizMode(grp.dwHash, ce.wID, vm);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AddCounters -- incremental (Fix 2 -- KV8_UDT_STALL)
// ---------------------------------------------------------------------------

void CounterTree::AddCounters(const kv8::SessionMeta& meta,
                              WaveformRenderer* pWaveform)
{
    for (const auto& [dwHash, sGroupName] : meta.hashToGroup)
    {
        auto cit = meta.hashToCounters.find(dwHash);
        if (cit == meta.hashToCounters.end() || cit->second.empty())
            continue;

        // Find existing group, or create a new one.
        GroupEntry* pGroup = nullptr;
        for (auto& g : m_groups)
        {
            if (g.dwHash == dwHash)
            {
                pGroup = &g;
                break;
            }
        }
        if (!pGroup)
        {
            GroupEntry ge;
            ge.dwHash = dwHash;
            ge.sName  = sGroupName;
            m_groups.push_back(std::move(ge));
            pGroup = &m_groups.back();
        }

        for (const auto& cm : cit->second)
        {
            if (cm.bIsUdtFeed)
                continue;

            uint64_t key = MakeKey(dwHash, cm.wCounterID);
            if (m_state.find(key) != m_state.end())
                continue;  // already known

            CounterEntry ce;
            ce.wID          = cm.wCounterID;
            ce.sName        = cm.sName;
            ce.sDisplayName = cm.sDisplayName;
            ce.dYMinReg     = cm.dbMin;
            ce.dYMaxReg     = cm.dbMax;
            pGroup->counters.push_back(ce);

            CounterDisplayState st;
            st.bVisible       = true;
            st.bEnabled       = (cm.wFlags & 1u) != 0;
            st.traceColor     = pWaveform
                ? pWaveform->GetCounterColor(dwHash, cm.wCounterID)
                : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            st.traceColorOrig = st.traceColor;
            st.iOrder         = static_cast<int>(pGroup->counters.size()) - 1;
            m_state[key]      = st;
        }

        // Re-sort counters within the group.
        std::sort(pGroup->counters.begin(), pGroup->counters.end(),
                  [](const CounterEntry& a, const CounterEntry& b)
                  { return a.dYMinReg < b.dYMinReg; });
    }

    // Re-sort groups.
    std::sort(m_groups.begin(), m_groups.end(),
              [](const GroupEntry& a, const GroupEntry& b)
              {
                  double aMin = a.counters.empty()
                                    ? 0.0 : a.counters.front().dYMinReg;
                  double bMin = b.counters.empty()
                                    ? 0.0 : b.counters.front().dYMinReg;
                  return aMin < bMin;
              });
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void CounterTree::Render(WaveformRenderer* pWaveform, StatsEngine* pStats)
{
    // Column layout:
    //   Online  (8) : E | V | Name | N | Vmin | Vmax | Vavg | Vmed
    //   Offline (7) : V | Name | N | Vmin | Vmax | Vavg | Vmed

    // Capture visible X limits so stats can be computed for the window.
    double dXMin = 0.0, dXMax = 0.0;
    bool bHasLimits = false;
    if (pWaveform && pWaveform->HasData())
    {
        pWaveform->GetVisibleXLimits(dXMin, dXMax);
        bHasLimits = (dXMax > dXMin);
    }

    // ---- Stats refresh throttle (P4.3) ---------------------------------
    // Rebuild caches every 4 frames (~15 Hz) to amortise the O(W) median scan.
    ++m_iStatsTick;
    if ((m_iStatsTick % 4) == 0 && pWaveform)
    {
        for (const auto& grp : m_groups)
        {
            for (const auto& ce : grp.counters)
            {
                uint64_t csk = MakeKey(grp.dwHash, ce.wID);
                if (bHasLimits)
                    m_statsCache[csk] =
                        pWaveform->QueryStats(grp.dwHash, ce.wID, dXMin, dXMax);
                else
                    m_statsCache[csk] = WaveformRenderer::CounterStats{};
            }
        }
        if (pStats)
        {
            for (const auto& grp : m_groups)
            {
                for (const auto& ce : grp.counters)
                {
                    uint64_t csk = MakeKey(grp.dwHash, ce.wID);
                    FullStatsCached fc;
                    fc.bValid = pStats->GetFullStats(
                        grp.dwHash, ce.wID, fc.n, fc.dMin, fc.dMax, fc.dAvg);
                    m_fullStatsCache[csk] = fc;
                }
            }
        }
    }

    // Column layout (all stats always visible -- no V/F toggle):
    //   Online  (12): E | V | Name | N | Nv | Vmin | Vmax | Vavg | Vmed | Min | Max | Avg
    //   Offline (11):     V | Name | N | Nv | Vmin | Vmax | Vavg | Vmed | Min | Max | Avg
    //
    // Name comes right after the checkbox column(s) so it is always visible.
    // N   = full-duration sample count  (StatsEngine)
    // Nv  = visible-window sample count (WaveformRenderer::QueryStats)
    // Vmin/Vmax/Vavg/Vmed = visible-window statistics
    // Min/Max/Avg          = full-duration statistics (StatsEngine)

    const int nCols = m_bOnline ? 13 : 12;

    // Per-layout key used for column-width persistence.
    // Suffix "c" marks the Name-first column order (bumped from "13"/"12" to
    // avoid loading stale widths from the previous E/V-first layout).
    const std::string sColKey = std::string("##CounterTable_") + (m_bOnline ? "13c" : "12c");

    // Column index constants -- Name is column 0 (leftmost, gets tree indent).
    // E and V follow immediately with IndentDisable so checkboxes never shift.
    const int iName = 0;
    const int iE    = m_bOnline ?  1 : -1;
    const int iV    = m_bOnline ?  2 :  1;
    const int iN    = m_bOnline ?  3 :  2;
    const int iNv   = m_bOnline ?  4 :  3;
    const int iVmin = m_bOnline ?  5 :  4;
    const int iVmax = m_bOnline ?  6 :  5;
    const int iVavg = m_bOnline ?  7 :  6;
    const int iVmed = m_bOnline ?  8 :  7;
    const int iFmin = m_bOnline ?  9 :  8;
    const int iFmax = m_bOnline ? 10 :  9;
    const int iFavg = m_bOnline ? 11 : 10;

    ImGuiTableFlags flags =
        ImGuiTableFlags_BordersOuter   |
        ImGuiTableFlags_BordersInnerV  |
        ImGuiTableFlags_RowBg          |
        ImGuiTableFlags_ScrollY        |
        ImGuiTableFlags_Resizable      |
        ImGuiTableFlags_SizingFixedFit;

    // Tighter row height: preserve horizontal cell margin but reduce
    // vertical padding to 1 px so the feed list is more compact.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                        ImVec2(ImGui::GetStyle().CellPadding.x, 1.0f));
    if (!ImGui::BeginTable("##CounterTable", nCols, flags))
    {
        ImGui::PopStyleVar();
        return;
    }

    // ---- Column setup ---------------------------------------------------
    // Widths are derived from representative text so they scale with font size.
    const float fPad      = ImGui::GetStyle().CellPadding.x * 2.0f + 14.0f;
    const float fCheckW   = ImGui::GetFrameHeight() * 0.7f + fPad;     // fits checkbox
    const float fColCount = ImGui::CalcTextSize("12.34M").x  + fPad;  // N / Nv
    const float fColStat  = ImGui::CalcTextSize("-12345.6").x + fPad; // stat values

    // Load saved column widths up front so they can be passed directly to
    // TableSetupColumn as init_width_or_weight.  TableSetColumnWidth must NOT
    // be called between BeginTable and TableHeadersRow -- MinColumnWidth is
    // only set inside TableUpdateLayout (triggered by TableHeadersRow), so
    // calling it before that asserts on a brand-new table.
    float aSavedW[13] = {};  // sized for max nCols (online=13, offline=12)
    if (m_pConfig)
    {
        const auto& tw = m_pConfig->Get().tableColumnWidths;
        auto twit = tw.find(sColKey);
        if (twit != tw.end() && static_cast<int>(twit->second.size()) == nCols)
            for (int k = 0; k < nCols; ++k)
                aSavedW[k] = twit->second[k];
    }
    // Returns saved pixel width when available, else the computed default.
    auto colW = [&](int k, float fDefault) -> float {
        return (aSavedW[k] > 0.0f) ? aSavedW[k] : fDefault;
    };

    // Name first: tree indentation shows hierarchy here.
    // E and V have IndentDisable so checkboxes stay left-aligned at all depths.
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch |
                                    ImGuiTableColumnFlags_IndentEnable, 1.0f);
    if (m_bOnline)
        ImGui::TableSetupColumn("E",    ImGuiTableColumnFlags_WidthFixed  |
                                        ImGuiTableColumnFlags_NoResize    |
                                        ImGuiTableColumnFlags_IndentDisable, fCheckW);
    ImGui::TableSetupColumn("V",    ImGuiTableColumnFlags_WidthFixed  |
                                    ImGuiTableColumnFlags_NoResize    |
                                    ImGuiTableColumnFlags_IndentDisable,    fCheckW);
    ImGui::TableSetupColumn("N",    ImGuiTableColumnFlags_WidthFixed, colW(iN,    fColCount));
    ImGui::TableSetupColumn("Nv",   ImGuiTableColumnFlags_WidthFixed, colW(iNv,   fColCount));
    ImGui::TableSetupColumn("Vmin", ImGuiTableColumnFlags_WidthFixed, colW(iVmin, fColStat));
    ImGui::TableSetupColumn("Vmax", ImGuiTableColumnFlags_WidthFixed, colW(iVmax, fColStat));
    ImGui::TableSetupColumn("Vavg", ImGuiTableColumnFlags_WidthFixed, colW(iVavg, fColStat));
    ImGui::TableSetupColumn("Vmed", ImGuiTableColumnFlags_WidthFixed, colW(iVmed, fColStat));
    ImGui::TableSetupColumn("Min",  ImGuiTableColumnFlags_WidthFixed, colW(iFmin, fColStat));
    ImGui::TableSetupColumn("Max",  ImGuiTableColumnFlags_WidthFixed, colW(iFmax, fColStat));
    ImGui::TableSetupColumn("Avg",  ImGuiTableColumnFlags_WidthFixed, colW(iFavg, fColStat));
    ImGui::TableSetupColumn("##pad", ImGuiTableColumnFlags_WidthFixed |
                                     ImGuiTableColumnFlags_NoResize, 8.0f); // right-edge padding

    ImGui::TableSetupScrollFreeze(0, 1);  // freeze header row
    ImGui::TableHeadersRow();

    // ---- Group / counter rows ------------------------------------------

    ImDrawList* pDL = ImGui::GetWindowDrawList();

    for (int g = 0; g < static_cast<int>(m_groups.size()); ++g)
    {
        GroupEntry& grp = m_groups[g];
        const int nTotal = static_cast<int>(grp.counters.size());

        // Aggregate visible / enabled counts for tri-state checkboxes.
        int nVisible = 0, nEnabled = 0;
        for (const auto& ce : grp.counters)
        {
            auto it = m_state.find(MakeKey(grp.dwHash, ce.wID));
            if (it != m_state.end())
            {
                if (it->second.bVisible) ++nVisible;
                if (it->second.bEnabled) ++nEnabled;
            }
        }

        // ---- Group row -------------------------------------------------

        ImGui::TableNextRow();

        // E column -- aggregate Enabled toggle (online only).
        if (m_bOnline && iE >= 0)
        {
            ImGui::TableSetColumnIndex(iE);
            // Show as checked only when ALL counters are enabled.
            // Any click drives the whole group to the new (toggled) state.
            bool bGroupE = (nEnabled == nTotal);
            char szId[32];
            std::snprintf(szId, sizeof(szId), "##GE%d", g);
            if (ImGui::Checkbox(szId, &bGroupE))
            {
                for (const auto& ce : grp.counters)
                {
                    auto it = m_state.find(MakeKey(grp.dwHash, ce.wID));
                    if (it != m_state.end())
                    {
                        it->second.bEnabled = bGroupE;
                        if (m_onEnableChanged)
                            m_onEnableChanged(grp.dwHash, ce.wID, bGroupE);
                    }
                }
            }
        }

        // V column -- aggregate Visible toggle.
        ImGui::TableSetColumnIndex(iV);
        {
            // Show as checked only when ALL counters are visible.
            bool bGroupV = (nVisible == nTotal);
            char szId[32];
            std::snprintf(szId, sizeof(szId), "##GV%d", g);
            if (ImGui::Checkbox(szId, &bGroupV) && pWaveform)
            {
                for (const auto& ce : grp.counters)
                {
                    auto it = m_state.find(MakeKey(grp.dwHash, ce.wID));
                    if (it != m_state.end())
                        it->second.bVisible = bGroupV;
                    pWaveform->SetCounterVisible(grp.dwHash, ce.wID, bGroupV);
                }
            }
        }

        // Name column -- group tree node (no per-group stats; stats are per-counter only).
        ImGui::TableSetColumnIndex(iName);
        char szGrpLabel[256];
        std::snprintf(szGrpLabel, sizeof(szGrpLabel),
                      "%s  (%d)##G%d",
                      grp.sName.c_str(), nTotal, g);
        ImGuiTreeNodeFlags grpFlags = ImGuiTreeNodeFlags_DefaultOpen;
        if (FontManager* pFM = FontManager::Get()) pFM->PushBold();
        const bool bOpen = ImGui::TreeNodeEx(szGrpLabel, grpFlags);
        FontManager::PopFont();

        if (!bOpen)
            continue;   // TreeNodeEx returned false -- no TreePop needed

        // ---- Counter rows: build per-group path tree, then render ------
        // Counter names use '/' as a hierarchy separator (e.g. "cpu/load/user").
        // We build a local tree each frame and render it recursively so that
        // each '/' level becomes a collapsible sub-folder inside the channel.

        struct LocalPathNode
        {
            std::string                sSeg;      // path segment label
            std::vector<LocalPathNode> children;  // ordered by first-seen
            std::vector<int>           leaves;    // flat indices into grp.counters
        };

        // Build the tree iteratively by splitting counter names on '/'.
        LocalPathNode lnRoot;
        for (int ci2 = 0; ci2 < nTotal; ++ci2)
        {
            const std::string& sN = grp.counters[ci2].sName;
            LocalPathNode* pN = &lnRoot;
            std::size_t p2 = 0;
            while (true)
            {
                const std::size_t sl    = sN.find('/', p2);
                const bool        bLast = (sl == std::string::npos);
                const std::string seg   = bLast ? sN.substr(p2)
                                                : sN.substr(p2, sl - p2);
                LocalPathNode* pC = nullptr;
                for (auto& c : pN->children)
                    if (c.sSeg == seg) { pC = &c; break; }
                if (!pC)
                {
                    pN->children.push_back(LocalPathNode{});
                    pN->children.back().sSeg = seg;
                    pC = &pN->children.back();
                }
                if (bLast) { pC->leaves.push_back(ci2); break; }
                p2  = sl + 1;
                pN  = pC;
            }
        }

        // Collect all descendant leaf counter indices under a node (depth-first).
        // Used to compute aggregate V/E state for sub-folder rows.
        std::function<void(const LocalPathNode&, std::vector<int>&)> collectLeaves;
        collectLeaves = [&](const LocalPathNode& n, std::vector<int>& out)
        {
            for (int ci : n.leaves) out.push_back(ci);
            for (const auto& c : n.children) collectLeaves(c, out);
        };

        // renderCounterRow: renders one table row for flat counter index 'ci'.
        // sDispName is the label shown in the Name column -- the last path
        // segment when inside a sub-tree, or the full name when unambiguous.
        auto renderCounterRow = [&](int ci, const char* sDispName)
        {
            const CounterEntry& ce = grp.counters[ci];
            const uint64_t key = MakeKey(grp.dwHash, ce.wID);
            auto it = m_state.find(key);
            if (it == m_state.end())
                return;
            CounterDisplayState& st = it->second;

            ImGui::TableNextRow();

            // P3.7: Tint the row when it is the highlighted counter.
            if (st.bHighlighted)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                       IM_COL32(55, 90, 155, 160));

            // Dim the row when the counter is not visible.
            const float fAlpha = st.bVisible ? 1.0f : 0.45f;

            // E column (online only).
            if (m_bOnline && iE >= 0)
            {
                ImGui::TableSetColumnIndex(iE);
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                    ImGui::GetStyle().Alpha * fAlpha);
                char szId[32];
                std::snprintf(szId, sizeof(szId), "##E%d_%d", g, ci);
                if (ImGui::Checkbox(szId, &st.bEnabled))
                {
                    if (m_onEnableChanged)
                        m_onEnableChanged(grp.dwHash, ce.wID, st.bEnabled);
                }
                ImGui::PopStyleVar();
            }

            // V column.
            ImGui::TableSetColumnIndex(iV);
            {
                char szId[32];
                std::snprintf(szId, sizeof(szId), "##V%d_%d", g, ci);
                if (ImGui::Checkbox(szId, &st.bVisible) && pWaveform)
                    pWaveform->SetCounterVisible(grp.dwHash, ce.wID,
                                                 st.bVisible);
            }

            // Name column: 12x12 color swatch then counter name.
            ImGui::TableSetColumnIndex(iName);
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                    ImGui::GetStyle().Alpha * fAlpha);

                // Color swatch: filled rect drawn at cursor position.
                ImVec2 p = ImGui::GetCursorScreenPos();
                const float kSw = 12.0f;
                const float kSh = 12.0f;
                // Vertically centre within the text line height.
                float fTextH = ImGui::GetTextLineHeight();
                float fVOff  = (fTextH - kSh) * 0.5f;
                if (fVOff < 0.0f) fVOff = 0.0f;

                ImU32 swCol = ImGui::ColorConvertFloat4ToU32(st.traceColor);
                // Fade the swatch when invisible.
                if (!st.bVisible)
                    swCol = (swCol & 0x00FFFFFFu) | 0x48000000u;
                pDL->AddRectFilled(
                    ImVec2(p.x,        p.y + fVOff),
                    ImVec2(p.x + kSw,  p.y + fVOff + kSh),
                    swCol);

                // Reserve space for the swatch and place text beside it.
                ImGui::Dummy(ImVec2(kSw + 4.0f, fTextH));
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextUnformatted(sDispName);

                // Show full counter path as a tooltip when the name is abbreviated
                // (i.e. only the last path segment is shown inside a sub-tree).
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) &&
                    std::strcmp(sDispName, ce.sName.c_str()) != 0)
                    ImGui::SetTooltip("%s", ce.sName.c_str());

                ImGui::PopStyleVar(); // alpha

                // ---- P3.8: Drag-and-drop source -------------------------
                // TextUnformatted has no ImGui ID (ID=0).  Without
                // SourceAllowNullID, BeginDragDropSource would fire
                // IM_ASSERT(0) whenever the mouse is held over the label,
                // crashing debug builds.  SourceAllowNullID uses a throwaway
                // rect-based ID so the drag works correctly; a quick click
                // (no drag motion) still returns false here, allowing the
                // highlight-toggle handler below to fire normally.
                if (ImGui::BeginDragDropSource(
                        ImGuiDragDropFlags_SourceAllowNullID))
                {
                    struct DragPayload { int g; int ci; };
                    DragPayload pl{g, ci};
                    ImGui::SetDragDropPayload("CT_COUNTER", &pl, sizeof(pl));
                    ImGui::Text("Move: %s", ce.sName.c_str());
                    ImGui::EndDragDropSource();
                }

                // ---- P3.8: Drag-and-drop target -------------------------
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* pay =
                            ImGui::AcceptDragDropPayload("CT_COUNTER"))
                    {
                        struct DragPayload { int g; int ci; };
                        const DragPayload* src =
                            static_cast<const DragPayload*>(pay->Data);
                        // Only allow reorder within the same group.
                        if (src->g == g && src->ci != ci)
                        {
                            std::swap(grp.counters[src->ci], grp.counters[ci]);
                            for (int k = 0;
                                 k < static_cast<int>(grp.counters.size()); ++k)
                            {
                                auto sit = m_state.find(
                                    MakeKey(grp.dwHash, grp.counters[k].wID));
                                if (sit != m_state.end())
                                    sit->second.iOrder = k;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                // ---- P3.7: Left-click toggles highlight -----------------
                if (ImGui::IsItemHovered() &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    const uint64_t thisKey = MakeKey(grp.dwHash, ce.wID);
                    if (m_nHighlightedKey == thisKey)
                    {
                        // Toggle off.
                        m_nHighlightedKey = 0;
                        st.bHighlighted   = false;
                        if (pWaveform) pWaveform->ClearHighlighted();
                    }
                    else
                    {
                        // Set this counter as highlighted.
                        for (auto& [k2, s2] : m_state)
                            s2.bHighlighted = false;
                        m_nHighlightedKey = thisKey;
                        st.bHighlighted   = true;
                        if (pWaveform)
                            pWaveform->SetHighlighted(grp.dwHash, ce.wID);
                    }
                }

                // ---- P3.5/P3.6: Right-click opens context menu ----------
                // NOTE: OpenPopup must NOT be called here -- the table uses
                // ImGuiTableFlags_ScrollY which creates an inner child window.
                // Calling OpenPopup inside that inner window and BeginPopup
                // outside creates an ID-stack mismatch so BeginPopup never
                // matches.  Instead we set a deferred flag and call OpenPopup
                // after EndTable() (back at the outer window scope).
                if (ImGui::IsItemHovered(
                        ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                {
                    m_iCtxGroup   = g;
                    m_iCtxCounter = ci;
                    m_dPopupYMin  = std::isnan(st.dYMin) ? ce.dYMinReg
                                                         : st.dYMin;
                    m_dPopupYMax  = std::isnan(st.dYMax) ? ce.dYMaxReg
                                                         : st.dYMax;
                    m_popupCol[0] = st.traceColor.x;
                    m_popupCol[1] = st.traceColor.y;
                    m_popupCol[2] = st.traceColor.z;
                    m_bCtxPending = true; // deferred: open popup after EndTable
                }
            }

            // Stats columns: all visible-window + full-duration side by side.
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                    ImGui::GetStyle().Alpha * fAlpha);

                uint64_t csk = MakeKey(grp.dwHash, ce.wID);

                // ---- Visible-window stats (from WaveformRenderer cache) ----
                char szNv[32], szVmin[32], szVmax[32], szVavg[32], szVmed[32];
                auto csit2 = m_statsCache.find(csk);
                if (csit2 != m_statsCache.end() && csit2->second.nWindow > 0)
                    FormatCount(csit2->second.nWindow, szNv, sizeof(szNv));
                else
                    std::strcpy(szNv, "--");
                if (csit2 != m_statsCache.end() && csit2->second.bHasWindowData)
                {
                    FormatVal(csit2->second.dMin,    szVmin, sizeof(szVmin));
                    FormatVal(csit2->second.dMax,    szVmax, sizeof(szVmax));
                    FormatVal(csit2->second.dAvg,    szVavg, sizeof(szVavg));
                    FormatVal(csit2->second.dMedian, szVmed, sizeof(szVmed));
                }
                else
                {
                    std::strcpy(szVmin, "--");
                    std::strcpy(szVmax, "--");
                    std::strcpy(szVavg, "--");
                    std::strcpy(szVmed, "--");
                }

                // ---- Full-duration stats (from StatsEngine cache) -----------
                char szN[32], szFmin[32], szFmax[32], szFavg[32];
                auto fit2 = m_fullStatsCache.find(csk);
                if (fit2 != m_fullStatsCache.end() && fit2->second.bValid)
                {
                    FormatCount(static_cast<int64_t>(fit2->second.n),
                                szN, sizeof(szN));
                    FormatVal(fit2->second.dMin, szFmin, sizeof(szFmin));
                    FormatVal(fit2->second.dMax, szFmax, sizeof(szFmax));
                    FormatVal(fit2->second.dAvg, szFavg, sizeof(szFavg));
                }
                else
                {
                    // Fall back to nTotal from visible-window cache if StatsEngine
                    // has not registered the counter yet.
                    if (csit2 != m_statsCache.end() && csit2->second.nTotal > 0)
                        FormatCount(csit2->second.nTotal, szN, sizeof(szN));
                    else
                        std::strcpy(szN, "--");
                    std::strcpy(szFmin, "--");
                    std::strcpy(szFmax, "--");
                    std::strcpy(szFavg, "--");
                }

                ImGui::TableSetColumnIndex(iN);    ImGui::Text("%s", szN);
                ImGui::TableSetColumnIndex(iNv);   ImGui::Text("%s", szNv);
                ImGui::TableSetColumnIndex(iVmin); ImGui::Text("%s", szVmin);
                ImGui::TableSetColumnIndex(iVmax); ImGui::Text("%s", szVmax);
                ImGui::TableSetColumnIndex(iVavg); ImGui::Text("%s", szVavg);
                ImGui::TableSetColumnIndex(iVmed); ImGui::Text("%s", szVmed);
                ImGui::TableSetColumnIndex(iFmin); ImGui::Text("%s", szFmin);
                ImGui::TableSetColumnIndex(iFmax); ImGui::Text("%s", szFmax);
                ImGui::TableSetColumnIndex(iFavg); ImGui::Text("%s", szFavg);

                ImGui::PopStyleVar();
            }
        };  // end renderCounterRow

        // renderPathNode: recursively renders the path sub-tree.
        // sPath is the accumulated path string from the channel root to this node.
        // It provides a stable, unique ImGui tree-node ID so that open/closed state
        // is preserved correctly across frames (stack pointer IDs change every frame).
        std::function<void(const LocalPathNode&, const std::string&)> renderPathNode;
        renderPathNode = [&](const LocalPathNode& node, const std::string& sPath)
        {
            // Direct leaves (edge case: counter name = intermediate segment).
            for (int ci : node.leaves)
                renderCounterRow(ci, grp.counters[ci].sName.c_str());

            for (const auto& child : node.children)
            {
                if (child.children.empty())
                {
                    // Pure leaf: render as a counter row.
                    // Prefer the human-readable display name when available
                    // (e.g. "Wind Speed (m/s)" instead of the C identifier
                    // "wind_speed_ms" that was stored as the path segment).
                    for (int ci : child.leaves)
                    {
                        const std::string& disp = grp.counters[ci].sDisplayName;
                        renderCounterRow(ci,
                            disp.empty() ? child.sSeg.c_str() : disp.c_str());
                    }
                }
                else
                {
                    // Intermediate path segment -- collapsible sub-folder row.
                    // Stable ID = group index + full path accumulated to this node.
                    const std::string sChildPath = sPath + "/" + child.sSeg;

                    // Collect descendants to drive aggregate checkboxes.
                    std::vector<int> descLeaves;
                    collectLeaves(child, descLeaves);
                    const int nDesc      = static_cast<int>(descLeaves.size());
                    int nDescVisible = 0, nDescEnabled = 0;
                    for (int dci : descLeaves)
                    {
                        auto sit = m_state.find(
                            MakeKey(grp.dwHash, grp.counters[dci].wID));
                        if (sit != m_state.end())
                        {
                            if (sit->second.bVisible) ++nDescVisible;
                            if (sit->second.bEnabled) ++nDescEnabled;
                        }
                    }

                    ImGui::TableNextRow();

                    // E column -- aggregate Enabled toggle (online only).
                    if (m_bOnline && iE >= 0)
                    {
                        ImGui::TableSetColumnIndex(iE);
                        bool bSubE = (nDescEnabled == nDesc);
                        char szEId[512];
                        std::snprintf(szEId, sizeof(szEId), "##SFE%d_%s",
                                      g, sChildPath.c_str());
                        if (ImGui::Checkbox(szEId, &bSubE))
                            for (int dci : descLeaves)
                            {
                                auto sit = m_state.find(
                                    MakeKey(grp.dwHash, grp.counters[dci].wID));
                                if (sit != m_state.end())
                                    sit->second.bEnabled = bSubE;
                            }
                    }

                    // V column -- aggregate Visible toggle.
                    ImGui::TableSetColumnIndex(iV);
                    {
                        bool bSubV = (nDescVisible == nDesc);
                        char szVId[512];
                        std::snprintf(szVId, sizeof(szVId), "##SFV%d_%s",
                                      g, sChildPath.c_str());
                        if (ImGui::Checkbox(szVId, &bSubV) && pWaveform)
                            for (int dci : descLeaves)
                            {
                                auto sit = m_state.find(
                                    MakeKey(grp.dwHash, grp.counters[dci].wID));
                                if (sit != m_state.end())
                                    sit->second.bVisible = bSubV;
                                pWaveform->SetCounterVisible(
                                    grp.dwHash, grp.counters[dci].wID, bSubV);
                            }
                    }

                    // Name column -- collapsible sub-folder tree node.
                    ImGui::TableSetColumnIndex(iName);
                    char szSubId[512];
                    std::snprintf(szSubId, sizeof(szSubId), "%s##SF%d_%s",
                                  child.sSeg.c_str(), g, sChildPath.c_str());
                    const bool bSubOpen = ImGui::TreeNodeEx(szSubId,
                        ImGuiTreeNodeFlags_DefaultOpen |
                        ImGuiTreeNodeFlags_SpanFullWidth);
                    if (bSubOpen)
                    {
                        for (int ci : child.leaves)
                            renderCounterRow(ci, grp.counters[ci].sName.c_str());
                        renderPathNode(child, sChildPath);
                        ImGui::TreePop();
                    }
                }
            }
        };

        renderPathNode(lnRoot, grp.sName);
        ImGui::TreePop();
    }

    // Persist column widths: compare to saved, mark dirty only when changed.
    if (m_pConfig)
    {
        ImGuiTable* pT  = ImGui::GetCurrentTable();
        auto& tw    = m_pConfig->GetMut().tableColumnWidths;
        auto& saved = tw[sColKey];
        if (static_cast<int>(saved.size()) != nCols) saved.assign(nCols, 0.0f);
        bool bW = false;
        if (pT)
            for (int k = 0; k < nCols; ++k)
            {
                const float w = pT->Columns[k].WidthGiven;
                if (w > 0.0f && std::fabs(w - saved[k]) > 0.5f) { saved[k] = w; bW = true; }
            }
        if (bW) m_pConfig->MarkDirty();
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();  // ImGuiStyleVar_CellPadding

    // ---- Context menu popup (P3.5 / P3.6 / P3.7) -----------------------
    // OpenPopup is called HERE (after EndTable) so it is at the outer window
    // scope, matching the BeginPopup call below.  This avoids the ID-stack
    // mismatch that occurs when OpenPopup is called inside the ScrollY table's
    // inner child window.
    if (m_bCtxPending)
    {
        ImGui::OpenPopup("##CounterCtx");
        m_bCtxPending = false;
    }

    if (ImGui::BeginPopup("##CounterCtx"))
    {
        if (m_iCtxGroup   >= 0 &&
            m_iCtxGroup   <  static_cast<int>(m_groups.size()) &&
            m_iCtxCounter >= 0)
        {
            GroupEntry&   cg  = m_groups[m_iCtxGroup];
            if (m_iCtxCounter < static_cast<int>(cg.counters.size()))
            {
                CounterEntry& cce = cg.counters[m_iCtxCounter];
                const uint64_t ckey = MakeKey(cg.dwHash, cce.wID);
                auto csit = m_state.find(ckey);
                const bool bHaveSt = (csit != m_state.end());

                // Highlight toggle.
                const bool bIsHl = (m_nHighlightedKey == ckey);
                if (ImGui::MenuItem(bIsHl ? "Clear highlight" : "Highlight"))
                {
                    if (!bIsHl)
                    {
                        for (auto& [k2, s2] : m_state) s2.bHighlighted = false;
                        m_nHighlightedKey = ckey;
                        if (bHaveSt) csit->second.bHighlighted = true;
                        if (pWaveform) pWaveform->SetHighlighted(cg.dwHash, cce.wID);
                    }
                    else
                    {
                        m_nHighlightedKey = 0;
                        for (auto& [k2, s2] : m_state) s2.bHighlighted = false;
                        if (pWaveform) pWaveform->ClearHighlighted();
                    }
                    ImGui::CloseCurrentPopup();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Set Y range..."))
                {
                    m_bYRangeOpen = true;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Reset Y range") && pWaveform)
                {
                    if (bHaveSt)
                    {
                        csit->second.dYMin =
                            std::numeric_limits<double>::quiet_NaN();
                        csit->second.dYMax =
                            std::numeric_limits<double>::quiet_NaN();
                    }
                    pWaveform->ResetCounterYRange(cg.dwHash, cce.wID);
                    ImGui::CloseCurrentPopup();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Change color..."))
                {
                    m_bColorOpen = true;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Reset color"))
                {
                    ImVec4 orig = pWaveform
                        ? pWaveform->GetPaletteColor(cg.dwHash, cce.wID)
                        : (bHaveSt ? csit->second.traceColorOrig
                                   : ImVec4(1,1,1,1));
                    if (bHaveSt) csit->second.traceColor = orig;
                    if (pWaveform) pWaveform->SetCounterColor(cg.dwHash, cce.wID, orig);
                    ImGui::CloseCurrentPopup();
                }

                ImGui::Separator();

                // Visualization mode submenu (P6.6 per-counter).
                if (ImGui::BeginMenu("Visualization mode"))
                {
                    const VizMode curViz =
                        bHaveSt ? csit->second.eVizMode : VizMode::Simple;

                    if (ImGui::MenuItem("Simple",    nullptr,
                                        curViz == VizMode::Simple))
                    {
                        if (bHaveSt) csit->second.eVizMode = VizMode::Simple;
                        if (pWaveform)
                            pWaveform->SetCounterVizMode(cg.dwHash, cce.wID,
                                                         VizMode::Simple);
                        SaveCounterVizMode(cce.sName, VizMode::Simple);
                    }
                    if (ImGui::MenuItem("Range",     nullptr,
                                        curViz == VizMode::Range))
                    {
                        if (bHaveSt) csit->second.eVizMode = VizMode::Range;
                        if (pWaveform)
                            pWaveform->SetCounterVizMode(cg.dwHash, cce.wID,
                                                         VizMode::Range);
                        SaveCounterVizMode(cce.sName, VizMode::Range);
                    }
                    if (ImGui::MenuItem("Cyclogram", nullptr,
                                        curViz == VizMode::Cyclogram))
                    {
                        if (bHaveSt) csit->second.eVizMode = VizMode::Cyclogram;
                        if (pWaveform)
                            pWaveform->SetCounterVizMode(cg.dwHash, cce.wID,
                                                         VizMode::Cyclogram);
                        SaveCounterVizMode(cce.sName, VizMode::Cyclogram);
                    }
                    ImGui::EndMenu();
                }
            }
        }
        ImGui::EndPopup();
    }

    // ---- Y-range modal popup (P3.5) -------------------------------------

    if (m_bYRangeOpen)
    {
        ImGui::OpenPopup("##YRangeModal");
        m_bYRangeOpen = false;
    }
    if (ImGui::BeginPopupModal("##YRangeModal", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Y-axis range:");
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputDouble("Min##yr", &m_dPopupYMin, 0.0, 0.0, "%.6g");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputDouble("Max##yr", &m_dPopupYMax, 0.0, 0.0, "%.6g");
        ImGui::Spacing();
        if (ImGui::Button("Apply", ImVec2(80.0f, 0.0f)))
        {
            if (m_iCtxGroup   >= 0 &&
                m_iCtxGroup   <  static_cast<int>(m_groups.size()) &&
                m_iCtxCounter >= 0)
            {
                GroupEntry&   cg  = m_groups[m_iCtxGroup];
                if (m_iCtxCounter < static_cast<int>(cg.counters.size()))
                {
                    CounterEntry& cce = cg.counters[m_iCtxCounter];
                    auto csit = m_state.find(MakeKey(cg.dwHash, cce.wID));
                    if (csit != m_state.end() && m_dPopupYMin < m_dPopupYMax)
                    {
                        csit->second.dYMin = m_dPopupYMin;
                        csit->second.dYMax = m_dPopupYMax;
                        if (pWaveform)
                            pWaveform->SetCounterYRange(cg.dwHash, cce.wID,
                                                        m_dPopupYMin,
                                                        m_dPopupYMax);
                    }
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to registry", ImVec2(0.0f, 0.0f)))
        {
            if (m_iCtxGroup   >= 0 &&
                m_iCtxGroup   <  static_cast<int>(m_groups.size()) &&
                m_iCtxCounter >= 0)
            {
                GroupEntry&   cg  = m_groups[m_iCtxGroup];
                if (m_iCtxCounter < static_cast<int>(cg.counters.size()))
                {
                    CounterEntry& cce = cg.counters[m_iCtxCounter];
                    auto csit = m_state.find(MakeKey(cg.dwHash, cce.wID));
                    if (csit != m_state.end())
                    {
                        csit->second.dYMin =
                            std::numeric_limits<double>::quiet_NaN();
                        csit->second.dYMax =
                            std::numeric_limits<double>::quiet_NaN();
                        if (pWaveform)
                            pWaveform->ResetCounterYRange(cg.dwHash, cce.wID);
                    }
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- Color modal popup (P3.6) ---------------------------------------

    if (m_bColorOpen)
    {
        ImGui::OpenPopup("##ColorModal");
        m_bColorOpen = false;
    }
    if (ImGui::BeginPopupModal("##ColorModal", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Trace color:");
        ImGui::ColorPicker3("##ColPicker", m_popupCol,
                            ImGuiColorEditFlags_DisplayRGB |
                            ImGuiColorEditFlags_NoTooltip);
        ImGui::Spacing();
        if (ImGui::Button("Apply", ImVec2(80.0f, 0.0f)))
        {
            if (m_iCtxGroup   >= 0 &&
                m_iCtxGroup   <  static_cast<int>(m_groups.size()) &&
                m_iCtxCounter >= 0)
            {
                GroupEntry&   cg  = m_groups[m_iCtxGroup];
                if (m_iCtxCounter < static_cast<int>(cg.counters.size()))
                {
                    CounterEntry& cce = cg.counters[m_iCtxCounter];
                    ImVec4 newCol(m_popupCol[0], m_popupCol[1],
                                  m_popupCol[2], 1.0f);
                    auto csit = m_state.find(MakeKey(cg.dwHash, cce.wID));
                    if (csit != m_state.end())
                        csit->second.traceColor = newCol;
                    if (pWaveform)
                        pWaveform->SetCounterColor(cg.dwHash, cce.wID, newCol);
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

} // CounterTree::Render

// ---------------------------------------------------------------------------
// BatchSetVizMode -- set all counters + persist
// ---------------------------------------------------------------------------

void CounterTree::BatchSetVizMode(VizMode m, WaveformRenderer* pWaveform)
{
    for (auto& [key, st] : m_state)
    {
        st.eVizMode = m;
        if (pWaveform)
            pWaveform->SetCounterVizMode(KeyHash(key), KeyID(key), m);
    }
    // Also update the global default so newly registered counters inherit it.
    if (pWaveform)
        pWaveform->SetVizMode(m);

    // Persist: update every owned counter name in the session JSON block.
    if (m_pConfig && !m_sPrefix.empty())
    {
        const char* label = (m == VizMode::Range)     ? "range"
                          : (m == VizMode::Cyclogram)  ? "cyclogram"
                          : "simple";
        auto& perSession = m_pConfig->GetMut().counterVizModes[m_sPrefix];
        for (const auto& grp : m_groups)
            for (const auto& ce : grp.counters)
                perSession[ce.sName] = label;
        m_pConfig->MarkDirty();
        m_pConfig->Save();
    }
}

// ---------------------------------------------------------------------------
// SaveCounterVizMode -- persist one counter's mode
// ---------------------------------------------------------------------------

void CounterTree::SaveCounterVizMode(const std::string& sName, VizMode m)
{
    if (!m_pConfig || m_sPrefix.empty())
        return;

    const char* label = (m == VizMode::Range)     ? "range"
                      : (m == VizMode::Cyclogram)  ? "cyclogram"
                      : "simple";
    m_pConfig->GetMut().counterVizModes[m_sPrefix][sName] = label;
    m_pConfig->MarkDirty();
    m_pConfig->Save();
}

// ---------------------------------------------------------------------------
// ShowAll / HideAll
// ---------------------------------------------------------------------------

void CounterTree::ShowAll(WaveformRenderer* pWaveform)
{
    for (auto& [key, st] : m_state)
    {
        st.bVisible = true;
        if (pWaveform)
            pWaveform->SetCounterVisible(KeyHash(key), KeyID(key), true);
    }
}

void CounterTree::HideAll(WaveformRenderer* pWaveform)
{
    for (auto& [key, st] : m_state)
    {
        st.bVisible = false;
        if (pWaveform)
            pWaveform->SetCounterVisible(KeyHash(key), KeyID(key), false);
    }
}

// ---------------------------------------------------------------------------
// Highlight control (P3.7)
// ---------------------------------------------------------------------------

void CounterTree::SetHighlighted(uint32_t dwHash, uint16_t wCounterID,
                                  WaveformRenderer* pWaveform)
{
    uint64_t newKey = MakeKey(dwHash, wCounterID);
    for (auto& [key, st] : m_state)
        st.bHighlighted = (key == newKey);
    m_nHighlightedKey = newKey;
    if (pWaveform) pWaveform->SetHighlighted(dwHash, wCounterID);
}

void CounterTree::ClearHighlighted(WaveformRenderer* pWaveform)
{
    for (auto& [key, st] : m_state)
        st.bHighlighted = false;
    m_nHighlightedKey = 0;
    if (pWaveform) pWaveform->ClearHighlighted();
}

// ---------------------------------------------------------------------------
// GetState
// ---------------------------------------------------------------------------

const CounterDisplayState* CounterTree::GetState(uint32_t dwHash,
                                                  uint16_t wCounterID) const
{
    auto it = m_state.find(MakeKey(dwHash, wCounterID));
    return (it != m_state.end()) ? &it->second : nullptr;
}

void CounterTree::SetCounterEnabled(uint32_t dwHash, uint16_t wCounterID,
                                     bool bEnabled)
{
    auto it = m_state.find(MakeKey(dwHash, wCounterID));
    if (it != m_state.end())
        it->second.bEnabled = bEnabled;
}
