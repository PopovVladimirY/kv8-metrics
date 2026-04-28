// kv8scope -- Kv8 Software Oscilloscope
// LogPanel.cpp -- Dockable trace-log viewer (Phase L4).

#include "LogPanel.h"

#include "LogStore.h"
#include "WaveformRenderer.h"

#include <kv8/Kv8Types.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

// ── Severity colour palette ─────────────────────────────────────────────────
//
// Colours match the design proposal (Phase L4.4): WARN amber, ERROR
// orange-red, FATAL magenta.  DEBUG and INFO are kept low-contrast so they
// don't dominate the table when other levels are present.
ImVec4 LogPanel::LevelColor(int level)
{
    switch (level)
    {
        case 0: return ImVec4(0.55f, 0.55f, 0.60f, 1.0f);  // DEBUG -- gray
        case 1: return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);  // INFO  -- white
        case 2: return ImVec4(1.00f, 0.72f, 0.00f, 1.0f);  // WARN  -- amber
        case 3: return ImVec4(1.00f, 0.27f, 0.13f, 1.0f);  // ERROR -- orange-red
        case 4: return ImVec4(1.00f, 0.00f, 1.00f, 1.0f);  // FATAL -- magenta
        default:return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
    }
}

// Subtle severity-based row background tint.  Uses the same hue as the
// foreground severity colour but at low alpha so the row reads as
// distinctly coloured without overpowering the text.  DEBUG/INFO are
// transparent (zero alpha) -- they're the common case and shouldn't add
// any visual weight to the table.
ImVec4 LogPanel::LevelRowBgColor(int level)
{
    switch (level)
    {
        case 2: return ImVec4(1.00f, 0.72f, 0.00f, 0.12f);  // WARN  -- faint amber
        case 3: return ImVec4(1.00f, 0.27f, 0.13f, 0.18f);  // ERROR -- soft orange-red
        case 4: return ImVec4(1.00f, 0.00f, 1.00f, 0.22f);  // FATAL -- magenta wash
        default:return ImVec4(0.0f,  0.0f,  0.0f,  0.0f);   // DEBUG/INFO -- none
    }
}

const char* LogPanel::LevelLabel(int level)
{
    switch (level)
    {
        case 0: return "DEBUG";
        case 1: return "INFO ";
        case 2: return "WARN ";
        case 3: return "ERROR";
        case 4: return "FATAL";
        default:return "?????";
    }
}

// Format a Unix-epoch ns timestamp as ISO 8601 with microsecond precision
// (UTC).  The buffer is owned by the caller; sz must be >= 32.
static void FormatWallNs(uint64_t qwWallNs, char* buf, size_t sz)
{
    const uint64_t qwSec = qwWallNs / 1000000000ULL;
    const uint64_t qwUs  = (qwWallNs % 1000000000ULL) / 1000ULL;
    std::time_t t = static_cast<std::time_t>(qwSec);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::snprintf(buf, sz, "%04d-%02d-%02d %02d:%02d:%02d.%06llu",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<unsigned long long>(qwUs));
}

