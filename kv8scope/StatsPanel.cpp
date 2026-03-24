// kv8scope -- Kv8 Software Oscilloscope
// StatsPanel.cpp -- Statistics panel (P4.4) implementation.

#include "StatsPanel.h"
#include "WaveformRenderer.h"
#include "StatsEngine.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

// ---------------------------------------------------------------------------
// Formatting helpers (local -- mirrors CounterTree helpers)
// ---------------------------------------------------------------------------

static void SpFmtCount(uint64_t n, char* buf, int sz)
{
    if (n == 0)
        std::snprintf(buf, static_cast<size_t>(sz), "--");
    else if (n < 1000u)
        std::snprintf(buf, static_cast<size_t>(sz), "%llu",
                      static_cast<unsigned long long>(n));
    else if (n < 1000000u)
        std::snprintf(buf, static_cast<size_t>(sz), "%.1fK",
                      static_cast<double>(n) / 1000.0);
    else
        std::snprintf(buf, static_cast<size_t>(sz), "%.2fM",
                      static_cast<double>(n) / 1000000.0);
}

static void SpFmtVal(double v, char* buf, int sz)
{
    std::snprintf(buf, static_cast<size_t>(sz), "%.3g", v);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void StatsPanel::Init(const kv8::SessionMeta& meta)
{
    m_rows.clear();
    for (const auto& [dwHash, sGroupName] : meta.hashToGroup)
    {
        auto cit = meta.hashToCounters.find(dwHash);
        if (cit == meta.hashToCounters.end()) continue;
        for (const auto& cm : cit->second)
        {
            if (cm.bIsUdtFeed) continue;  // skip raw feed descriptor; virtual scalars only
            CounterRow row;
            row.dwHash     = dwHash;
            row.wID        = cm.wCounterID;
            row.sGroupName = sGroupName;
            row.sName      = cm.sName;
            m_rows.push_back(std::move(row));
        }
    }
}

// ---------------------------------------------------------------------------
// AddCounters -- incremental (Fix 2 -- KV8_UDT_STALL)
// ---------------------------------------------------------------------------

void StatsPanel::AddCounters(const kv8::SessionMeta& meta)
{
    for (const auto& [dwHash, sGroupName] : meta.hashToGroup)
    {
        auto cit = meta.hashToCounters.find(dwHash);
        if (cit == meta.hashToCounters.end()) continue;
        for (const auto& cm : cit->second)
        {
            if (cm.bIsUdtFeed) continue;

            // Check if this counter is already in m_rows.
            bool bFound = false;
            for (const auto& r : m_rows)
            {
                if (r.dwHash == dwHash && r.wID == cm.wCounterID)
                {
                    bFound = true;
                    break;
                }
            }
            if (bFound) continue;

            CounterRow row;
            row.dwHash     = dwHash;
            row.wID        = cm.wCounterID;
            row.sGroupName = sGroupName;
            row.sName      = cm.sName;
            m_rows.push_back(std::move(row));
        }
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void StatsPanel::Render(WaveformRenderer* pWaveform, StatsEngine* pStats)
{
    if (!m_bVisible)
        return;

    ImGui::SetNextWindowSize(ImVec2(900.0f, 280.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Statistics##StatsPanel", &m_bVisible))
    {
        ImGui::End();
        return;
    }

    // ---- Filter text box ------------------------------------------------
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Filter##SPFilter", m_szFilter, sizeof(m_szFilter));
    ImGui::SameLine();
    ImGui::TextDisabled("(filter by counter name)");

    // ---- Capture visible X limits ---------------------------------------
    double dXMin = 0.0, dXMax = 0.0;
    bool bHasWindow = false;
    if (pWaveform && pWaveform->HasData())
    {
        pWaveform->GetVisibleXLimits(dXMin, dXMax);
        bHasWindow = (dXMax > dXMin);
    }

    // ---- Table ----------------------------------------------------------
    static const ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_BordersOuter  |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_RowBg         |
        ImGuiTableFlags_ScrollY       |
        ImGuiTableFlags_Sortable      |
        ImGuiTableFlags_Resizable     |
        ImGuiTableFlags_SizingFixedFit;

    // Columns:
    //  0  Group       stretch ~15% of remaining space (resizable)
    //  1  Counter     stretch ~85% of remaining space (resizable)
    //  2  N (total)   fixed px
    //  3  N (vis)     fixed px
    //  4  Vmin        fixed px
    //  5  Vmax        fixed px
    //  6  Vavg        fixed px
    //  7  Vmed        fixed px
    //  8  Min(full)   fixed px
    //  9  Max(full)   fixed px
    // 10  Avg(full)   fixed px

    // Tighter row height: preserve horizontal margin but reduce vertical padding.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                        ImVec2(ImGui::GetStyle().CellPadding.x, 1.0f));
    // ID is versioned so stale ini entries (wrong IsStretch flag or fixed/stretch
    // type mismatch) are not loaded on upgrade.  Use _v3 after switching Group
    // and Counter from WidthFixed/WidthStretch to both WidthStretch so ini
    // stores ratio weights -- no type mismatch possible on future upgrades.
    if (!ImGui::BeginTable("##SPTable_v3", 11, kTableFlags))
    {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);

    const float fPad      = ImGui::GetStyle().CellPadding.x * 2.0f + 4.0f;
    const float fColCount = ImGui::CalcTextSize("12.34M").x  + fPad;
    const float fColStat  = ImGui::CalcTextSize("-12345.6").x + fPad;

    ImGui::TableSetupColumn("Group",    ImGuiTableColumnFlags_WidthStretch, 15.0f);
    ImGui::TableSetupColumn("Counter",  ImGuiTableColumnFlags_WidthStretch, 85.0f);
    ImGui::TableSetupColumn("N(all)",   ImGuiTableColumnFlags_WidthFixed, fColCount);
    ImGui::TableSetupColumn("N(vis)",   ImGuiTableColumnFlags_WidthFixed, fColCount);
    ImGui::TableSetupColumn("Vmin",     ImGuiTableColumnFlags_WidthFixed, fColStat);
    ImGui::TableSetupColumn("Vmax",     ImGuiTableColumnFlags_WidthFixed, fColStat);
    ImGui::TableSetupColumn("Vavg",     ImGuiTableColumnFlags_WidthFixed, fColStat);
    ImGui::TableSetupColumn("Vmed",     ImGuiTableColumnFlags_WidthFixed, fColStat);
    ImGui::TableSetupColumn("Min",      ImGuiTableColumnFlags_WidthFixed, fColStat);
    ImGui::TableSetupColumn("Max",      ImGuiTableColumnFlags_WidthFixed, fColStat);
    ImGui::TableSetupColumn("Avg",      ImGuiTableColumnFlags_WidthFixed, fColStat);
    ImGui::TableHeadersRow();

    // ---- Sort spec ------------------------------------------------------
    if (ImGuiTableSortSpecs* pSortSpecs = ImGui::TableGetSortSpecs())
    {
        if (pSortSpecs->SpecsDirty && pSortSpecs->SpecsCount > 0)
        {
            m_iSortCol = static_cast<int>(pSortSpecs->Specs[0].ColumnIndex);
            m_bSortAsc = (pSortSpecs->Specs[0].SortDirection ==
                          ImGuiSortDirection_Ascending);
            pSortSpecs->SpecsDirty = false;
        }
    }

    // ---- Build filtered & sorted display list ---------------------------
    // Collect indices into m_rows that pass the filter.
    const char* pFilter = m_szFilter;
    const bool  bFilter = (pFilter[0] != '\0');

    std::vector<int> indices;
    indices.reserve(m_rows.size());
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
    {
        if (bFilter)
        {
            const std::string& sN = m_rows[i].sName;
            // Case-insensitive substring search via strstr on lowercase copy.
            // Simple approach: use strstr directly (case-sensitive).
            if (std::strstr(sN.c_str(), pFilter) == nullptr &&
                std::strstr(m_rows[i].sGroupName.c_str(), pFilter) == nullptr)
                continue;
        }
        indices.push_back(i);
    }

    // Sort if a column was clicked.
    if (m_iSortCol >= 0 && !indices.empty())
    {
        // Pre-fetch stats for sorting to avoid redundant queries.
        // For large sessions this is acceptable at 15 Hz.
        struct SortKey { double v; uint64_t n; };
        std::vector<SortKey> sortKeys(m_rows.size(), {0.0, 0});

        if (pWaveform && pStats)
        {
            for (int idx : indices)
            {
                const CounterRow& r = m_rows[idx];
                uint64_t nFull = 0; double dMin = 0.0, dMax = 0.0, dAvg = 0.0;
                pStats->GetFullStats(r.dwHash, r.wID, nFull, dMin, dMax, dAvg);
                WaveformRenderer::CounterStats cs;
                if (bHasWindow)
                    cs = pWaveform->QueryStats(r.dwHash, r.wID, dXMin, dXMax);
                switch (m_iSortCol)
                {
                case 2: sortKeys[idx] = {static_cast<double>(nFull), nFull}; break;
                case 3: sortKeys[idx] = {static_cast<double>(cs.nWindow), 0}; break;
                case 4: sortKeys[idx] = {cs.dMin, 0};  break;
                case 5: sortKeys[idx] = {cs.dMax, 0};  break;
                case 6: sortKeys[idx] = {cs.dAvg, 0};  break;
                case 7: sortKeys[idx] = {cs.dMedian, 0}; break;
                case 8: sortKeys[idx] = {dMin, 0};  break;
                case 9: sortKeys[idx] = {dMax, 0};  break;
                case 10: sortKeys[idx] = {dAvg, 0}; break;
                default: break;
                }
            }
        }

        if (m_iSortCol < 2)
        {
            // Sort by text columns (Group or Counter).
            std::sort(indices.begin(), indices.end(),
                      [&](int a, int b)
            {
                const std::string& sa = (m_iSortCol == 0)
                    ? m_rows[a].sGroupName : m_rows[a].sName;
                const std::string& sb = (m_iSortCol == 0)
                    ? m_rows[b].sGroupName : m_rows[b].sName;
                return m_bSortAsc ? (sa < sb) : (sa > sb);
            });
        }
        else
        {
            std::sort(indices.begin(), indices.end(),
                      [&](int a, int b)
            {
                double va = sortKeys[a].v;
                double vb = sortKeys[b].v;
                return m_bSortAsc ? (va < vb) : (va > vb);
            });
        }
    }

    // ---- Rows -----------------------------------------------------------
    for (int idx : indices)
    {
        const CounterRow& r = m_rows[idx];

        // Full-duration stats from StatsEngine.
        uint64_t nFull = 0;
        double   dFMin = 0.0, dFMax = 0.0, dFAvg = 0.0;
        bool     bFull = false;
        if (pStats)
            bFull = pStats->GetFullStats(r.dwHash, r.wID,
                                         nFull, dFMin, dFMax, dFAvg);

        // Visible-window stats from WaveformRenderer.
        WaveformRenderer::CounterStats cs;
        if (pWaveform && bHasWindow)
            cs = pWaveform->QueryStats(r.dwHash, r.wID, dXMin, dXMax);

        ImGui::TableNextRow();

        // Group
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(r.sGroupName.c_str());

        // Counter
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(r.sName.c_str());

        // N(all) -- full-duration total
        char szBuf[32];
        ImGui::TableSetColumnIndex(2);
        SpFmtCount(static_cast<uint64_t>(cs.nTotal), szBuf, sizeof(szBuf));
        ImGui::TextUnformatted(szBuf);

        // N(vis) -- visible-window sample count
        ImGui::TableSetColumnIndex(3);
        SpFmtCount(static_cast<uint64_t>(cs.nWindow), szBuf, sizeof(szBuf));
        ImGui::TextUnformatted(szBuf);

        if (cs.bHasWindowData)
        {
            ImGui::TableSetColumnIndex(4);
            SpFmtVal(cs.dMin, szBuf, sizeof(szBuf));
            ImGui::TextUnformatted(szBuf);

            ImGui::TableSetColumnIndex(5);
            SpFmtVal(cs.dMax, szBuf, sizeof(szBuf));
            ImGui::TextUnformatted(szBuf);

            ImGui::TableSetColumnIndex(6);
            SpFmtVal(cs.dAvg, szBuf, sizeof(szBuf));
            ImGui::TextUnformatted(szBuf);

            ImGui::TableSetColumnIndex(7);
            SpFmtVal(cs.dMedian, szBuf, sizeof(szBuf));
            ImGui::TextUnformatted(szBuf);
        }
        else
        {
            for (int col = 4; col <= 7; ++col)
            {
                ImGui::TableSetColumnIndex(col);
                ImGui::TextDisabled("--");
            }
        }

        if (bFull)
        {
            ImGui::TableSetColumnIndex(8);
            SpFmtVal(dFMin, szBuf, sizeof(szBuf));
            ImGui::TextUnformatted(szBuf);

            ImGui::TableSetColumnIndex(9);
            SpFmtVal(dFMax, szBuf, sizeof(szBuf));
            ImGui::TextUnformatted(szBuf);

            ImGui::TableSetColumnIndex(10);
            SpFmtVal(dFAvg, szBuf, sizeof(szBuf));
            ImGui::TextUnformatted(szBuf);
        }
        else
        {
            for (int col = 8; col <= 10; ++col)
            {
                ImGui::TableSetColumnIndex(col);
                ImGui::TextDisabled("--");
            }
        }
    }

    ImGui::EndTable();
    ImGui::PopStyleVar();  // ImGuiStyleVar_CellPadding
    ImGui::End();
}
