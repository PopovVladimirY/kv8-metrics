// kv8scope -- Kv8 Software Oscilloscope
// ScopeWindow.h -- One scope window per opened session.
//
// Manages the ImGui window, toolbar, counter list, waveform area
// (rendered by WaveformRenderer via ImPlot), and per-window status bar.
// Owns a ConsumerThread for Kafka ingestion and a WaveformRenderer for
// chart drawing.

#pragma once

#include "AnnotationStore.h"
#include "SessionManager.h"
#include <kv8/IKv8Producer.h>
#include <kv8/Kv8Types.h>

#include <cstdint>
#include <memory>
#include <string>

class ConfigStore;
class ConsumerThread;
class CounterTree;
class StatsEngine;
class StatsPanel;
class WaveformRenderer;

class ScopeWindow
{
public:
    /// Construct a scope window for the given session.
    /// @param sPrefix  "<channel>.<sessionID>" -- used as the unique key.
    /// @param meta     Full session metadata from the discovery scan.
    /// @param pConfig  Read-only config reference (time window, etc.).
    /// @param bOnline  true if a live producer is running for this session.
    ///                 Enables auto-scroll mode.  false for historical sessions
    ///                 (starts with the full range visible, auto-scroll off).
    ScopeWindow(const std::string& sPrefix,
                const kv8::SessionMeta& meta,
                ConfigStore* pConfig,
                bool bOnline = false);
    ~ScopeWindow();

    // Non-copyable, non-movable
    ScopeWindow(const ScopeWindow&)            = delete;
    ScopeWindow& operator=(const ScopeWindow&) = delete;
    ScopeWindow(ScopeWindow&&)                 = delete;
    ScopeWindow& operator=(ScopeWindow&&)      = delete;

    /// Render the ImGui window.  Returns false when the user closed the
    /// window (the caller should destroy this instance).
    bool Render();

    /// Raise / bring this window to front (for reuse when double-clicking
    /// the same session again).
    void Focus();

    /// Update the session liveness state.
    /// Drives the E-column visibility in CounterTree and the auto-scroll
    /// mode; also updates m_bOnline for backward compat with the status bar.
    void SetLiveness(SessionLiveness eLiveness);

    /// Post a MetaUpdated notification.  The actual reinitialisation happens
    /// at the start of the next Render() frame so the render thread is never
    /// stalled waiting for a ConsumerThread::Join().
    void NotifyMetaUpdated(const kv8::SessionMeta& meta);

    /// Session prefix that uniquely identifies this window.
    const std::string& Prefix() const { return m_sPrefix; }

    /// Is the window still open?
    bool IsOpen() const { return m_bOpen; }

private:
    void RenderToolbar();
    void RenderCounterList();
    void RenderWaveformArea();
    void RenderScopeStatusBar();

    /// Drain all per-counter ring buffers into the WaveformRenderer.
    void DrainAllRingBuffers();

    std::string        m_sPrefix;
    std::string        m_sWindowTitle;  // "Scope: <sessionID>"
    kv8::SessionMeta   m_meta;
    ConfigStore*       m_pConfig = nullptr;

    bool m_bOpen       = true;   // window open flag (passed to ImGui::Begin)
    bool m_bFocusNext  = false;  // set by Focus(), consumed next frame

    bool m_bOnline     = false;  // session type: true = live producer running
    SessionLiveness m_eLiveness = SessionLiveness::Unknown; // current liveness state

    // Deferred reinit state: set by NotifyMetaUpdated(), consumed at the top
    // of the next Render() frame to avoid blocking the render thread on Join().
    bool             m_bReinitPending = false;
    kv8::SessionMeta m_pendingMeta;
    // Old consumer kept alive until Join() completes at the start of Render().
    std::unique_ptr<ConsumerThread> m_pOldConsumer;

    // Toolbar state
    bool m_bAutoScroll = true;   // [>] / [||] toggle

    // Consumer thread (P2.2) -- owns IKv8Consumer + per-counter ring buffers.
    std::unique_ptr<ConsumerThread> m_pConsumer;

    // Waveform renderer (P2.3) -- drains ring buffers, draws ImPlot chart.
    std::unique_ptr<WaveformRenderer> m_pWaveform;

    // Counter tree (P3.1) -- multi-column table with V/E checkboxes,
    // color swatches, and per-counter statistics.
    std::unique_ptr<CounterTree> m_pCounterTree;

    // Stats engine (P4.1) -- full-duration running accumulators fed by
    // consumer thread (O(1) per sample via atomic CAS).
    std::unique_ptr<StatsEngine> m_pStats;

    // Statistics panel (P4.4) -- dockable table with all stats columns.
    // Toggled with Ctrl+I.
    std::unique_ptr<StatsPanel> m_pStatsPanel;
    bool m_bShowStats = false;

    // Msg/s rate tracking for the status bar.
    uint64_t m_nPrevMsgCount  = 0;
    float    m_fPrevRateTime  = 0.0f;
    float    m_fMsgRate       = 0.0f;

    // FPS display -- updated at most once per second to avoid flicker.
    float    m_fPrevFpsTime   = 0.0f;
    float    m_fDisplayedFps  = 0.0f;

    // ---- CSV Export dialog state ----------------------------------------
    bool     m_bShowExportDialog = false;
    int      m_iExportRange      = 0;    // 0 = visible window, 1 = entire duration
    char     m_szExportPath[1024] = {};
    char     m_szExportStatus[256] = {}; // last result message
    void RenderExportDialog();

    // ---- Annotation store -----------------------------------------------
    // Owned by this ScopeWindow; the raw pointer is shared with WaveformRenderer.
    std::unique_ptr<AnnotationStore>  m_pAnnotations;

    // Kafka producer for persisting annotations to the _annotations topic.
    // Created once in the constructor; nullptr if the broker is unreachable.
    std::unique_ptr<kv8::IKv8Producer> m_pAnnProducer;

    // Kafka producer for writing counter enable/disable commands to the
    // <session>.ctrl topic.  Created once in the constructor.
    std::unique_ptr<kv8::IKv8Producer> m_pCtrlProducer;
    std::string m_sCtrlTopic; // <sessionPrefix>._ctl

    // Annotation topic name: <channel>.<sessionID>._annotations
    std::string m_sAnnotationTopic;

    // ---- Annotation editor state ----------------------------------------
    bool     m_bShowAnnEditor   = false; // editor popup visible
    bool     m_bAnnEditorIsNew  = true;  // true = create, false = edit existing
    double   m_dAnnEditorTime   = 0.0;   // timestamp of the annotation being edited
    char     m_szAnnTitle[128]  = {};    // title input buffer
    char     m_szAnnDesc[512]   = {};    // description input buffer
    int      m_iAnnType         = 0;     // AnnotationType index
    std::string m_sAnnEditID;            // ID of annotation being edited
    void RenderAnnotationEditor();

    // ---- Annotation panel state -----------------------------------------
    bool     m_bShowAnnPanel    = false; // annotation panel popup visible
    void RenderAnnotationPanel();
};
