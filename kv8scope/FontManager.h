// kv8scope -- Kv8 Software Oscilloscope
// FontManager.h -- Font face / size selection, atlas build, runtime reload.
//
// Face options (config key -> display label):
//   "sans_serif"      -> "Sans Serif"      (Segoe UI / DejaVu Sans)
//   "arial_nova"      -> "Arial Nova"      (Arial Nova / Arial / DejaVu Sans)
//   "aptos"           -> "Aptos Semibold"  (Aptos / Segoe UI / DejaVu Sans)
//   "classic_console" -> "Classic Console" (Consolas / DejaVu Sans Mono)
//   "gothic"          -> "Gothic"          (Old English Text MT / Gabriola)
//   "script"          -> "Script"          (Segoe Script / Lucida Handwriting)

#pragma once

#include "imgui.h"

#include <string>

// ---------------------------------------------------------------------------
// FontManager
// ---------------------------------------------------------------------------

class FontManager
{
public:
    // Available size options (pixels).
    static constexpr float k_fSizes[]  = { 12.0f, 17.0f, 22.0f, 27.0f, 32.0f };
    static constexpr int   k_nSizes    = 5;
    static constexpr const char* k_szSizeLabels[] =
        { "Small (12)", "Normal (17)", "Large (22)", "X-Large (27)", "XX-Large (32)" };

    // Atlas is always baked at this size; io.FontGlobalScale handles the rest.
    static constexpr float k_fBaseSize = 22.0f;

    // Available face options.
    static constexpr const char* k_szFaceIds[]    =
        { "sans_serif", "arial_nova", "aptos", "classic_console",
          "gothic", "script" };
    static constexpr const char* k_szFaceLabels[] =
        { "Sans Serif", "Arial Nova", "Aptos Semibold", "Classic Console",
          "Gothic", "Script" };
    static constexpr int k_nFaces = 6;

    FontManager() = default;

    // ---------------------------------------------------------------------------
    // Build phase (call before first frame, atlas must be empty)
    // Adds all four variants (R/B/I/BI) to the ImGui font atlas and sets the
    // default font.  Caller must invoke io.Fonts->Build() (or rely on the
    // backend NewFrame to do it) after this.
    // ---------------------------------------------------------------------------
    void Build(const std::string& sFace, float fSize);

    // ---------------------------------------------------------------------------
    // Runtime reload (between frames)
    // Call RequestRebuild() when the user changes settings, then check
    // NeedsRebuild() in main.cpp.  The main loop should:
    //   ImGui_ImplOpenGL3_DestroyFontsTexture();
    //   fontMgr.DoRebuild();
    //   ImGui_ImplOpenGL3_CreateFontsTexture();
    // ---------------------------------------------------------------------------
    void RequestRebuild(const std::string& sFace, float fSize);
    bool NeedsRebuild() const { return m_bNeedsRebuild; }
    void DoRebuild();   // clears atlas, calls Build(), calls io.Fonts->Build()

    // ---------------------------------------------------------------------------
    // Font access
    // ---------------------------------------------------------------------------
    ImFont* Regular()    const { return m_pRegular; }
    ImFont* Bold()       const { return m_pBold    ? m_pBold    : m_pRegular; }
    ImFont* Italic()     const { return m_pItalic  ? m_pItalic  : m_pRegular; }
    ImFont* BoldItalic() const { return m_pBoldItalic ? m_pBoldItalic
                                                       : Bold(); }

    // Convenience push/pop (safe even when a variant is unavailable).
    void PushBold()       const { ImGui::PushFont(Bold()); }
    void PushItalic()     const { ImGui::PushFont(Italic()); }
    void PushBoldItalic() const { ImGui::PushFont(BoldItalic()); }
    static void PopFont() { ImGui::PopFont(); }

    // ---------------------------------------------------------------------------
    // State queries
    // ---------------------------------------------------------------------------
    const std::string& ActiveFace() const { return m_sActiveFace; }
    float              ActiveSize() const { return m_fActiveSize; }

    // ---------------------------------------------------------------------------
    // Global singleton -- set & get
    // (Set by App on construction so sub-components can call FontManager::Get())
    // ---------------------------------------------------------------------------
    static void         SetInstance(FontManager* p) { s_pInstance = p; }
    static FontManager* Get()                       { return s_pInstance; }

private:
    // Try candidate paths in order; return the first that exists, or "".
    static std::string FindFile(std::initializer_list<const char*> candidates);

    // Load one font variant from a set of candidate paths into the atlas.
    // Returns nullptr if no candidate file exists (fallback applied by caller).
    static ImFont* TryLoad(std::initializer_list<const char*> candidates,
                           float fSize,
                           bool bBold = false);

    void           DoLoad(const std::string& sFace, float fSize);

    ImFont* m_pRegular    = nullptr;
    ImFont* m_pBold       = nullptr;
    ImFont* m_pItalic     = nullptr;
    ImFont* m_pBoldItalic = nullptr;

    std::string m_sActiveFace   = "sans_serif";
    float       m_fActiveSize   = k_fBaseSize;
    bool        m_bNeedsRebuild = false;
    std::string m_sPendingFace;
    float       m_fPendingSize  = k_fBaseSize;

    static FontManager* s_pInstance;
};
