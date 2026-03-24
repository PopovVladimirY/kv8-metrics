// kv8scope -- Kv8 Software Oscilloscope
// SessionListPanel.cpp -- Session tree with table, context menu, and popups.

#include "SessionListPanel.h"
#include "ConfigStore.h"
#include "SessionManager.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string SessionListPanel::ParseStartTime(const std::string& sSessionID)
{
    // Session ID format: YYYYMMDDTHHMMSSZ-PPPP-RRRR  (UTC time)
    // Convert to local time for display so it matches the graph X-axis labels.
    if (sSessionID.size() < 15)
        return sSessionID;

    const std::string dt = sSessionID.substr(0, 15); // "YYYYMMDDTHHMMSS"
    if (dt.size() < 15 || dt[8] != 'T')
        return sSessionID;

    // Parse the UTC fields.
    struct tm utc = {};
    utc.tm_year = std::stoi(dt.substr(0, 4)) - 1900;
    utc.tm_mon  = std::stoi(dt.substr(4, 2)) - 1;
    utc.tm_mday = std::stoi(dt.substr(6, 2));
    utc.tm_hour = std::stoi(dt.substr(9, 2));
    utc.tm_min  = std::stoi(dt.substr(11, 2));
    utc.tm_sec  = std::stoi(dt.substr(13, 2));

    // Convert UTC struct tm -> time_t.
#ifdef _WIN32
    const time_t t = _mkgmtime(&utc);
#else
    const time_t t = timegm(&utc);
#endif
    if (t == static_cast<time_t>(-1))
        return sSessionID;

    // Convert time_t -> local struct tm.
    struct tm local = {};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif

    char buf[80];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
    return std::string(buf);
}

int SessionListPanel::CountCounters(const kv8::SessionMeta& meta)
{
    int total = 0;
    for (const auto& kv : meta.hashToCounters)
        total += static_cast<int>(kv.second.size());
    return total;
}

