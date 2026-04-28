// kv8scope -- Kv8 Software Oscilloscope
// LogPanel.cpp -- Dockable trace-log viewer (Phase L4).

#include "LogPanel.h"

#include "LogStore.h"
#include "WaveformRenderer.h"
#include "FontManager.h"

#include <kv8/Kv8Types.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#  include <commdlg.h>
#endif

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

// Compact UTC timestamp (YYYYMMDDTHHMMSSZ) for filenames.  Buffer must
// hold at least 17 chars including NUL.
static void FormatWallNsCompact(uint64_t qwWallNs, char* buf, size_t sz)
{
    const uint64_t qwSec = qwWallNs / 1000000000ULL;
    std::time_t t = static_cast<std::time_t>(qwSec);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::snprintf(buf, sz, "%04d%02d%02dT%02d%02d%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// Replace any character that is not a portable filename character with
// an underscore.  Mirrors the policy used by other kv8 tools (alnum,
// '.', '_', '-' kept verbatim).  The session label can carry slashes
// and spaces so we cannot pass it through unchanged.
static std::string SanitiseForFilename(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool bSafe =
            (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
            (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-';
        out.push_back(bSafe ? c : '_');
    }
    if (out.empty()) out = "trace_log";
    return out;
}

// ----------------------------------------------------------------------------
// Auto-resolve helper.  Given a freshly-selected log entry, narrow the
// telemetry X span so the selected WARN/ERROR/FATAL marker is well clear
// of its nearest WARN+ neighbour on the timeline.  Only zooms in -- if
// the current span already resolves the entry, the requested span equals
// the current span (so no visible change beyond a re-centre).
//
// Triggering rules:
//   * Only WARN+ entries (DEBUG/INFO never get a marker).
//   * Always centres the view on the selected entry.
//   * Span = max(8 * neighbour_distance, 1 second), capped to current span.
//   * No neighbour at all -> centre with a small default span (5 s),
//     still capped to current span so we never zoom out.
// ----------------------------------------------------------------------------
static void AutoResolveSelection(WaveformRenderer*       pWaveform,
                                  const LogStore::Entry&  selected,
                                  const std::vector<LogStore::Entry>& entries)
{
    if (!pWaveform)                                         return;
    if (static_cast<int>(selected.eLevel) < 2)              return;  // WARN+ only

    // Walk all entries to find the closest WARN+ neighbour in time.
    uint64_t qwBestDtNs = ~0ULL;
    for (const auto& e : entries)
    {
        if (e.tsNs == selected.tsNs)                        continue;
        if (static_cast<int>(e.eLevel) < 2)                 continue;
        const uint64_t qwDt = (e.tsNs > selected.tsNs)
            ? (e.tsNs - selected.tsNs)
            : (selected.tsNs - e.tsNs);
        if (qwDt < qwBestDtNs) qwBestDtNs = qwDt;
    }

    double dXMin = 0.0, dXMax = 0.0;
    pWaveform->GetVisibleXLimits(dXMin, dXMax);
    const double dCurSpanSec = dXMax - dXMin;
    if (dCurSpanSec <= 0.0)    return;

    // Margin factor: neighbour should sit at ~4x the bucket width away so
    // the LOD aggregator (4 px buckets, ~250 buckets across the plot)
    // keeps the selection in its own bucket.  Solving:
    //   neighbour_distance >= 4 * (span / 250)  ==>  span <= 62.5 * neighbour
    // Using 60 keeps the maths round.
    static constexpr double kSpanPerNeighbourSec = 60.0;
    static constexpr double kMinSpanSec          =  1.0;
    static constexpr double kNoNeighbourSpanSec  =  5.0;

    double dTargetSpanSec = kNoNeighbourSpanSec;
    if (qwBestDtNs != ~0ULL)
    {
        const double dDtSec = static_cast<double>(qwBestDtNs) * 1.0e-9;
        dTargetSpanSec = dDtSec * kSpanPerNeighbourSec;
        if (dTargetSpanSec < kMinSpanSec) dTargetSpanSec = kMinSpanSec;
    }
    // Never zoom out -- this mode only ever tightens the view.
    if (dTargetSpanSec > dCurSpanSec) dTargetSpanSec = dCurSpanSec;
    if (dTargetSpanSec <= 0.0)        return;

    const double dCenterRel =
        static_cast<double>(selected.tsNs) * 1.0e-9 - pWaveform->GetSessionOrigin();
    pWaveform->NavigateToWithSpan(dCenterRel, dTargetSpanSec);
}

void LogPanel::Render(WaveformRenderer* pWaveform)
{
    if (!m_bVisible)  return;
    if (!m_pStore)    return;

    // ── Cross-panel sync: marker click on the waveform ────────────────────
    // Promotes that entry to the panel's selection so the row is
    // highlighted and scrolled into view on this frame.
    if (pWaveform)
    {
        uint64_t qwClickedNs = 0;
        if (pWaveform->ConsumeLogMarkerClick(qwClickedNs) && qwClickedNs)
        {
            m_qwSelectedTsNs     = qwClickedNs;
            m_bScrollToSelection = true;
            // Manual selection wins over Follow auto-scroll.
            m_bFollow            = false;
        }
        // Push current selection so the waveform draws a dashed cursor
        // at this timestamp on every plot.  Pushed unconditionally each
        // frame so deselection (m_qwSelectedTsNs == 0) clears the line.
        pWaveform->SetSelectedLogTsNs(m_qwSelectedTsNs);

        // Auto-resolve: when enabled, a freshly-selected WARN+ entry that
        // would aggregate into a multi-entry LOD bucket triggers a one-
        // shot zoom-in tight enough to separate it from its neighbours.
        // Memoised by m_qwLastResolvedTsNs so we do not re-zoom every
        // frame while the same selection persists.
        if (m_bAutoResolve && m_qwSelectedTsNs != 0 &&
            m_qwSelectedTsNs != m_qwLastResolvedTsNs)
        {
            m_qwLastResolvedTsNs = m_qwSelectedTsNs;
            const auto& entries = m_pStore->GetAll();
            for (const auto& e : entries)
            {
                if (e.tsNs == m_qwSelectedTsNs)
                {
                    AutoResolveSelection(pWaveform, e, entries);
                    break;
                }
            }
        }
        else if (m_qwSelectedTsNs == 0)
        {
            m_qwLastResolvedTsNs = 0;
        }
    }

    // Reasonable default size/position so the window is visible the very
    // first time it pops up.  Persisted across sessions via imgui.ini.
    // The "##" disambiguator must be STABLE across kv8scope runs so
    // ImGui can match the saved entry in imgui.ini.  Earlier we used
    // the LogPanel instance address (`%p`) here -- correct as a per-
    // process disambiguator but the address changes every launch, so
    // imgui.ini never restored size/position.  The session label is
    // unique per ScopeWindow and stable across runs of the same
    // session, so prefer it; fall back to a fixed string when no label
    // is set yet (single-session edge case).
    ImGui::SetNextWindowSize(ImVec2(960.0f, 320.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80.0f, 560.0f),  ImGuiCond_FirstUseEver);

    const std::string sIdSuffix = m_sSessionLabel.empty()
        ? std::string("default")
        : SanitiseForFilename(m_sSessionLabel);  // alnum / . / _ / -
    char szTitle[160];
    std::snprintf(szTitle, sizeof(szTitle),
                  "Trace Log##LogPanel_%s",
                  sIdSuffix.c_str());

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
    ImGui::BeginDisabled(!m_bSessionLive);
    ImGui::Checkbox("Follow", &m_bFollow);
    ImGui::EndDisabled();
    if (!m_bSessionLive && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Follow is only meaningful for live sessions.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Sync", &m_bSync))
    {
        // Toggling Sync ON forces a re-scroll on this frame.
        m_qwLastWindowMaxNs = 0;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep the log row selection synchronised\n"
                          "with the waveform's visible window.");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-resolve", &m_bAutoResolve);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When a WARN/ERROR/FATAL entry is selected and the\n"
                          "telemetry zoom is too coarse, narrow the X range\n"
                          "just enough to separate the entry from its\n"
                          "neighbours on the timeline.  Never zooms out.");
    ImGui::SameLine();
    ImGui::Text("| Visible: %zu / %zu",
                m_pStore->CountVisible(),
                m_pStore->GetAll().size());

    ImGui::SameLine();
    if (ImGui::Button("Export..."))
        m_bShowExportDialog = true;

    ImGui::Separator();

    // ── Visible X range from the waveform (for in-window highlight) ────────
    // GetVisibleXLimits() is session-relative seconds, but LogStore::Entry
    // timestamps are absolute Unix-epoch ns.  Add the session origin
    // before converting -- without this, every entry falls outside the
    // window because session-relative ns (~10^10) is dwarfed by absolute
    // Unix ns (~10^18) and no row ever passes the in-window filter.
    double dXMin = 0.0, dXMax = 0.0;
    if (pWaveform)
        pWaveform->GetVisibleXLimits(dXMin, dXMax);
    const double dOrigin = pWaveform ? pWaveform->GetSessionOrigin() : 0.0;
    const uint64_t tsMinNs =
        (dXMin > 0.0) ? static_cast<uint64_t>((dXMin + dOrigin) * 1.0e9) : 0ULL;
    const uint64_t tsMaxNs =
        (dXMax > 0.0) ? static_cast<uint64_t>((dXMax + dOrigin) * 1.0e9) : ~0ULL;

    // Auto-deselect when the selected entry has scrolled out of the
    // waveform's visible window.  Without this, a stale selection
    // persists on the timeline (halo + dashed cursor at an off-screen
    // X coordinate) and a subsequent click on a different marker
    // sometimes failed to register on the first attempt -- the panel
    // was still pinned to the previous selection on that frame and
    // only updated on the next.  Dropping the selection here also
    // matches user intent: out-of-view means out-of-context.
    if (m_qwSelectedTsNs != 0 && pWaveform && dXMax > dXMin &&
        (m_qwSelectedTsNs < tsMinNs || m_qwSelectedTsNs > tsMaxNs))
    {
        m_qwSelectedTsNs     = 0;
        m_qwLastResolvedTsNs = 0;
    }

    // ── Sync mode: when the waveform window moves, snap the table's
    // selection to the newest entry inside the new window.  This keeps
    // both views aligned without requiring an explicit click.
    if (m_bSync && pWaveform && tsMaxNs != m_qwLastWindowMaxNs)
    {
        m_qwLastWindowMaxNs = tsMaxNs;
        const auto& entries = m_pStore->GetAll();
        // Pick the latest entry that falls inside [tsMinNs, tsMaxNs].
        // Entries are time-ordered, so a reverse linear scan is bounded
        // by the number of post-window records and is cheap in practice.
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        {
            if (it->tsNs >= tsMinNs && it->tsNs <= tsMaxNs)
            {
                if (it->tsNs != m_qwSelectedTsNs)
                {
                    m_qwSelectedTsNs     = it->tsNs;
                    m_bScrollToSelection = true;
                    // Sync owns the scroll position -- Follow would fight it.
                    m_bFollow            = false;
                }
                break;
            }
        }
    }

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
        // Background colour for the selected row (cross-panel pin).
        const ImU32 uSelectedBg = ImGui::GetColorU32(ImVec4(0.25f, 0.50f, 0.85f, 0.60f));

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

            // Cross-panel selection -- overrides the in-window tint.
            const bool bSelected = (m_qwSelectedTsNs != 0 &&
                                    e.tsNs == m_qwSelectedTsNs);
            if (bSelected)
            {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, uSelectedBg);
                if (m_bScrollToSelection)
                {
                    ImGui::SetScrollHereY(0.5f);
                    m_bScrollToSelection = false;
                }
            }

            // Bold the selected row so it stays distinguishable from the
            // background tint alone -- helps when scanning a busy table.
            FontManager* pFM = FontManager::Get();
            if (bSelected && pFM)
                pFM->PushBold();

            char szTs[40];
            FormatWallNs(e.tsNs, szTs, sizeof(szTs));

            ImGui::TableNextColumn();
            // Selectable spans the row; on click, seek the waveform to this entry.
            if (ImGui::Selectable(szTs, bSelected,
                                  ImGuiSelectableFlags_SpanAllColumns))
            {
                m_qwSelectedTsNs = e.tsNs;
                if (pWaveform)
                {
                    // tsNs is absolute Unix epoch; NavigateTo expects
                    // session-relative seconds.  Subtract the session origin
                    // or the seek lands ~1.7e9 s past every sample and the
                    // graphs render empty.
                    const double dAbs = static_cast<double>(e.tsNs) * 1.0e-9;
                    pWaveform->NavigateTo(dAbs - pWaveform->GetSessionOrigin());
                    // Run AutoResolve immediately too so a single click
                    // produces a single combined pan + zoom on the next
                    // frame instead of pan now / zoom one frame later.
                    if (m_bAutoResolve)
                    {
                        AutoResolveSelection(pWaveform, e, entries);
                        m_qwLastResolvedTsNs = e.tsNs;
                    }
                }
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

            if (bSelected && pFM)
                FontManager::PopFont();

            ImGui::PopID();
        }

        if (m_bFollow && m_bSessionLive && !m_bScrollToSelection)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndTable();
    }

    // ── Export-to-CSV dialog ──────────────────────────────────────────────
    // Modal popup driven by the toolbar's [Export...] button.  Three knobs:
    //   1. Range -- All entries vs. only those inside the telemetry plot's
    //      visible X window (uses pWaveform->GetVisibleXLimits()).
    //   2. Apply current filter -- when on, the level-mask checkboxes and
    //      substring filter from the panel are reused; when off the export
    //      contains every entry in the chosen range regardless of filter.
    //   3. Output path -- editable + native file picker on Windows.
    if (m_bShowExportDialog)
    {
        ImGui::OpenPopup("Export trace log to CSV##LogPanelExport");
        m_bShowExportDialog = false;
        // Each open starts a fresh suggestion -- the user may have
        // changed sessions or chosen a different range since last time.
        m_bExportPathAuto    = true;
        m_szExportPath[0]    = '\0';
        m_szExportStatus[0]  = '\0';
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Export trace log to CSV##LogPanelExport",
                                nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Range:");
        ImGui::SameLine();
        ImGui::RadioButton("All entries##logExpAll",     &m_iExportRange, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Telemetry visible window##logExpVis",
                           &m_iExportRange, 1);

        ImGui::Checkbox("Apply current filter (level mask + text)",
                        &m_bExportApplyFilter);

        // Resolve the chosen time window now so the user can see the row
        // count estimate before clicking Export.
        uint64_t tsLoNs = 0ULL;
        uint64_t tsHiNs = ~0ULL;
        if (m_iExportRange == 1 && pWaveform)
        {
            // Same coordinate-frame fix as the in-window highlight:
            // GetVisibleXLimits() is session-relative seconds, entries
            // are absolute Unix-epoch ns -- add session origin first.
            double dXMin = 0.0, dXMax = 0.0;
            pWaveform->GetVisibleXLimits(dXMin, dXMax);
            const double dOriginExp = pWaveform->GetSessionOrigin();
            tsLoNs = (dXMin > 0.0)
                ? static_cast<uint64_t>((dXMin + dOriginExp) * 1.0e9) : 0ULL;
            tsHiNs = (dXMax > 0.0)
                ? static_cast<uint64_t>((dXMax + dOriginExp) * 1.0e9) : ~0ULL;
        }

        const uint8_t      uMask    = m_bExportApplyFilter
                                          ? m_pStore->GetLevelMask() : uint8_t{0x1Fu};
        const std::string  sFilter  = m_bExportApplyFilter
                                          ? m_pStore->GetTextFilter() : std::string{};

        // Estimate -- O(N) over entries; cheap enough for an interactive
        // dialog that opens on user click.
        size_t nEstimate = 0;
        for (const auto& e : m_pStore->GetAll())
        {
            if (e.tsNs < tsLoNs || e.tsNs > tsHiNs) continue;
            const uint8_t bit = static_cast<uint8_t>(
                1u << static_cast<unsigned>(e.eLevel));
            if (!(uMask & bit)) continue;
            if (!sFilter.empty() &&
                e.sMessage.find(sFilter) == std::string::npos) continue;
            ++nEstimate;
        }
        ImGui::TextDisabled("  Will export %zu of %zu entries",
                            nEstimate, m_pStore->GetAll().size());

        // Suggested filename: regenerated every frame as long as the
        // user has not manually edited the path.  Long names are fine
        // -- they encode the session, range and time window so files
        // sort and self-document on disk.
        if (m_bExportPathAuto)
        {
            // For "All entries" the chosen window is unbounded; use the
            // store's actual time span instead so the filename reflects
            // what is really being exported.
            uint64_t tsLoLabel = tsLoNs;
            uint64_t tsHiLabel = tsHiNs;
            const auto& entries = m_pStore->GetAll();
            if (m_iExportRange == 0 && !entries.empty())
            {
                tsLoLabel = entries.front().tsNs;
                tsHiLabel = entries.back().tsNs;
            }

            const std::string sSession = SanitiseForFilename(
                m_sSessionLabel.empty() ? std::string("trace_log")
                                        : m_sSessionLabel);
            const char* sRangeTag = (m_iExportRange == 0) ? "all" : "win";
            const bool  bFiltered = m_bExportApplyFilter &&
                                    (m_pStore->GetLevelMask() != 0x1Fu ||
                                     !m_pStore->GetTextFilter().empty());

            char szLo[24] = {};
            char szHi[24] = {};
            if (tsLoLabel != 0ULL && tsLoLabel != ~0ULL)
                FormatWallNsCompact(tsLoLabel, szLo, sizeof(szLo));
            if (tsHiLabel != 0ULL && tsHiLabel != ~0ULL)
                FormatWallNsCompact(tsHiLabel, szHi, sizeof(szHi));

            char szSuggested[1024];
            if (szLo[0] && szHi[0])
                std::snprintf(szSuggested, sizeof(szSuggested),
                              "%s_log_%s_%s_to_%s%s.csv",
                              sSession.c_str(), sRangeTag, szLo, szHi,
                              bFiltered ? "_filt" : "");
            else
                std::snprintf(szSuggested, sizeof(szSuggested),
                              "%s_log_%s%s.csv",
                              sSession.c_str(), sRangeTag,
                              bFiltered ? "_filt" : "");

            std::strncpy(m_szExportPath, szSuggested,
                         sizeof(m_szExportPath) - 1);
            m_szExportPath[sizeof(m_szExportPath) - 1] = '\0';
        }

        ImGui::Separator();
        ImGui::Text("Output file:");
        ImGui::SetNextItemWidth(-120.0f);
        if (ImGui::InputText("##logExportPath",
                             m_szExportPath, sizeof(m_szExportPath)))
        {
            // Any keystroke pins the path -- stop overwriting it.
            m_bExportPathAuto = false;
        }
#ifdef _WIN32
        ImGui::SameLine();
        if (ImGui::Button("Browse...##logExportBrowse"))
        {
            OPENFILENAMEA ofn{};
            char szFile[1024];
            std::strncpy(szFile, m_szExportPath, sizeof(szFile) - 1);
            szFile[sizeof(szFile) - 1] = '\0';
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = "CSV files\0*.csv\0All files\0*.*\0";
            ofn.lpstrFile   = szFile;
            ofn.nMaxFile    = static_cast<DWORD>(sizeof(szFile));
            ofn.lpstrDefExt = "csv";
            ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
            if (GetSaveFileNameA(&ofn))
            {
                std::strncpy(m_szExportPath, szFile,
                             sizeof(m_szExportPath) - 1);
                // Browsing implies the user picked a specific path.
                m_bExportPathAuto = false;
            }
        }
#endif

        ImGui::Separator();

        const bool bCanExport = (m_szExportPath[0] != '\0');
        if (!bCanExport) ImGui::BeginDisabled();
        if (ImGui::Button("Export##logExportGo", ImVec2(100, 0)))
        {
            size_t nWritten = 0;
            const bool bOk = m_pStore->ExportCSV(
                tsLoNs, tsHiNs, uMask, sFilter,
                std::string(m_szExportPath), &nWritten);
            if (bOk)
                std::snprintf(m_szExportStatus, sizeof(m_szExportStatus),
                              "Saved %zu entries to %s",
                              nWritten, m_szExportPath);
            else
                std::snprintf(m_szExportStatus, sizeof(m_szExportStatus),
                              "ERROR: could not write %s",
                              m_szExportPath);
        }
        if (!bCanExport) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Close##logExportClose", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();

        if (m_szExportStatus[0])
        {
            ImGui::Separator();
            ImGui::TextWrapped("%s", m_szExportStatus);
        }

        ImGui::EndPopup();
    }

    ImGui::End();
}
