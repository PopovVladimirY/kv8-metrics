// kv8scope -- Kv8 Software Oscilloscope
// ScopeWindow.cpp -- ImGui window shell for a single session.

#include "ScopeWindow.h"
#include "ConfigStore.h"
#include "ConsumerThread.h"
#include "CounterTree.h"
#include "StatsEngine.h"
#include "StatsPanel.h"
#include "LogStore.h"
#include "LogPanel.h"
#include "WaveformRenderer.h"

#include <kv8/IKv8Producer.h>

#include "imgui.h"

#include <cinttypes>
#include "implot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ScopeWindow::ScopeWindow(const std::string& sPrefix,
                         const kv8::SessionMeta& meta,
                         ConfigStore* pConfig,
                         bool bOnline)
    : m_sPrefix(sPrefix)
    , m_meta(meta)
    , m_pConfig(pConfig)
    , m_bOnline(bOnline)
    , m_bAutoScroll(bOnline)   // live sessions start in auto-scroll mode
{
    // Build a human-readable window title.  Prefer the session name from
    // the registry; fall back to the raw session ID.
    if (!m_meta.sName.empty())
        m_sWindowTitle = "Scope: " + m_meta.sName;
    else
        m_sWindowTitle = "Scope: " + m_meta.sSessionID;

    // Create ConsumerThread -- do NOT Start() yet so we can register
    // all callbacks before the first message is polled.
    m_pConsumer = std::make_unique<ConsumerThread>(meta, pConfig);

    // -----------------------------------------------------------------
    // Annotation Kafka persistence (Section 14)
    // -----------------------------------------------------------------
    // Topic: <channel>.<sessionID>._annotations  (log-compacted via broker config)
    m_sAnnotationTopic = m_meta.sSessionPrefix + "._annotations";
    m_pAnnotations     = std::make_unique<AnnotationStore>();

    // Create a dedicated Kafka producer for publishing annotation records.
    {
        kv8::Kv8Config cfg;
        const auto& sc     = pConfig->Get();
        cfg.sBrokers       = sc.sBrokers;
        cfg.sSecurityProto = sc.sSecurityProtocol;
        cfg.sSaslMechanism = sc.sSaslMechanism;
        cfg.sUser          = sc.sUsername;
        cfg.sPass          = sc.sPassword;
        m_pAnnProducer = kv8::IKv8Producer::Create(cfg);
    }
    if (m_pAnnProducer)
        m_pAnnotations->SetKafkaSink(m_pAnnProducer.get(), m_sAnnotationTopic);

    // Create a dedicated Kafka producer for writing counter enable/disable
    // commands to the <session>._ctl topic.
    m_sCtrlTopic = m_meta.sSessionPrefix + "._ctl";
    {
        kv8::Kv8Config cfg;
        const auto& sc     = pConfig->Get();
        cfg.sBrokers       = sc.sBrokers;
        cfg.sSecurityProto = sc.sSecurityProtocol;
        cfg.sSaslMechanism = sc.sSaslMechanism;
        cfg.sUser          = sc.sUsername;
        cfg.sPass          = sc.sPassword;
        m_pCtrlProducer = kv8::IKv8Producer::Create(cfg);
    }

    // Tell the consumer thread to load existing annotations at startup and
    // subscribe for live updates.  Must be called before Start().
    m_pConsumer->SetAnnotationStore(m_pAnnotations.get(), m_sAnnotationTopic);

    // Tell the consumer thread to replay and subscribe to the ._ctl topic
    // so it picks up counter enabled state persisted from prior sessions.
    m_pConsumer->SetCtlTopic(m_sCtrlTopic);

    // -----------------------------------------------------------------
    // Create the waveform renderer and register all known counters.
    // -----------------------------------------------------------------
    m_pWaveform = std::make_unique<WaveformRenderer>(pConfig);
    for (const auto& [dwHash, counters] : m_meta.hashToCounters)
    {
        for (const auto& cm : counters)
        {
            if (cm.bIsUdtFeed) continue;  // skip descriptor; virtual scalars registered separately
            m_pWaveform->RegisterCounter(dwHash, cm.wCounterID, cm.sName,
                                         cm.dbMin, cm.dbMax);
        }
    }

    // Attach annotation store to the waveform renderer.
    m_pWaveform->SetAnnotationStore(m_pAnnotations.get());

    // Create the counter tree and initialise per-counter display state.
    // Colors are fetched from WaveformRenderer (which already has them
    // from the RegisterCounter calls above).
    m_pCounterTree = std::make_unique<CounterTree>();
    m_pCounterTree->Init(m_meta, m_bOnline, m_pWaveform.get(),
                         m_pConfig, m_sPrefix);

    // Wire E-checkbox callback to publish enable/disable commands to ctrl topic.
    if (m_pCtrlProducer)
    {
        m_pCounterTree->SetEnableChangedCallback(
            [this](uint32_t dwHash, uint16_t wID, bool bEnabled)
            {
                using namespace std::chrono;
                int64_t tsMs = duration_cast<milliseconds>(
                    system_clock::now().time_since_epoch()).count();
                char szCmd[128];
                std::snprintf(szCmd, sizeof(szCmd),
                    "{\"v\":1,\"cmd\":\"ctr_state\",\"wid\":%u,\"enabled\":%s,\"ts\":%" PRId64 "}",
                    static_cast<unsigned>(wID),
                    bEnabled ? "true" : "false", tsMs);
                m_pCtrlProducer->Produce(m_sCtrlTopic, szCmd,
                                         std::strlen(szCmd), nullptr, 0);
                m_pCtrlProducer->Flush(100);
            });
    }

    // Create StatsEngine (P4.1): register every counter then attach to the
    // consumer thread so Feed() is called on each ingested sample.
    m_pStats = std::make_unique<StatsEngine>();
    for (const auto& [dwHash, counters] : m_meta.hashToCounters)
        for (const auto& cm : counters)
        {
            if (cm.bIsUdtFeed) continue;
            m_pStats->RegisterCounter(dwHash, cm.wCounterID);
        }
    m_pConsumer->SetStatsEngine(m_pStats.get());

    // Create statistics panel (P4.4).
    m_pStatsPanel = std::make_unique<StatsPanel>();
    m_pStatsPanel->Init(m_meta);

    // -----------------------------------------------------------------
    // Trace-log pipeline (Phase L4).  The LogStore receives decoded
    // Kv8LogRecord entries from ConsumerThread on the ._log topic, plus
    // KV8_CID_LOG_SITE descriptors from ._registry.  Sites already known
    // at session-discovery time are seeded directly from SessionMeta.
    // -----------------------------------------------------------------
    m_pLogStore = std::make_unique<LogStore>();
    if (!m_meta.logSites.empty())
        m_pLogStore->SeedLogSites(m_meta.logSites);

    m_pLogPanel = std::make_unique<LogPanel>();
    m_pLogPanel->SetLogStore(m_pLogStore.get());
    m_pWaveform->SetLogStore(m_pLogStore.get());

    {
        // Channel name = sSessionPrefix with the trailing ".<sessionID>" stripped.
        std::string sChannel = m_meta.sSessionPrefix;
        if (sChannel.size() > m_meta.sSessionID.size() + 1)
            sChannel.resize(sChannel.size() - m_meta.sSessionID.size() - 1);
        const std::string sRegistryTopic = sChannel + "._registry";
        m_pConsumer->SetLogStore(m_pLogStore.get(),
                                 m_meta.sLogTopic,
                                 sRegistryTopic);
    }

    // All callbacks registered -- start ingesting data.
    m_pConsumer->Start();

}

