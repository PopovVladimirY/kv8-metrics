// kv8scope -- Kv8 Software Oscilloscope
// App.h -- Top-level application class owning all state and rendering.

#pragma once

#include "ConfigStore.h"
#include "ConnectionDialog.h"
#include "FontManager.h"
#include "ScopeWindow.h"
#include "SessionListPanel.h"
#include "SessionManager.h"
#include "ThemeManager.h"
#include "imgui.h"

#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

class App
{
public:
    explicit App(const std::string& sConfigPath);
    ~App();

    // Non-copyable, non-movable
    App(const App&)            = delete;
    App& operator=(const App&) = delete;
    App(App&&)                 = delete;
    App& operator=(App&&)      = delete;

    // Called once per frame from the main loop.
    void Render();

    // Called by main.cpp after ImGui is initialised to load the initial font atlas.
    void BuildInitialFonts();

    // Called by main.cpp between frames when fonts need a GPU texture rebuild.
    bool FontsNeedRebuild()  const;
    void DoFontRebuild();

    // Returns true when File > Exit was chosen.
    bool WantsExit() const { return m_bWantsExit; }

private:
    void RenderMenuBar();
    void RenderDockSpace();
    void RenderStatusBar();
    void RenderAboutPopup();
    void PollSessionEvents();
    void OnOpenSession(const std::string& sPrefix,
                       const kv8::SessionMeta& meta,
                       SessionLiveness eLiveness);

    std::unique_ptr<ConfigStore>      m_pConfig;
    std::unique_ptr<FontManager>      m_pFonts;
    std::unique_ptr<ThemeManager>     m_pTheme;
    std::unique_ptr<SessionManager>   m_pSessions;
    std::unique_ptr<SessionListPanel> m_pSessionPanel;
    std::unique_ptr<ConnectionDialog> m_pConnDialog;

    std::vector<std::unique_ptr<ScopeWindow>> m_scopeWindows;

    bool        m_bWantsExit      = false;
    bool        m_bShowAbout      = false;

    // Connection state for status bar.
    enum class ConnState { Disconnected, Connected, Error };
    ConnState   m_eConnState      = ConnState::Disconnected;
    std::string m_sConnError;
};