void LogPanel::Render(WaveformRenderer* pWaveform)
{
    if (!m_bVisible)  return;
    if (!m_pStore)    return;

    // Reasonable default size/position so the window is visible the very
    // first time it pops up.  Persisted across sessions via imgui.ini.
    // The "##" title disambiguator is the panel's instance address so
    // multiple sessions don't collide on a shared "Trace Log" window.
    ImGui::SetNextWindowSize(ImVec2(960.0f, 320.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80.0f, 560.0f),  ImGuiCond_FirstUseEver);

    char szTitle[64];
    std::snprintf(szTitle, sizeof(szTitle),
                  "Trace Log##LogPanel_%p",
                  static_cast<const void*>(this));

    if (!ImGui::Begin(szTitle, &m_bVisible))
    {
        ImGui::End();
        return;
    }

    // ── Toolbar ────────────────────────────────────────────────────────────
    // Severity toggle buttons; clicking flips the corresponding bit in the
    // LogStore's level mask.  Buttons are coloured by their level so the
    // active selection is visually obvious at a glance.
    uint8_t mask = m_pStore->GetLevelMask();
    for (int lvl = 0; lvl < kv8::KV8_LOG_LEVEL_COUNT; ++lvl)
    {
        const uint8_t bit  = static_cast<uint8_t>(1u << lvl);
        const bool    bOn  = (mask & bit) != 0;
        const ImVec4  base = LevelColor(lvl);
        ImVec4 normal = base;
        if (!bOn) { normal.x *= 0.35f; normal.y *= 0.35f; normal.z *= 0.35f; }
        ImGui::PushStyleColor(ImGuiCol_Button,        normal);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, base);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  base);
        if (ImGui::Button(LevelLabel(lvl)))
            mask ^= bit;
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
    }
    m_pStore->SetLevelMask(mask);

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputTextWithHint("##log_filter", "filter text",
                                  m_szFilter, sizeof(m_szFilter)))
    {
        m_pStore->SetTextFilter(m_szFilter);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &m_bFollow);
    ImGui::SameLine();
    ImGui::Text("| Visible: %zu / %zu",
                m_pStore->CountVisible(),
                m_pStore->GetAll().size());

    ImGui::Separator();

    // ── Visible X range from the waveform (for in-window highlight) ────────
    double dXMin = 0.0, dXMax = 0.0;
    if (pWaveform)
        pWaveform->GetVisibleXLimits(dXMin, dXMax);
    const uint64_t tsMinNs =
        (dXMin > 0.0) ? static_cast<uint64_t>(dXMin * 1.0e9) : 0ULL;
    const uint64_t tsMaxNs =
        (dXMax > 0.0) ? static_cast<uint64_t>(dXMax * 1.0e9) : ~0ULL;

    // ── Table ──────────────────────────────────────────────────────────────
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Borders   | ImGuiTableFlags_RowBg    |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY  |
        ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("##log_table", 7, kTableFlags))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Level",     ImGuiTableColumnFlags_WidthFixed,  60.0f);
        ImGui::TableSetupColumn("CPU",       ImGuiTableColumnFlags_WidthFixed,  40.0f);
        ImGui::TableSetupColumn("Thread",    ImGuiTableColumnFlags_WidthFixed,  80.0f);
        ImGui::TableSetupColumn("File:Line", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Function",  ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Message",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        const auto& entries = m_pStore->GetAll();
        const std::string sFilter = m_pStore->GetTextFilter();

        // Background colour for entries inside the visible waveform window.
        const ImU32 uInWindowBg = ImGui::GetColorU32(ImVec4(0.10f, 0.20f, 0.30f, 0.55f));

        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];

            const uint8_t bit = static_cast<uint8_t>(1u << static_cast<unsigned>(e.eLevel));
            if (!(mask & bit))                                      continue;
            if (!sFilter.empty() &&
                e.sMessage.find(sFilter) == std::string::npos)
                continue;

            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            // Severity tint goes on RowBg0 (base layer); the in-window
            // overlay (if any) goes on RowBg1 so both are visible at once.
            const ImVec4 vSevBg = LevelRowBgColor(static_cast<int>(e.eLevel));
            if (vSevBg.w > 0.0f)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::GetColorU32(vSevBg));

            const bool bInWindow = (e.tsNs >= tsMinNs && e.tsNs <= tsMaxNs);
            if (bInWindow)
            {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, uInWindowBg);
            }

            char szTs[40];
            FormatWallNs(e.tsNs, szTs, sizeof(szTs));

            ImGui::TableNextColumn();
            // Selectable spans the row; on click, seek the waveform to this entry.
            if (ImGui::Selectable(szTs, false,
                                  ImGuiSelectableFlags_SpanAllColumns))
            {
                if (pWaveform)
                    pWaveform->NavigateTo(static_cast<double>(e.tsNs) * 1.0e-9);
            }

            ImGui::TableNextColumn();
            ImGui::TextColored(LevelColor(static_cast<int>(e.eLevel)),
                               "%s", LevelLabel(static_cast<int>(e.eLevel)));

            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(e.wCpuID));

            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", static_cast<unsigned>(e.dwThreadID));

            ImGui::TableNextColumn();
            if (e.bSiteResolved)
                ImGui::Text("%s:%u", e.sFile.c_str(), static_cast<unsigned>(e.dwLine));
            else
                ImGui::TextDisabled("%s", e.sFile.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", e.sFunc.c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.sMessage.c_str());
            if (ImGui::IsItemHovered() && !e.sMessage.empty())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(640.0f);
                ImGui::TextUnformatted(e.sMessage.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ImGui::PopID();
        }

        if (m_bFollow)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndTable();
    }

    ImGui::End();
}