std::string SessionListPanel::HumanChannel(const std::string& sChannel)
{
    // Kafka-sanitized channel names have dots instead of slashes.
    // Convert back to the original hierarchical form for readability.
    // e.g. "kv8.test_channel" -> "kv8/test_channel"
    std::string out = sChannel;
    for (char& c : out) { if (c == '.') c = '/'; }
    return out;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SessionListPanel::SessionListPanel() = default;

// ---------------------------------------------------------------------------
// Data mutation
// ---------------------------------------------------------------------------

void SessionListPanel::AddSession(const std::string& sChannel,
                                  const std::string& sPrefix,
                                  const kv8::SessionMeta& meta)
{
    SessionEntry entry;
    entry.sChannel = sChannel;
    entry.sPrefix  = sPrefix;
    entry.meta     = meta;
    entry.eLiveness = SessionLiveness::Unknown;
    m_sessions.push_back(std::move(entry));

    // Keep sorted by channel then prefix.
    std::sort(m_sessions.begin(), m_sessions.end(),
        [](const SessionEntry& a, const SessionEntry& b)
        {
            if (a.sChannel != b.sChannel) return a.sChannel < b.sChannel;
            return a.sPrefix < b.sPrefix;
        });
}

void SessionListPanel::RemoveSession(const std::string& sPrefix)
{
    auto it = std::remove_if(m_sessions.begin(), m_sessions.end(),
        [&](const SessionEntry& se) { return se.sPrefix == sPrefix; });
    m_sessions.erase(it, m_sessions.end());
}

void SessionListPanel::UpdateSessionMeta(const std::string& sPrefix,
                                          const kv8::SessionMeta& meta)
{
    for (auto& se : m_sessions)
    {
        if (se.sPrefix == sPrefix)
        {
            se.meta = meta;
            return;
        }
    }
}

void SessionListPanel::SetSessionLiveness(const std::string& sPrefix,
                                           SessionLiveness eLiveness)
{
    for (auto& se : m_sessions)
    {
        if (se.sPrefix == sPrefix)
        {
            se.eLiveness = eLiveness;
            return;
        }
    }
}

void SessionListPanel::SetSessionOnline(const std::string& sPrefix, bool bOnline)
{
    SetSessionLiveness(sPrefix,
        bOnline ? SessionLiveness::Live : SessionLiveness::Offline);
}

void SessionListPanel::Clear()
{
    m_sessions.clear();
}

bool SessionListPanel::ConsumeOpenRequest(std::string& outPrefix,
                                          kv8::SessionMeta& outMeta,
                                          SessionLiveness& outLiveness)
{
    for (auto& se : m_sessions)
    {
        if (se.bOpenRequested)
        {
            se.bOpenRequested = false;
            outPrefix   = se.sPrefix;
            outMeta     = se.meta;
            outLiveness = se.eLiveness;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void SessionListPanel::Render()
{
    ImGui::SetNextWindowSize(ImVec2(620.0f, 400.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Sessions"))
    {
        if (m_sessions.empty())
        {
            ImGui::TextDisabled("%s", m_sConnHint.c_str());
        }
        else
        {
            // ---- Table with columns ----------------------------------------
            const ImGuiTableFlags tableFlags =
                ImGuiTableFlags_Resizable      |
                ImGuiTableFlags_Reorderable    |
                ImGuiTableFlags_Hideable       |
                ImGuiTableFlags_RowBg          |
                ImGuiTableFlags_BordersInnerV  |
                ImGuiTableFlags_ScrollY;

            if (ImGui::BeginTable("##SessionTable", 7, tableFlags))
            {
                ImGui::TableSetupScrollFreeze(0, 1); // freeze header row

                // Size fixed columns from actual text so they scale with font.
                const float fPad      = ImGui::GetStyle().CellPadding.x * 2.0f + 16.0f;
                const float fStatusW  = ImGui::GetTextLineHeight() + fPad; // fits dot indicator
                const float fColSessID = ImGui::CalcTextSize("20260228T172134Z-8001-XXXXXXXXXXX").x + fPad;
                const float fColName  = ImGui::CalcTextSize("MMMMMMMMMMMMMMMMMM").x + fPad;
                const float fColCount = ImGui::CalcTextSize("Counters").x + fPad;
                const float fColGrp   = ImGui::CalcTextSize("Groups").x  + fPad;
                const float fColDate  = ImGui::CalcTextSize("2026-02-28 23:59").x + fPad;

                // Prefer saved column widths from config (cross-session persistence).
                // Fall back to the text-derived natural widths computed above.
                // Using init_width_or_weight in TableSetupColumn is safe on every
                // frame including the very first one, unlike TableSetColumnWidth()
                // which requires MinColumnWidth > 0 (only available after layout).
                static constexpr int k_nSessionCols = 7;
                const float aDefaultW[k_nSessionCols] = {
                    fStatusW, fColSessID, fColName,
                    fColCount, fColGrp, fColDate, 0.0f
                };
                float aSavedW[k_nSessionCols] = {};
                if (m_pConfig)
                {
                    const auto& tw = m_pConfig->Get().tableColumnWidths;
                    auto twit = tw.find("##SessionTable");
                    if (twit != tw.end() &&
                        static_cast<int>(twit->second.size()) == k_nSessionCols)
                        for (int k = 0; k < k_nSessionCols; ++k)
                            aSavedW[k] = twit->second[k];
                }
                auto colW = [&](int k) -> float {
                    return (aSavedW[k] > 0.0f) ? aSavedW[k] : aDefaultW[k];
                };

                ImGui::TableSetupColumn("##Status",
                    ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_NoResize, colW(0));
                ImGui::TableSetupColumn("Session ID",
                    ImGuiTableColumnFlags_WidthFixed, colW(1));
                ImGui::TableSetupColumn("Name",
                    ImGuiTableColumnFlags_WidthFixed, colW(2));
                ImGui::TableSetupColumn("Counters",
                    ImGuiTableColumnFlags_WidthFixed, colW(3));
                ImGui::TableSetupColumn("Groups",
                    ImGuiTableColumnFlags_WidthFixed, colW(4));
                ImGui::TableSetupColumn("Started",
                    ImGuiTableColumnFlags_WidthFixed, colW(5));
                ImGui::TableSetupColumn("##pad",
                    ImGuiTableColumnFlags_WidthStretch); // absorbs leftover space

                ImGui::TableHeadersRow();

                // Render rows grouped by channel via TreeNode.
                std::string sPrevChannel;
                bool bTreeOpen = false;

                for (size_t i = 0; i < m_sessions.size(); ++i)
                {
                    const auto& se = m_sessions[i];

                    // --- Channel group header ---
                    if (se.sChannel != sPrevChannel)
                    {
                        // Close previous channel tree node if open.
                        if (bTreeOpen)
                            ImGui::TreePop();

                        sPrevChannel = se.sChannel;

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); // Status (empty for channel row)
                        ImGui::TableNextColumn(); // Session ID -- tree node here

                        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                        bTreeOpen = ImGui::TreeNodeEx(
                            HumanChannel(se.sChannel).c_str(),
                            ImGuiTreeNodeFlags_SpanAllColumns |
                            ImGuiTreeNodeFlags_DefaultOpen);

                        if (!bTreeOpen)
                        {
                            // Channel collapsed -- skip all sessions in it.
                            // Advance i past remaining sessions of this channel.
                            while (i + 1 < m_sessions.size() &&
                                   m_sessions[i + 1].sChannel == se.sChannel)
                                ++i;

                            // Render context menu on channel row.
                            if (ImGui::BeginPopupContextItem())
                            {
                                if (ImGui::MenuItem("Delete channel..."))
                                {
                                    m_deleteChannelName   = se.sChannel;
                                    m_bShowDeleteChannel  = true;
                                }
                                ImGui::EndPopup();
                            }
                            continue;
                        }

                        // Context menu on channel header when open.
                        if (ImGui::BeginPopupContextItem())
                        {
                            if (ImGui::MenuItem("Delete channel..."))
                            {
                                m_deleteChannelName  = se.sChannel;
                                m_bShowDeleteChannel = true;
                            }
                            ImGui::EndPopup();
                        }
                    }

                    if (!bTreeOpen)
                        continue;

                    // --- Session row ---
                    RenderSessionRow(i);
                }

                // Close the last open tree node.
                if (bTreeOpen)
                    ImGui::TreePop();

                // Persist column widths when the user resizes.
                if (m_pConfig)
                {
                    ImGuiTable* pT  = ImGui::GetCurrentTable();
                    auto& tw    = m_pConfig->GetMut().tableColumnWidths;
                    auto& saved = tw["##SessionTable"];
                    if (static_cast<int>(saved.size()) != k_nSessionCols)
                        saved.assign(k_nSessionCols, 0.0f);
                    bool bW = false;
                    if (pT)
                        for (int k = 0; k < k_nSessionCols; ++k)
                        {
                            const float w = pT->Columns[k].WidthGiven;
                            if (w > 0.0f && std::fabs(w - saved[k]) > 0.5f)
                            { saved[k] = w; bW = true; }
                        }
                    if (bW) m_pConfig->MarkDirty();
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();

    // Render modal popups outside the window block.
    if (m_bShowInspect)
        RenderInspectPopup();
    if (m_bShowDeleteSession)
        RenderDeleteSessionPopup();
    if (m_bShowDeleteChannel)
        RenderDeleteChannelPopup();
}

// ---------------------------------------------------------------------------
// RenderSessionRow -- one session inside the table
// ---------------------------------------------------------------------------

void SessionListPanel::RenderSessionRow(size_t idx)
{
    auto& se = m_sessions[idx];

    ImGui::TableNextRow();
    ImGui::PushID(static_cast<int>(idx));

    // Column 0: Status dot -- color and shape reflect liveness state
    ImGui::TableNextColumn();
    {
        ImVec2 pos    = ImGui::GetCursorScreenPos();
        const float fLineH = ImGui::GetTextLineHeight();
        float cy           = pos.y + fLineH * 0.5f;
        const float fDotR  = fLineH * 0.28f;
        const float cx     = pos.x + fLineH * 0.5f;

        const ImVec4 colLive        = m_pThemeColors
            ? m_pThemeColors->onlineBadge
            : ImVec4(0.290f, 0.871f, 0.251f, 1.0f); // #4ade40 bright grass green
        const ImVec4 colOffline     = m_pThemeColors
            ? m_pThemeColors->offlineBadge
            : ImVec4(0.431f, 0.463f, 0.506f, 1.0f); // #6e7681 Night Sky grey
        const ImVec4 colGoingOffline = ImVec4(1.0f, 0.647f, 0.0f, 1.0f); // amber

        auto col32 = [](ImVec4 v) { return ImGui::ColorConvertFloat4ToU32(v); };

        switch (se.eLiveness)
        {
        case SessionLiveness::Live:
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(cx, cy), fDotR, col32(colLive));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("LIVE");
            break;

        case SessionLiveness::GoingOffline:
        {
            // Pulsing amber dot: pulse between 0.4 and 1.0 alpha at ~1 Hz.
            float pulse = 0.4f + 0.6f * (0.5f + 0.5f * sinf(ImGui::GetTime() * 6.28f));
            ImVec4 amber = colGoingOffline;
            amber.w = pulse;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(cx, cy), fDotR, col32(amber));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("LIVE?");
            break;
        }

        case SessionLiveness::Offline:
            // Hollow circle: session offline.
            ImGui::GetWindowDrawList()->AddCircle(
                ImVec2(cx, cy), fDotR, col32(colOffline), 0, 1.5f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("OFFLINE");
            break;

        case SessionLiveness::Historical:
            // Hollow circle: session tombstoned.
            ImGui::GetWindowDrawList()->AddCircle(
                ImVec2(cx, cy), fDotR, col32(colOffline), 0, 1.5f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("HISTORY");
            break;

        default: // Unknown
            // Small grey dot while first probe is running.
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(cx, cy), fDotR * 0.6f, col32(colOffline));
            break;
        }

        ImGui::Dummy(ImVec2(1.0f, fLineH));
    }

    // Column 1: Session ID (selectable + double-click)
    ImGui::TableNextColumn();
    {
        const char* pID = se.meta.sSessionID.empty()
            ? se.sPrefix.c_str()
            : se.meta.sSessionID.c_str();

        ImGuiSelectableFlags selFlags =
            ImGuiSelectableFlags_SpanAllColumns |
            ImGuiSelectableFlags_AllowDoubleClick;

        if (ImGui::Selectable(pID, se.bSelected, selFlags))
        {
            const bool bCtrl  = ImGui::GetIO().KeyCtrl;
            const bool bShift = ImGui::GetIO().KeyShift;
            const bool bDbl   = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

            if (bDbl)
            {
                // Double-click: clear selection, select this, open.
                for (auto& s : m_sessions) s.bSelected = false;
                se.bSelected      = true;
                m_lastClickedIdx  = idx;
                se.bOpenRequested = true;
            }
            else if (bCtrl)
            {
                // Ctrl+click: toggle this row without changing others.
                se.bSelected     = !se.bSelected;
                m_lastClickedIdx = idx;
            }
            else if (bShift && m_lastClickedIdx < m_sessions.size())
            {
                // Shift+click: extend selection from anchor to here.
                const size_t lo = std::min(idx, m_lastClickedIdx);
                const size_t hi = std::max(idx, m_lastClickedIdx);
                for (size_t k = 0; k < m_sessions.size(); ++k)
                    m_sessions[k].bSelected = (k >= lo && k <= hi);
            }
            else
            {
                // Plain click: clear all, select this.
                for (auto& s : m_sessions) s.bSelected = false;
                se.bSelected     = true;
                m_lastClickedIdx = idx;
            }
        }

        // Context menu on session row.
        RenderContextMenu(idx);
    }

    // Column 2: Name
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(se.meta.sName.c_str());

    // Column 3: Counter count
    ImGui::TableNextColumn();
    ImGui::Text("%d", CountCounters(se.meta));

    // Column 4: Group count
    ImGui::TableNextColumn();
    ImGui::Text("%d", static_cast<int>(se.meta.hashToGroup.size()));

    // Column 5: Started
    ImGui::TableNextColumn();
    {
        std::string sStarted = ParseStartTime(se.meta.sSessionID);
        ImGui::TextUnformatted(sStarted.c_str());
    }

    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Context menu (right-click on a session row)
// ---------------------------------------------------------------------------

void SessionListPanel::RenderContextMenu(size_t idx)
{
    if (!ImGui::BeginPopupContextItem())
        return;

    if (ImGui::MenuItem("Open"))
    {
        m_sessions[idx].bOpenRequested = true;
    }

    if (ImGui::MenuItem("Inspect details..."))
    {
        m_inspectIdx   = idx;
        m_bShowInspect = true;
    }

    ImGui::Separator();

    // Count selected sessions to decide between single and multi-delete.
    int nSel = 0;
    for (const auto& s : m_sessions) if (s.bSelected) ++nSel;
    const bool bThisSel = m_sessions[idx].bSelected;

    if (nSel > 1 && bThisSel)
    {
        char szLabel[64];
        snprintf(szLabel, sizeof(szLabel), "Delete %d sessions...", nSel);
        if (ImGui::MenuItem(szLabel))
        {
            m_deleteSessionPrefixes.clear();
            for (const auto& s : m_sessions)
                if (s.bSelected) m_deleteSessionPrefixes.push_back(s.sPrefix);
            m_bShowDeleteSession = true;
        }
    }
    else
    {
        if (ImGui::MenuItem("Delete session..."))
        {
            m_deleteSessionPrefixes.clear();
            m_deleteSessionPrefixes.push_back(m_sessions[idx].sPrefix);
            m_bShowDeleteSession = true;
        }
    }

    if (ImGui::MenuItem("Delete channel..."))
    {
        m_deleteChannelName  = m_sessions[idx].sChannel;
        m_bShowDeleteChannel = true;
    }

    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Inspect details popup
// ---------------------------------------------------------------------------

void SessionListPanel::RenderInspectPopup()
{
    ImGui::OpenPopup("Session Details");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Session Details", &m_bShowInspect,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_inspectIdx < m_sessions.size())
        {
            const auto& se = m_sessions[m_inspectIdx];
            const auto& meta = se.meta;

            ImGui::Text("Channel:        %s", HumanChannel(se.sChannel).c_str());
            ImGui::Text("Prefix:         %s", se.sPrefix.c_str());
            ImGui::Text("Session ID:     %s", meta.sSessionID.c_str());
            ImGui::Text("Name:           %s", meta.sName.c_str());
            ImGui::Text("Log topic:      %s", meta.sLogTopic.c_str());
            ImGui::Text("Control topic:  %s", meta.sControlTopic.c_str());
            ImGui::Text("Data topics:    %d", static_cast<int>(meta.dataTopics.size()));
            ImGui::Text("Groups:         %d", static_cast<int>(meta.hashToGroup.size()));
            ImGui::Text("Counters:       %d", CountCounters(meta));
            ImGui::Text("Online:         %s",
                        (se.eLiveness == SessionLiveness::Live ||
                         se.eLiveness == SessionLiveness::GoingOffline) ? "Yes" : "No");

            ImGui::Separator();
            ImGui::Text("Started:        %s",
                         ParseStartTime(meta.sSessionID).c_str());

            // Groups detail
            if (!meta.hashToGroup.empty())
            {
                ImGui::Separator();
                if (ImGui::TreeNodeEx("Groups", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    for (const auto& kv : meta.hashToGroup)
                    {
                        ImGui::BulletText("0x%08X  %s", kv.first, kv.second.c_str());

                        // Show counters in this group.
                        auto it = meta.hashToCounters.find(kv.first);
                        if (it != meta.hashToCounters.end())
                        {
                            ImGui::Indent();
                            for (const auto& cm : it->second)
                            {
                                ImGui::Text("  [%u] %s  (%.1f .. %.1f)",
                                            cm.wCounterID,
                                            cm.sName.c_str(),
                                            cm.dbMin, cm.dbMax);
                            }
                            ImGui::Unindent();
                        }
                    }
                    ImGui::TreePop();
                }
            }

            // Data topics list
            if (!meta.dataTopics.empty())
            {
                ImGui::Separator();
                if (ImGui::TreeNode("Data topics"))
                {
                    for (const auto& t : meta.dataTopics)
                        ImGui::BulletText("%s", t.c_str());
                    ImGui::TreePop();
                }
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
        {
            m_bShowInspect = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Delete session confirmation popup
// ---------------------------------------------------------------------------

void SessionListPanel::RenderDeleteSessionPopup()
{
    ImGui::OpenPopup("Delete Session?");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Delete Session?", &m_bShowDeleteSession,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        const size_t n = m_deleteSessionPrefixes.size();
        if (n == 1)
        {
            ImGui::Text("Permanently delete session:");
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f),
                               "  %s", m_deleteSessionPrefixes[0].c_str());
        }
        else
        {
            ImGui::Text("Permanently delete %zu session(s):", n);
            for (const auto& p : m_deleteSessionPrefixes)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f),
                                   "  %s", p.c_str());
        }
        ImGui::Text("This will remove all Kafka topics for these sessions.");
        ImGui::Text("This action cannot be undone.");

        ImGui::Spacing();

        if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f)))
        {
            if (m_pSessionMgr)
            {
                for (const auto& sPrefix : m_deleteSessionPrefixes)
                {
                    for (const auto& se : m_sessions)
                    {
                        if (se.sPrefix == sPrefix)
                        {
                            m_pSessionMgr->DeleteSession(se.sChannel, se.meta);
                            break;
                        }
                    }
                    RemoveSession(sPrefix);
                }
            }
            m_deleteSessionPrefixes.clear();
            m_bShowDeleteSession = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            m_deleteSessionPrefixes.clear();
            m_bShowDeleteSession = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Delete channel confirmation popup (double confirmation)
// ---------------------------------------------------------------------------

void SessionListPanel::RenderDeleteChannelPopup()
{
    ImGui::OpenPopup("Delete Channel?");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Delete Channel?", &m_bShowDeleteChannel,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Permanently delete ALL sessions in channel:");
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                           "  %s", m_deleteChannelName.c_str());
        ImGui::Spacing();
        ImGui::Text("This will remove every Kafka topic starting with");
        ImGui::Text("\"%s.\" including the registry.", m_deleteChannelName.c_str());
        ImGui::Text("This action cannot be undone.");

        ImGui::Spacing();

        if (ImGui::Button("Delete Channel", ImVec2(140.0f, 0.0f)))
        {
            if (m_pSessionMgr)
            {
                m_pSessionMgr->DeleteChannel(m_deleteChannelName);

                // Remove all sessions belonging to this channel.
                auto it = std::remove_if(m_sessions.begin(), m_sessions.end(),
                    [&](const SessionEntry& se)
                    { return se.sChannel == m_deleteChannelName; });
                m_sessions.erase(it, m_sessions.end());
            }
            m_bShowDeleteChannel = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            m_bShowDeleteChannel = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