ScopeWindow::~ScopeWindow()
{
    // Stop the consumer thread before destroying ring buffers.
    if (m_pConsumer)
    {
        m_pConsumer->RequestStop();
        m_pConsumer->Join();
    }
}

// ---------------------------------------------------------------------------
// Focus -- bring this window to front
// ---------------------------------------------------------------------------

void ScopeWindow::Focus()
{
    m_bFocusNext = true;
}

// ---------------------------------------------------------------------------
// Render -- returns false when the window was closed
// ---------------------------------------------------------------------------

bool ScopeWindow::Render()
{
    if (!m_bOpen)
        return false;

    // -----------------------------------------------------------------------
    // Deferred reinit -- non-blocking.
    //
    // Phase A (done in NotifyMetaUpdated): RequestStop() + set flag.
    // Phase B (done here, each frame):
    //   1. Move m_pConsumer -> m_pOldConsumer (once).
    //   2. Poll m_pOldConsumer->IsDone() each frame.  If not done yet,
    //      skip the rebuild and continue rendering -- no blocking.
    //   3. Once IsDone() returns true, Join() is instantaneous.
    //      Destroy the old consumer and rebuild UI + new consumer.
    // -----------------------------------------------------------------------
    if (m_bReinitPending)
    {
        // Move the stopping consumer aside (once).
        if (m_pConsumer && !m_pOldConsumer)
        {
            m_pOldConsumer = std::move(m_pConsumer);
            // m_pConsumer is now nullptr; m_pOldConsumer is stopping.
        }

        // Poll: has the old consumer thread finished (including the
        // librdkafka drain + close inside ~IKv8Consumer)?
        if (m_pOldConsumer && !m_pOldConsumer->IsDone())
        {
            // Not done yet -- skip the rebuild this frame.
            // Fall through to render the rest of the window normally;
            // the old consumer's ring buffers are NOT drained (no stale data).
        }
        else
        {
            // Old consumer is fully stopped (or was already nullptr).
            if (m_pOldConsumer)
            {
                m_pOldConsumer->Join();   // instantaneous -- thread already exited
                m_pOldConsumer.reset();
            }

            // Incremental hot-patch (Fix 2 -- KV8_UDT_STALL):
            // Keep existing WaveformRenderer / CounterTree / StatsEngine /
            // StatsPanel and add only the newly appeared counters.  This
            // preserves all accumulated graph data so the display never goes
            // blank during a MetaUpdated reinit.
            m_meta = m_pendingMeta;

            for (const auto& [dwHash, counters] : m_meta.hashToCounters)
                for (const auto& cm : counters)
                {
                    if (cm.bIsUdtFeed) continue;
                    m_pWaveform->RegisterCounterIfNew(
                        dwHash, cm.wCounterID, cm.sName,
                        cm.dbMin, cm.dbMax);
                }

            m_pCounterTree->AddCounters(m_meta, m_pWaveform.get());

            for (const auto& [dwHash, counters] : m_meta.hashToCounters)
                for (const auto& cm : counters)
                {
                    if (cm.bIsUdtFeed) continue;
                    m_pStats->RegisterCounterIfNew(dwHash, cm.wCounterID);
                }

            m_pStatsPanel->AddCounters(m_meta);

            // Recreate the consumer with the full metadata so it has the
            // complete topic list and UDT decode contexts.
            m_pConsumer = std::make_unique<ConsumerThread>(m_meta, m_pConfig);
            m_pConsumer->SetAnnotationStore(m_pAnnotations.get(), m_sAnnotationTopic);
            m_pConsumer->SetCtlTopic(m_sCtrlTopic);
            m_pConsumer->SetStatsEngine(m_pStats.get());
            m_pConsumer->Start();

            m_bReinitPending = false;

            fprintf(stderr, "[kv8scope] MetaUpdated: hot-patched \"%s\" "
                    "with full UDT schema (data preserved).\n",
                    m_sPrefix.c_str());
        }
    }
    // -----------------------------------------------------------------------

    if (m_bFocusNext)
    {
        ImGui::SetNextWindowFocus();
        m_bFocusNext = false;
    }

    // Use a stable ID based on the prefix so docking layout persists.
    ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(m_sWindowTitle.c_str(), &m_bOpen))
    {
        ImGui::End();
        return m_bOpen;
    }

    // Drain ring buffers into the waveform renderer each frame.
    DrainAllRingBuffers();

    // Drain annotation records that arrived from Kafka on the consumer thread.
    if (m_pAnnotations)
        m_pAnnotations->DrainPending();

    // Drain trace-log records that arrived from Kafka (Phase L4).
    if (m_pLogStore)
        m_pLogStore->DrainPending();

    // Drain counter enable/disable records that arrived from Kafka (originating
    // from kv8log producer calls or another kv8scope instance).
    if (m_pConsumer && m_pCounterTree)
    {
        m_pConsumer->DrainCounterStateEvents([this](uint16_t wid, bool bEnabled)
        {
            for (const auto& [dwHash, counters] : m_meta.hashToCounters)
                for (const auto& cm : counters)
                    if (!cm.bIsUdtFeed && cm.wCounterID == wid)
                    {
                        m_pCounterTree->SetCounterEnabled(dwHash, wid, bEnabled);
                        return;
                    }
        });
    }

    RenderToolbar();
    ImGui::Separator();

    // Vertical stacked layout: counter list on top, waveform below.
    // This gives the table and graph full width of the window.
    float fTotalH = ImGui::GetContentRegionAvail().y
                  - ImGui::GetFrameHeightWithSpacing(); // reserve status bar

    float fListH = 150.0f; // default counter list height
    if (fListH > fTotalH * 0.35f)
        fListH = fTotalH * 0.35f;

    // Top child: counter list (full width, resizable height)
    ImGui::BeginChild("##CounterList",
                      ImVec2(0.0f, fListH),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    RenderCounterList();
    ImGui::EndChild();

    // Bottom child: waveform area (reserve 44 px for the overview strip below).
    static constexpr float kStripH = 44.0f;
    ImGui::BeginChild("##WaveformArea",
                      ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - kStripH),
                      ImGuiChildFlags_None);
    RenderWaveformArea();
    ImGui::EndChild();

    // Overview strip (section 6.7.1) -- miniature full-session view with
    // a highlight rect for the current viewport.  Click or drag to pan.
    if (m_pWaveform)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        const double dNavTarget =
            m_pWaveform->RenderOverviewStrip(ImVec2(0.0f, kStripH));
        ImGui::PopStyleVar();

        if (!std::isnan(dNavTarget))
        {
            m_bAutoScroll = false;
            m_pWaveform->NavigateTo(dNavTarget);
        }
    }

    // P3.7: propagate trace-click from waveform to counter tree
    // (bidirectional highlight linking).
    if (m_pWaveform && m_pCounterTree)
    {
        uint32_t dwH = 0; uint16_t wI = 0;
        if (m_pWaveform->ConsumeLastPlotClick(dwH, wI))
        {
            if (dwH != 0 || wI != 0)
                m_pCounterTree->SetHighlighted(dwH, wI, m_pWaveform.get());
            else
                m_pCounterTree->ClearHighlighted(m_pWaveform.get());
        }
    }

    // P8: Ctrl+left-click in the waveform requests a new annotation.
    if (m_pWaveform && m_pAnnotations)
    {
        double dReqTime = 0.0;
        if (m_pWaveform->ConsumeAnnotationRequest(dReqTime))
        {
            m_bAnnEditorIsNew = true;
            // Snap to the nearest visible sample (earliest when tied).
            // SnapToNearestSample works in session-relative coordinates;
            // add the origin back so m_dAnnEditorTime is Unix epoch seconds.
            m_dAnnEditorTime  = m_pWaveform->SnapToNearestSample(dReqTime)
                              + m_pWaveform->GetSessionOrigin();
            m_iAnnType        = 0;
            m_szAnnTitle[0]   = '\0';
            m_szAnnDesc[0]    = '\0';
            m_sAnnEditID.clear();
            m_bShowAnnEditor  = true;
        }
    }

    // P4.4: Ctrl+I toggles the statistics panel.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_I, false))
    {
        m_bShowStats = !m_bShowStats;
        if (m_pStatsPanel)
            m_pStatsPanel->ToggleVisible();
    }

    // L4: Ctrl+L toggles the trace-log panel.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_L, false))
    {
        if (m_pLogPanel)
            m_pLogPanel->ToggleVisible();
    }

    // P8: Ctrl+N opens the annotation editor at the center of the visible window.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_N, false) &&
        m_pAnnotations && m_pWaveform)
    {
        double dXMin = 0.0, dXMax = 1.0;
        m_pWaveform->GetVisibleXLimits(dXMin, dXMax);
        m_bAnnEditorIsNew = true;
        // Snap to the nearest visible sample to the window centre.
        // dXMin/dXMax are session-relative; add origin for absolute epoch.
        m_dAnnEditorTime  = m_pWaveform->SnapToNearestSample(
                                (dXMin + dXMax) * 0.5)
                          + m_pWaveform->GetSessionOrigin();
        m_iAnnType        = 0;
        m_szAnnTitle[0]   = '\0';
        m_szAnnDesc[0]    = '\0';
        m_sAnnEditID.clear();
        m_bShowAnnEditor  = true;
    }

    // P8: Ctrl+Shift+N toggles the annotation panel.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift &&
        ImGui::IsKeyPressed(ImGuiKey_N, false))
    {
        m_bShowAnnPanel = !m_bShowAnnPanel;
    }

    // P4.4: render the statistics panel (manages its own ImGui window).
    if (m_pStatsPanel)
        m_pStatsPanel->Render(m_pWaveform.get(), m_pStats.get());

    // L4: render the trace-log panel (manages its own ImGui window).
    if (m_pLogPanel)
        m_pLogPanel->Render(m_pWaveform.get());

    // Any panel (annotations, log panel, future) that called
    // WaveformRenderer::NavigateTo() since the previous frame implicitly
    // requests pause-live: a programmatic seek would otherwise be
    // overwritten by the live-edge auto-scroll path on the next frame and
    // the waveforms would either snap back to live or, worse, render at
    // an out-of-data X position and appear empty.
    if (m_pWaveform && m_pWaveform->ConsumeNavRequested())
        m_bAutoScroll = false;

    // P8: render annotation editor popup and panel window.
    if (m_bShowAnnEditor)
        RenderAnnotationEditor();
    if (m_bShowAnnPanel)
        RenderAnnotationPanel();

    // Bottom status bar
    RenderScopeStatusBar();

    ImGui::End();
    return m_bOpen;
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void ScopeWindow::RenderToolbar()
{
    // Single live-scroll control.
    // Pause is always available; returning to live requires a live session.
    const bool bCanGoLive = (m_eLiveness == SessionLiveness::Live ||
                             m_eLiveness == SessionLiveness::GoingOffline);

    if (m_bAutoScroll)
    {
        if (ImGui::Button(" || ##Pause"))
            m_bAutoScroll = false;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pause auto-scroll");
    }
    else
    {
        if (!bCanGoLive)
            ImGui::BeginDisabled(true);

        if (ImGui::Button("Return to live"))
            m_bAutoScroll = true;

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !bCanGoLive)
            ImGui::SetTooltip("Not available: session is offline");

        if (!bCanGoLive)
            ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (m_bAutoScroll && m_pWaveform)
        ImGui::TextDisabled("Live window: %.3g s", m_pWaveform->GetLiveSpan());
    else
        ImGui::TextDisabled("Live window: %.3g s",
                            m_pConfig ? m_pConfig->Get().dRealtimeTimeWin : 15.0);

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton("Show All") && m_pCounterTree)
        m_pCounterTree->ShowAll(m_pWaveform.get());
    ImGui::SameLine();
    if (ImGui::SmallButton("Hide All") && m_pCounterTree)
        m_pCounterTree->HideAll(m_pWaveform.get());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // P4.4: Stats panel toggle (also Ctrl+I)
    if (ImGui::SmallButton(m_bShowStats ? "[Stats ON]" : "[Stats]##stats"))
    {
        m_bShowStats = !m_bShowStats;
        if (m_pStatsPanel)
            m_pStatsPanel->ToggleVisible();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle statistics panel (Ctrl+I)");
    ImGui::SameLine();

    // L4: Trace-log panel toggle (also Ctrl+L), with live entry counter so
    // the user has feedback even when the panel is closed.
    {
        const bool bLogOn = m_pLogPanel && m_pLogPanel->IsVisible();
        const size_t nLog = m_pLogStore ? m_pLogStore->GetAll().size() : 0;
        char szLabel[64];
        std::snprintf(szLabel, sizeof(szLabel),
                      bLogOn ? "[Log ON %zu]##log" : "[Log %zu]##log",
                      nLog);
        if (ImGui::SmallButton(szLabel) && m_pLogPanel)
            m_pLogPanel->ToggleVisible();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggle trace-log panel (Ctrl+L) -- %zu entries",
                              nLog);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // P6.6: Visualization mode [S] / [R] / [C]
    {
        const ImVec4 kActive = ImVec4(0.26f, 0.59f, 0.98f, 0.45f);
        const VizMode cur =
            m_pWaveform ? m_pWaveform->GetVizMode() : VizMode::Simple;

        if (cur == VizMode::Simple)
            ImGui::PushStyleColor(ImGuiCol_Button, kActive);
        if (ImGui::SmallButton("S##vizS") && m_pCounterTree)
            m_pCounterTree->BatchSetVizMode(VizMode::Simple, m_pWaveform.get());
        if (cur == VizMode::Simple)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Simple: avg line");

        ImGui::SameLine();

        if (cur == VizMode::Range)
            ImGui::PushStyleColor(ImGuiCol_Button, kActive);
        if (ImGui::SmallButton("R##vizR") && m_pCounterTree)
            m_pCounterTree->BatchSetVizMode(VizMode::Range, m_pWaveform.get());
        if (cur == VizMode::Range)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Range: min/max fill + midpoint");

        ImGui::SameLine();

        if (cur == VizMode::Cyclogram)
            ImGui::PushStyleColor(ImGuiCol_Button, kActive);
        if (ImGui::SmallButton("C##vizC") && m_pCounterTree)
            m_pCounterTree->BatchSetVizMode(VizMode::Cyclogram, m_pWaveform.get());
        if (cur == VizMode::Cyclogram)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cyclogram: zero-order-hold staircase");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Cross-bar toggle
    {
        const bool bOn = m_pConfig ? m_pConfig->Get().bCrossBar : true;
        if (bOn)
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(0.26f, 0.59f, 0.98f, 0.45f));
        if (ImGui::SmallButton(bOn ? "[X-bar ON]" : "[X-bar]##xbar"))
        {
            if (m_pConfig)
            {
                m_pConfig->GetMut().bCrossBar = !bOn;
                m_pConfig->MarkDirty();
            }
        }
        if (bOn)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggle cross-bar (cursor value readout)");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Export CSV button
    if (ImGui::SmallButton("[Export CSV]##exp"))
    {
        // Build a default filename: <sessionID>_export.csv in CWD.
        // Pre-fill path only when opening fresh (status cleared).
        if (!m_bShowExportDialog)
        {
            // Default path: <sessionID>_export.csv
            const std::string& sid = m_meta.sSessionID.empty()
                                   ? m_sPrefix : m_meta.sSessionID;
            std::snprintf(m_szExportPath, sizeof(m_szExportPath),
                          "%s_export.csv", sid.c_str());
            m_szExportStatus[0] = '\0';
        }
        m_bShowExportDialog = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Export visible counters to CSV");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // P8: Annotation visibility toggle + per-type filter dropdown.
    {
        const bool bAnnOn = m_pWaveform ? m_pWaveform->GetAnnotationsVisible() : true;
        if (bAnnOn)
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(0.26f, 0.59f, 0.98f, 0.45f));
        if (ImGui::SmallButton(bAnnOn ? "[Ann ON]" : "[Ann]##ann"))
        {
            if (m_pWaveform)
                m_pWaveform->SetAnnotationsVisible(!bAnnOn);
        }
        if (bAnnOn)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggle all annotation markers");

        ImGui::SameLine(0.0f, 2.0f);

        // Small arrow button opens the per-type filter popup.
        if (ImGui::ArrowButton("##annDrop", ImGuiDir_Down))
            ImGui::OpenPopup("##AnnTypePopup");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Filter annotation types / open panel");

        if (ImGui::BeginPopup("##AnnTypePopup"))
        {
            ImGui::TextDisabled("Annotation types:");
            ImGui::Separator();
            for (int ti = 0; ti < AnnotationStore::kTypeCount; ++ti)
            {
                const AnnotationType t   = static_cast<AnnotationType>(ti);
                bool bTypeOn = m_pWaveform
                    ? m_pWaveform->GetAnnotationTypeVisible(t) : true;
                const ImVec4 tc = AnnotationStore::TypeColor(t);
                ImGui::PushStyleColor(ImGuiCol_Text, tc);
                if (ImGui::Checkbox(AnnotationStore::TypeName(t), &bTypeOn))
                {
                    if (m_pWaveform)
                        m_pWaveform->SetAnnotationTypeVisible(t, bTypeOn);
                }
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(m_bShowAnnPanel ? "Close Panel" : "Open Panel"))
                m_bShowAnnPanel = !m_bShowAnnPanel;
            ImGui::EndPopup();
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("Session: %s", m_meta.sName.c_str());

    // Export dialog is rendered here (inside the scope window frame)
    if (m_bShowExportDialog)
        RenderExportDialog();
}

// ---------------------------------------------------------------------------
// CSV export dialog
// ---------------------------------------------------------------------------

void ScopeWindow::RenderExportDialog()
{
    ImGui::SetNextWindowSize(ImVec2(520.0f, 230.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
               ImGui::GetIO().DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool bOpen = true;
    if (!ImGui::Begin("Export CSV##exportdlg", &bOpen,
                      ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        if (!bOpen) m_bShowExportDialog = false;
        return;
    }

    // Range selector
    ImGui::Text("Export range:");
    ImGui::SameLine();
    ImGui::RadioButton("Visible window##expV", &m_iExportRange, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Entire duration##expA", &m_iExportRange, 1);

    // Show row count estimate
    if (m_pWaveform)
    {
        double dXMin, dXMax;
        if (m_iExportRange == 0)
            m_pWaveform->GetVisibleXLimits(dXMin, dXMax);
        else
        {
            dXMin = m_pWaveform->GetEarliestTimestamp();
            dXMax = m_pWaveform->GetLatestTimestamp();
        }
        ImGui::TextDisabled("  Window: %.3f s  --  %.3f s  (span %.1f s)",
                            dXMin, dXMax, dXMax - dXMin);
    }

    ImGui::Separator();

    // File path input + optional native browse button
    ImGui::Text("Output file:");
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputText("##exportpath", m_szExportPath, sizeof(m_szExportPath));

#ifdef _WIN32
    ImGui::SameLine();
    if (ImGui::Button("Browse...##expbrowse"))
    {
        OPENFILENAMEA ofn;
        char szFile[1024];
        std::strncpy(szFile, m_szExportPath, sizeof(szFile) - 1);
        szFile[sizeof(szFile) - 1] = '\0';
        std::memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize  = sizeof(ofn);
        ofn.hwndOwner    = nullptr;
        ofn.lpstrFilter  = "CSV files\0*.csv\0All files\0*.*\0";
        ofn.lpstrFile    = szFile;
        ofn.nMaxFile     = static_cast<DWORD>(sizeof(szFile));
        ofn.lpstrDefExt  = "csv";
        ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        if (GetSaveFileNameA(&ofn))
            std::strncpy(m_szExportPath, szFile, sizeof(m_szExportPath) - 1);
    }
#endif

    ImGui::Separator();

    // Action buttons
    const bool bCanExport = (m_pWaveform && m_szExportPath[0] != '\0');
    if (!bCanExport)
        ImGui::BeginDisabled();

    if (ImGui::Button("Export##doexport", ImVec2(100, 0)))
    {
        double dXMin, dXMax;
        if (m_iExportRange == 0)
            m_pWaveform->GetVisibleXLimits(dXMin, dXMax);
        else
        {
            dXMin = m_pWaveform->GetEarliestTimestamp();
            dXMax = m_pWaveform->GetLatestTimestamp();
        }

        const bool bOk = m_pWaveform->ExportCSV(dXMin, dXMax,
                                                 std::string(m_szExportPath));
        if (bOk)
            std::snprintf(m_szExportStatus, sizeof(m_szExportStatus),
                          "Saved: %s", m_szExportPath);
        else
            std::snprintf(m_szExportStatus, sizeof(m_szExportStatus),
                          "ERROR: could not write %s", m_szExportPath);
    }

    if (!bCanExport)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Close##closeexport", ImVec2(100, 0)))
        m_bShowExportDialog = false;

    // Status line
    if (m_szExportStatus[0] != '\0')
    {
        ImGui::Spacing();
        // Green if success, red if error
        const bool bErr = (std::strncmp(m_szExportStatus, "ERROR", 5) == 0);
        ImGui::PushStyleColor(ImGuiCol_Text,
            bErr ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                 : ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
        ImGui::TextWrapped("%s", m_szExportStatus);
        ImGui::PopStyleColor();
    }

    if (!bOpen) m_bShowExportDialog = false;
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Annotation editor -- compact modal popup for creating or editing one
// annotation.  Opened by Ctrl+left-click on the waveform or by clicking
// Edit in the annotation panel.
// ---------------------------------------------------------------------------

void ScopeWindow::RenderAnnotationEditor()
{
    if (!m_pAnnotations)
        return;

    // Width-only hint: let AlwaysAutoResize compute the height from content.
    ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
               ImGui::GetIO().DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool bOpen = true;
    const char* pTitle = m_bAnnEditorIsNew ? "New Annotation##anneditor"
                                           : "Edit Annotation##anneditor";
    if (!ImGui::Begin(pTitle, &bOpen,
                      ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoCollapse       |
                      ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        if (!bOpen) m_bShowAnnEditor = false;
        return;
    }

    // Timestamp display (read-only).
    {
        time_t tSec = static_cast<time_t>(m_dAnnEditorTime);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &tSec);
#else
        localtime_r(&tSec, &tmv);
#endif
        char tbuf[32];
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
        ImGui::TextDisabled("Timestamp: %s  (%.3f s)", tbuf, m_dAnnEditorTime);
    }
    ImGui::Separator();

    // Type selector -- colorized radio buttons.
    ImGui::Text("Type:");
    ImGui::SameLine();
    for (int ti = 0; ti < AnnotationStore::kTypeCount; ++ti)
    {
        const AnnotationType t  = static_cast<AnnotationType>(ti);
        const ImVec4         tc = AnnotationStore::TypeColor(t);
        if (ti > 0) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, tc);
        ImGui::RadioButton(AnnotationStore::TypeName(t), &m_iAnnType, ti);
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // Title field.
    ImGui::Text("Title (required):");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##anntitle", m_szAnnTitle, sizeof(m_szAnnTitle));

    ImGui::Spacing();

    // Description field (multiline, optional).
    // Height is computed from the current font so it shows ~4 lines without
    // internal scrolling regardless of the active font size.
    ImGui::Text("Description (optional):");
    const float fDescH = ImGui::GetTextLineHeightWithSpacing() * 4.0f
                       + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::InputTextMultiline("##anndesc", m_szAnnDesc, sizeof(m_szAnnDesc),
                              ImVec2(-1.0f, fDescH));

    ImGui::Separator();

    const bool bCanSave = (m_szAnnTitle[0] != '\0');
    if (!bCanSave) ImGui::BeginDisabled();

    if (ImGui::Button(m_bAnnEditorIsNew ? "Add##annAdd" : "Save##annSave",
                      ImVec2(100.0f, 0.0f)))
    {
        const AnnotationType eType =
            static_cast<AnnotationType>(m_iAnnType);

        if (m_bAnnEditorIsNew)
        {
            m_pAnnotations->Add(m_dAnnEditorTime,
                                std::string(m_szAnnTitle),
                                std::string(m_szAnnDesc),
                                eType);
        }
        else
        {
            m_pAnnotations->Edit(m_sAnnEditID,
                                 std::string(m_szAnnTitle),
                                 std::string(m_szAnnDesc),
                                 eType);
        }
        m_bShowAnnEditor = false;
    }

    if (!bCanSave) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel##annCancel", ImVec2(100.0f, 0.0f)))
        m_bShowAnnEditor = false;

    if (!bOpen) m_bShowAnnEditor = false;
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Annotation panel -- sortable list of all annotations for this session.
// Opened via the Ann dropdown or Ctrl+Shift+N.
// ---------------------------------------------------------------------------

void ScopeWindow::RenderAnnotationPanel()
{
    if (!m_pAnnotations)
        return;

    ImGui::SetNextWindowSize(ImVec2(640.0f, 260.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
               ImGui::GetIO().DisplaySize.y * 0.75f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    bool bOpen = true;
    if (!ImGui::Begin("Annotations##annpanel", &bOpen,
                      ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        if (!bOpen) m_bShowAnnPanel = false;
        return;
    }

    // Toolbar row.
    if (ImGui::SmallButton("[+ New]##annNew"))
    {
        // Open editor for a new annotation snapped to the visible sample
        // nearest to the centre of the current visible window.
        double dXMin = 0.0, dXMax = 1.0;
        if (m_pWaveform)
            m_pWaveform->GetVisibleXLimits(dXMin, dXMax);
        m_bAnnEditorIsNew = true;
        m_dAnnEditorTime  = m_pWaveform
            ? m_pWaveform->SnapToNearestSample((dXMin + dXMax) * 0.5)
            : (dXMin + dXMax) * 0.5;
        m_iAnnType        = 0;
        m_szAnnTitle[0]   = '\0';
        m_szAnnDesc[0]    = '\0';
        m_sAnnEditID.clear();
        m_bShowAnnEditor  = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add annotation at center of visible window (Ctrl+N)");

    ImGui::SameLine();

    // Count active vs deleted.
    int nActive = 0, nDeleted = 0;
    for (const auto& a : m_pAnnotations->GetAll())
        (a.bDeleted ? nDeleted : nActive)++;

    if (nDeleted > 0)
        ImGui::TextDisabled("%d annotation(s), %d deleted", nActive, nDeleted);
    else
        ImGui::TextDisabled("%d annotation(s)", nActive);

    ImGui::SameLine();
    // "Show deleted" toggle -- controls waveform renderer and panel list.
    bool bShowDel = m_pWaveform ? m_pWaveform->GetShowDeletedAnnotations() : false;
    if (ImGui::Checkbox("Show deleted##annshowdel", &bShowDel) && m_pWaveform)
        m_pWaveform->SetShowDeletedAnnotations(bShowDel);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show soft-deleted annotations with transparency");

    ImGui::Separator();

    // Table: Time | Type | Title | (buttons)
    static constexpr ImGuiTableFlags kTF =
        ImGuiTableFlags_RowBg          |
        ImGuiTableFlags_BordersInnerV  |
        ImGuiTableFlags_ScrollY        |
        ImGuiTableFlags_SizingFixedFit;

    const float fRowH  = ImGui::GetTextLineHeightWithSpacing();
    const float fAvail = ImGui::GetContentRegionAvail().y - fRowH;

    if (ImGui::BeginTable("##annTable", 4, kTF,
                          ImVec2(0.0f, fAvail > 40.0f ? fAvail : 40.0f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time",    ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Title",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 58.0f);
        ImGui::TableHeadersRow();

        const auto& all = m_pAnnotations->GetAll();

        // Track if an annotation was requested for deletion this frame.
        std::string sDeleteID;
        std::string sEditID;
        std::string sNavID;

        for (const Annotation& ann : all)
        {
            // Skip deleted annotations when "Show deleted" is off.
            const bool bShowDelNow = m_pWaveform &&
                                     m_pWaveform->GetShowDeletedAnnotations();
            if (ann.bDeleted && !bShowDelNow)
                continue;

            // Deleted rows are visually dimmed.
            if (ann.bDeleted)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.55f, 0.55f, 0.55f, 0.55f));

            ImGui::TableNextRow();

            // Time column -- also acts as the full-row double-click target.
            ImGui::TableSetColumnIndex(0);
            {
                time_t tSec = static_cast<time_t>(ann.dTimestamp);
                struct tm tmv;
#ifdef _WIN32
                localtime_s(&tmv, &tSec);
#else
                localtime_r(&tSec, &tmv);
#endif
                char tbuf[16];
                std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);

                // Selectable spanning the full row so the double-click can
                // be detected anywhere on the row, not just on the text.
                // ImGuiSelectableFlags_AllowOverlap lets the action buttons
                // in the last column receive their own click events normally.
                ImGui::Selectable(tbuf, false,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap);
                if (ImGui::IsItemHovered() &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    sNavID = ann.sID;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Double-click to navigate to this annotation");
            }

            // Type column (colored -- use dimmed color for deleted).
            ImGui::TableSetColumnIndex(1);
            {
                const ImVec4 tc = AnnotationStore::TypeColor(ann.eType);
                const float  fa = ann.bDeleted ? 0.40f : 1.0f;
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(tc.x, tc.y, tc.z, fa));
                if (ann.bDeleted)
                    ImGui::TextUnformatted("(deleted)");
                else
                    ImGui::TextUnformatted(AnnotationStore::TypeName(ann.eType));
                ImGui::PopStyleColor();
            }

            // Title column (tooltip shows description).
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(ann.sTitle.c_str());
            if (ImGui::IsItemHovered() && !ann.sDescription.empty())
            {
                // Width matches the description input field in the editor
                // (480 px window minus window padding on both sides).
                ImGui::SetNextWindowSize(
                    ImVec2(460.0f, 0.0f), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.8f);
                ImGui::BeginTooltip();
                ImGui::TextWrapped("%s", ann.sDescription.c_str());
                ImGui::EndTooltip();
            }

            // Actions column: [Ed] [Del] -- disabled for deleted annotations.
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(ann.sID.c_str());

            if (ann.bDeleted)
            {
                // Show placeholder so column width is consistent.
                ImGui::TextDisabled("--");
            }
            else
            {
                if (ImGui::SmallButton("Ed"))
                    sEditID = ann.sID;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Edit this annotation");

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.65f, 0.13f, 0.09f, 0.60f));
                if (ImGui::SmallButton("X"))
                    sDeleteID = ann.sID;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Delete this annotation");
            }

            ImGui::PopID();

            if (ann.bDeleted)
                ImGui::PopStyleColor();
        }

        ImGui::EndTable();

        // Handle deferred edit/delete (must be outside the table iteration).
        if (!sDeleteID.empty())
            m_pAnnotations->Delete(sDeleteID);

        // Navigate to annotation: pause auto-scroll and centre the waveform
        // on the annotated timestamp, keeping the current window width.
        if (!sNavID.empty() && m_pWaveform)
        {
            for (const Annotation& ann : all)
            {
                if (ann.sID == sNavID)
                {
                    m_bAutoScroll = false;
                    // ann.dTimestamp is absolute epoch; NavigateTo uses
                    // session-relative coordinates.
                    m_pWaveform->NavigateTo(
                        ann.dTimestamp - m_pWaveform->GetSessionOrigin());
                    break;
                }
            }
        }

        if (!sEditID.empty())
        {
            // Find the annotation to populate editor fields.
            for (const Annotation& ann : all)
            {
                if (ann.sID == sEditID)
                {
                    m_bAnnEditorIsNew = false;
                    m_dAnnEditorTime  = ann.dTimestamp;
                    m_iAnnType        = static_cast<int>(ann.eType);
                    std::strncpy(m_szAnnTitle, ann.sTitle.c_str(),
                                 sizeof(m_szAnnTitle) - 1);
                    m_szAnnTitle[sizeof(m_szAnnTitle) - 1] = '\0';
                    std::strncpy(m_szAnnDesc, ann.sDescription.c_str(),
                                 sizeof(m_szAnnDesc) - 1);
                    m_szAnnDesc[sizeof(m_szAnnDesc) - 1] = '\0';
                    m_sAnnEditID      = sEditID;
                    m_bShowAnnEditor  = true;
                    break;
                }
            }
        }
    }

    if (!bOpen) m_bShowAnnPanel = false;
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Counter list (placeholder -- proper tree in P3)
// ---------------------------------------------------------------------------

void ScopeWindow::RenderCounterList()
{
    if (m_pCounterTree)
        m_pCounterTree->Render(m_pWaveform.get(), m_pStats.get());
}

// ---------------------------------------------------------------------------
// Waveform area -- delegates to WaveformRenderer
// ---------------------------------------------------------------------------

void ScopeWindow::RenderWaveformArea()
{
    if (!m_pWaveform)
        return;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    double dTimeWindow = m_pConfig ? m_pConfig->Get().dRealtimeTimeWin : 15.0;

    bool bUserInteracted = m_pWaveform->Render(avail, m_bAutoScroll,
                                               dTimeWindow);

    // bUserInteracted is only true for pan (left-drag), not for scroll-zoom.
    // Zoom while live keeps the session live; only panning navigates away.
    if (bUserInteracted && m_bAutoScroll)
        m_bAutoScroll = false;
}

// ---------------------------------------------------------------------------
// DrainAllRingBuffers -- move samples from SPSC rings into WaveformRenderer
// ---------------------------------------------------------------------------

void ScopeWindow::DrainAllRingBuffers()
{
    if (!m_pConsumer || !m_pWaveform)
        return;

    auto keys = m_pConsumer->GetCounterKeys();
    for (const auto& [dwHash, wID] : keys)
    {
        auto* pRing = m_pConsumer->GetRingBuffer(dwHash, wID);
        m_pWaveform->DrainRingBuffer(dwHash, wID, pRing);
    }
}

// ---------------------------------------------------------------------------
// Session liveness update
// ---------------------------------------------------------------------------

void ScopeWindow::SetLiveness(SessionLiveness e)
{
    m_eLiveness = e;
    bool bNowOnline = (e == SessionLiveness::Live ||
                       e == SessionLiveness::GoingOffline);
    if (m_bOnline != bNowOnline)
    {
        m_bOnline = bNowOnline;
        if (m_pCounterTree)
            m_pCounterTree->SetOnline(bNowOnline);
        if (!bNowOnline)
            m_bAutoScroll = false;
    }
}

// ---------------------------------------------------------------------------
// NotifyMetaUpdated -- non-blocking post; actual reinit deferred to Render()
// ---------------------------------------------------------------------------

void ScopeWindow::NotifyMetaUpdated(const kv8::SessionMeta& meta)
{
    // If a reinit is already pending (e.g. two MetaUpdated events in the same
    // frame), just update the pending meta with the richer version.
    m_pendingMeta    = meta;
    m_bReinitPending = true;
    // Signal the current consumer to stop -- it will exit gracefully on its
    // own without us needing to Join() here on the render thread.
    if (m_pConsumer)
        m_pConsumer->RequestStop();
}

// ---------------------------------------------------------------------------
// Per-window status bar (placeholder metrics)
// ---------------------------------------------------------------------------

void ScopeWindow::RenderScopeStatusBar()
{
    // Update displayed FPS once per second.
    {
        float fNowFps = static_cast<float>(ImGui::GetTime());
        if (fNowFps - m_fPrevFpsTime >= 1.0f)
        {
            float dt = ImGui::GetIO().DeltaTime;
            m_fDisplayedFps  = (dt > 0.0f) ? (1.0f / dt) : 0.0f;
            m_fPrevFpsTime   = fNowFps;
        }
    }

    if (!m_pConsumer)
    {
        ImGui::TextDisabled("No consumer | 0 msg/s | %.0f fps",
                            m_fDisplayedFps);
        return;
    }

    // Update msg/s rate every ~500 ms.
    float fNow = static_cast<float>(ImGui::GetTime());
    if (fNow - m_fPrevRateTime >= 0.5f && fNow > m_fPrevRateTime)
    {
        uint64_t nNow = m_pConsumer->GetTotalMessages();
        float dt = fNow - m_fPrevRateTime;
        m_fMsgRate = static_cast<float>(nNow - m_nPrevMsgCount) / dt;
        m_nPrevMsgCount = nNow;
        m_fPrevRateTime = fNow;
    }

    // Status string derived from the session liveness state maintained
    // by SessionManager and pushed here via SetLiveness().
    const char* sStatus = "Connecting";
    if (m_pConsumer->IsConnected())
    {
        switch (m_eLiveness)
        {
        case SessionLiveness::Live:         sStatus = "Live";       break;
        case SessionLiveness::GoingOffline: sStatus = "Live?";      break;
        case SessionLiveness::Offline:      sStatus = "Offline";    break;
        case SessionLiveness::Historical:   sStatus = "Historical"; break;
        default:                            sStatus = "Waiting";    break;
        }
    }

    uint64_t nDrop = m_pConsumer->GetDropCount();
    if (nDrop > 0)
        ImGui::TextDisabled("%s | %.0f msg/s | %llu dropped | Lag: N/A | %.0f fps",
                            sStatus, m_fMsgRate,
                            (unsigned long long)nDrop,
                            m_fDisplayedFps);
    else
        ImGui::TextDisabled("%s | %.0f msg/s | Lag: N/A | %.0f fps",
                            sStatus, m_fMsgRate,
                            m_fDisplayedFps);
}
