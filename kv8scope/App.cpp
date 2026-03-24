// kv8scope -- Kv8 Software Oscilloscope
// App.cpp -- Top-level application: DockSpace, menu bar, session panel,
//            status bar.

#include "App.h"
#include "ConfigStore.h"
#include "ScopeWindow.h"
#include "SessionListPanel.h"
#include "SessionManager.h"
#include "ThemeManager.h"

#include "imgui.h"
#include "imgui_internal.h"  // ImGuiDockNodeFlags_PassthruCentralNode
#include "implot.h"

#include <algorithm>
#include <cstdio>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

App::App(const std::string& sConfigPath)
    : m_pConfig(std::make_unique<ConfigStore>(sConfigPath))
{
    m_pConfig->Load();

    // Fonts are built by main.cpp after ImGui is initialized.
    // FontManager must exist here so the menu can query active face/size.
    m_pFonts = std::make_unique<FontManager>();
    FontManager::SetInstance(m_pFonts.get());

    m_pTheme = std::make_unique<ThemeManager>();
    m_pTheme->Apply(m_pConfig->Get().sTheme);

    m_pSessions = std::make_unique<SessionManager>(*m_pConfig);
    m_pSessions->Start();

    m_pSessionPanel = std::make_unique<SessionListPanel>();
    m_pSessionPanel->SetBackends(m_pSessions.get(), m_pConfig.get());
    m_pSessionPanel->SetTheme(&m_pTheme->Colors());

    m_pConnDialog = std::make_unique<ConnectionDialog>();
    m_pConnDialog->SetBackends(m_pConfig.get());
    m_pConnDialog->SetOnAccept([this]()
    {
        // Restart discovery with the new connection settings.
        m_scopeWindows.clear();  // close all open scope windows
        m_pSessionPanel->Clear();
        m_eConnState = ConnState::Disconnected;
        m_sConnError.clear();
        m_pSessions->Restart();
    });
}

App::~App()
{
    m_scopeWindows.clear();
    m_pConnDialog.reset();
    m_pSessionPanel.reset();
    m_pSessions->Stop();
    m_pConfig->Save();
}

// ---------------------------------------------------------------------------
// Render -- called once per frame
// ---------------------------------------------------------------------------

void App::Render()
{
    PollSessionEvents();

    RenderDockSpace();
    RenderMenuBar();
    m_pSessionPanel->Render();

    // Render all open scope windows; remove closed ones.
    for (auto it = m_scopeWindows.begin(); it != m_scopeWindows.end(); )
    {
        if (!(*it)->Render())
            it = m_scopeWindows.erase(it);
        else
            ++it;
    }

    RenderStatusBar();

    m_pConnDialog->Render();

    if (m_bShowAbout)
    {
        RenderAboutPopup();
    }
}

// ---------------------------------------------------------------------------
// DockSpace -- full-viewport docking area
// ---------------------------------------------------------------------------

void App::RenderDockSpace()
{
    // Create a full-viewport DockSpace that fills the main window area
    // below the menu bar.
    const ImGuiViewport* pViewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(pViewport->WorkPos);
    ImGui::SetNextWindowSize(pViewport->WorkSize);
    ImGui::SetNextWindowViewport(pViewport->ID);

    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDocking        |
        ImGuiWindowFlags_NoTitleBar       |
        ImGuiWindowFlags_NoCollapse       |
        ImGuiWindowFlags_NoResize         |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus       |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("##DockSpaceHost", nullptr, windowFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspaceId = ImGui::GetID("kv8scopeDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Main menu bar
// ---------------------------------------------------------------------------

void App::RenderMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Connect..."))
            {
                m_pConnDialog->Open();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
            {
                m_bWantsExit = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::BeginMenu("Theme"))
            {
                const std::string& sActive = m_pTheme->ActiveTheme();

                if (ImGui::MenuItem("Night Sky",     nullptr,
                                    sActive == "night_sky"))
                {
                    m_pTheme->Apply("night_sky");
                    m_pConfig->GetMut().sTheme = "night_sky";
                    m_pConfig->MarkDirty();
                }
                if (ImGui::MenuItem("Sahara Desert", nullptr,
                                    sActive == "sahara_desert"))
                {
                    m_pTheme->Apply("sahara_desert");
                    m_pConfig->GetMut().sTheme = "sahara_desert";
                    m_pConfig->MarkDirty();
                }
                if (ImGui::MenuItem("Ocean Deep",    nullptr,
                                    sActive == "ocean_deep"))
                {
                    m_pTheme->Apply("ocean_deep");
                    m_pConfig->GetMut().sTheme = "ocean_deep";
                    m_pConfig->MarkDirty();
                }
                if (ImGui::MenuItem("Classic Dark",  nullptr,
                                    sActive == "classic_dark"))
                {
                    m_pTheme->Apply("classic_dark");
                    m_pConfig->GetMut().sTheme = "classic_dark";
                    m_pConfig->MarkDirty();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Morning Light", nullptr,
                                    sActive == "morning_light"))
                {
                    m_pTheme->Apply("morning_light");
                    m_pConfig->GetMut().sTheme = "morning_light";
                    m_pConfig->MarkDirty();
                }
                if (ImGui::MenuItem("Blizzard",      nullptr,
                                    sActive == "blizzard"))
                {
                    m_pTheme->Apply("blizzard");
                    m_pConfig->GetMut().sTheme = "blizzard";
                    m_pConfig->MarkDirty();
                }
                if (ImGui::MenuItem("Classic Light",  nullptr,
                                    sActive == "classic_light"))
                {
                    m_pTheme->Apply("classic_light");
                    m_pConfig->GetMut().sTheme = "classic_light";
                    m_pConfig->MarkDirty();
                }
                if (ImGui::MenuItem("Paper White",    nullptr,
                                    sActive == "paper_white"))
                {
                    m_pTheme->Apply("paper_white");
                    m_pConfig->GetMut().sTheme = "paper_white";
                    m_pConfig->MarkDirty();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Inferno",       nullptr,
                                    sActive == "inferno"))
                {
                    m_pTheme->Apply("inferno");
                    m_pConfig->GetMut().sTheme = "inferno";
                    m_pConfig->MarkDirty();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Neon Bubble Gum",        nullptr,
                                    sActive == "neon_bubble_gum"))
                {
                    m_pTheme->Apply("neon_bubble_gum");
                    m_pConfig->GetMut().sTheme = "neon_bubble_gum";
                    m_pConfig->MarkDirty();
                }
                if (ImGui::MenuItem("Psychedelic Delirium",   nullptr,
                                    sActive == "psychedelic_delirium"))
                {
                    m_pTheme->Apply("psychedelic_delirium");
                    m_pConfig->GetMut().sTheme = "psychedelic_delirium";
                    m_pConfig->MarkDirty();
                }
                ImGui::EndMenu();
            }
            // Font face and size
            if (ImGui::BeginMenu("Font"))
            {
                const std::string& sActiveFace = m_pFonts->ActiveFace();
                const float fActiveSize        = m_pFonts->ActiveSize();

                ImGui::TextDisabled("Face");
                ImGui::Separator();
                for (int i = 0; i < FontManager::k_nFaces; ++i)
                {
                    const bool bActive =
                        (sActiveFace == FontManager::k_szFaceIds[i]);
                    if (ImGui::MenuItem(FontManager::k_szFaceLabels[i],
                                        nullptr, bActive))
                    {
                        m_pConfig->GetMut().sFontFace =
                            FontManager::k_szFaceIds[i];
                        m_pConfig->MarkDirty();
                        m_pFonts->RequestRebuild(
                            m_pConfig->Get().sFontFace,
                            static_cast<float>(m_pConfig->Get().iFontSize));
                    }
                }

                ImGui::Spacing();
                ImGui::TextDisabled("Size");
                ImGui::Separator();
                for (int i = 0; i < FontManager::k_nSizes; ++i)
                {
                    const bool bActive =
                        (fActiveSize == FontManager::k_fSizes[i]);
                    if (ImGui::MenuItem(FontManager::k_szSizeLabels[i],
                                        nullptr, bActive))
                    {
                        m_pConfig->GetMut().iFontSize =
                            static_cast<int>(FontManager::k_fSizes[i]);
                        m_pConfig->MarkDirty();
                        m_pFonts->RequestRebuild(
                            m_pConfig->Get().sFontFace,
                            static_cast<float>(m_pConfig->Get().iFontSize));
                    }
                }
                ImGui::EndMenu();
            }

            // P4.4: Statistics panel toggle
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About"))
            {
                m_bShowAbout = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

// ---------------------------------------------------------------------------
// Status bar -- thin window anchored to the bottom
// ---------------------------------------------------------------------------

void App::RenderStatusBar()
{
    const ImGuiViewport* pViewport = ImGui::GetMainViewport();
    const float fBarHeight = ImGui::GetFrameHeight() + 2.0f;

    ImGui::SetNextWindowPos(
        ImVec2(pViewport->WorkPos.x,
               pViewport->WorkPos.y + pViewport->WorkSize.y - fBarHeight));
    ImGui::SetNextWindowSize(
        ImVec2(pViewport->WorkSize.x, fBarHeight));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration    |
        ImGuiWindowFlags_NoInputs        |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 2.0f));

    if (ImGui::Begin("##StatusBar", nullptr, flags))
    {
        const ThemeColors& tc = m_pTheme->Colors();

        switch (m_eConnState)
        {
        case ConnState::Connected:
            ImGui::TextColored(tc.onlineBadge, "Connected");
            ImGui::SameLine();
            ImGui::TextColored(tc.textSecondary,
                               " | %d session(s)",
                               static_cast<int>(m_pSessionPanel->SessionCount()));
            if (!m_scopeWindows.empty())
            {
                ImGui::SameLine();
                ImGui::TextColored(tc.textSecondary,
                                   " | %d scope(s)",
                                   static_cast<int>(m_scopeWindows.size()));
            }
            break;

        case ConnState::Error:
            ImGui::TextColored(ImVec4(0.84f, 0.37f, 0.0f, 1.0f),  // #d55e00
                               "Error: %s", m_sConnError.c_str());
            break;

        case ConnState::Disconnected:
        default:
            ImGui::TextColored(tc.offlineBadge, "Disconnected");
            break;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ---------------------------------------------------------------------------
// PollSessionEvents -- drain SessionManager queue each frame
// ---------------------------------------------------------------------------

void App::PollSessionEvents()
{
    if (!m_pSessions)
        return;

    std::vector<SessionEvent> events;
    m_pSessions->DrainEvents(events);

    for (auto& ev : events)
    {
        switch (ev.eType)
        {
        case SessionEvent::Connected:
            m_eConnState = ConnState::Connected;
            m_sConnError.clear();
            m_pSessionPanel->SetConnectionHint("No sessions discovered.");
            break;

        case SessionEvent::ConnectionError:
            m_eConnState = ConnState::Error;
            m_sConnError = std::move(ev.sMessage);
            m_pSessionPanel->SetConnectionHint(
                "Broker error (see status bar).");
            break;

        case SessionEvent::Appeared:
            m_pSessionPanel->AddSession(ev.sChannel,
                                        ev.sSessionPrefix,
                                        ev.meta);
            break;

        case SessionEvent::MetaUpdated:
            // A live session gained new UDT virtual fields after the first
            // discovery scan -- update the stored meta in the session list
            // and reinitialise any open ScopeWindow so it subscribes to the
            // new data topics and renders the new counters.
            m_pSessionPanel->UpdateSessionMeta(ev.sSessionPrefix, ev.meta);
            for (auto& pWin : m_scopeWindows)
            {
                if (pWin->Prefix() == ev.sSessionPrefix)
                {
                    pWin->NotifyMetaUpdated(ev.meta);
                    break;
                }
            }
            break;

        case SessionEvent::Disappeared:
            m_pSessionPanel->RemoveSession(ev.sSessionPrefix);
            break;

        case SessionEvent::StatusChanged:
            m_pSessionPanel->SetSessionLiveness(ev.sSessionPrefix,
                                                ev.liveness.eState);
            // Also update any open ScopeWindow for this session.
            for (auto& pWin : m_scopeWindows)
            {
                if (pWin->Prefix() == ev.sSessionPrefix)
                {
                    pWin->SetLiveness(ev.liveness.eState);
                    break;
                }
            }
            break;
        }
    }

    // Consume open requests -- create or raise a ScopeWindow.
    std::string sPrefix;
    kv8::SessionMeta meta;
    SessionLiveness eLiveness = SessionLiveness::Unknown;
    while (m_pSessionPanel->ConsumeOpenRequest(sPrefix, meta, eLiveness))
    {
        OnOpenSession(sPrefix, meta, eLiveness);
    }
}

// ---------------------------------------------------------------------------
// OnOpenSession -- create or raise a ScopeWindow
// ---------------------------------------------------------------------------

void App::OnOpenSession(const std::string& sPrefix,
                        const kv8::SessionMeta& meta,
                        SessionLiveness eLiveness)
{
    // If already open, just raise it.
    for (auto& pWin : m_scopeWindows)
    {
        if (pWin->Prefix() == sPrefix)
        {
            pWin->Focus();
            return;
        }
    }

    const bool bOnline = (eLiveness == SessionLiveness::Live ||
                          eLiveness == SessionLiveness::GoingOffline);

    // Create a new scope window.  Pass bOnline so the window knows whether
    // to start in auto-scroll (live) or free-pan (historical) mode.
    auto pWin = std::make_unique<ScopeWindow>(sPrefix, meta, m_pConfig.get(), bOnline);
    pWin->SetLiveness(eLiveness);
    m_scopeWindows.push_back(std::move(pWin));

    std::fprintf(stderr, "[kv8scope] Opened scope: %s (online=%s)\n",
                 sPrefix.c_str(), bOnline ? "yes" : "no");
}

// ---------------------------------------------------------------------------
// Font lifecycle helpers (called from main.cpp)
// ---------------------------------------------------------------------------

void App::BuildInitialFonts()
{
    m_pFonts->Build(m_pConfig->Get().sFontFace,
                    static_cast<float>(m_pConfig->Get().iFontSize));
}

bool App::FontsNeedRebuild() const
{
    return m_pFonts && m_pFonts->NeedsRebuild();
}

void App::DoFontRebuild()
{
    if (m_pFonts)
        m_pFonts->DoRebuild();
}

// ---------------------------------------------------------------------------
// About popup
// ---------------------------------------------------------------------------

void App::RenderAboutPopup()
{
    ImGui::OpenPopup("About kv8scope");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("About kv8scope", &m_bShowAbout,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("kv8scope -- Kv8 Software Oscilloscope");
        ImGui::Separator();
        ImGui::Text("Built with Dear ImGui %s", IMGUI_VERSION);
        ImGui::Text("ImPlot %s", IMPLOT_VERSION);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Haiku by the machine that built this.
        if (FontManager* pFM = FontManager::Get()) pFM->PushItalic();
        ImGui::TextDisabled("Bits flow, pixels bake");
        ImGui::TextDisabled("Atlas clears, fonts bloom anew");
        ImGui::TextDisabled("Scale set. Frame complete.");
        if (FontManager::Get()) FontManager::PopFont();
        ImGui::Spacing();
        ImGui::TextDisabled("  -- GitHub Copilot");
        ImGui::Spacing();

        if (ImGui::Button("OK", ImVec2(120.0f, 0.0f)))
        {
            m_bShowAbout = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
