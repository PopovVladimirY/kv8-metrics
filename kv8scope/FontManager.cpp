// kv8scope -- Kv8 Software Oscilloscope
// FontManager.cpp -- Font atlas build and runtime reload.

#include "FontManager.h"

#include "imgui.h"

#include <filesystem>
#include <cstdio>

// ---------------------------------------------------------------------------
// Static singleton
// ---------------------------------------------------------------------------

FontManager* FontManager::s_pInstance = nullptr;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

std::string FontManager::FindFile(std::initializer_list<const char*> candidates)
{
    for (const char* path : candidates)
    {
        if (path && std::filesystem::exists(path))
            return path;
    }
    return {};
}

// Load one font variant.  Returns nullptr if no file found (caller applies fallback).
// bBold: set to true to load with slightly heavier oversample for bold simulation
// when no true-bold file is available (unused but kept for future use by callers).
ImFont* FontManager::TryLoad(std::initializer_list<const char*> candidates,
                              float fSize, bool /*bBold*/)
{
    std::string sPath = FindFile(candidates);
    if (sPath.empty())
        return nullptr;

    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = false;
    return io.Fonts->AddFontFromFileTTF(sPath.c_str(), fSize, &cfg, nullptr);
}

// ---------------------------------------------------------------------------
// DoLoad -- add all four variants for the requested face, baked at k_fBaseSize.
// io.FontGlobalScale is set to fSize/k_fBaseSize so the entire UI scales.
//
// Font file search order: preferred paths first, generic fallbacks last.
// All paths tried on both Windows and Linux; FindFile() skips missing ones.
// ---------------------------------------------------------------------------

void FontManager::DoLoad(const std::string& sFace, float fSize)
{
    m_pRegular    = nullptr;
    m_pBold       = nullptr;
    m_pItalic     = nullptr;
    m_pBoldItalic = nullptr;

    // Always bake at the base size; FontGlobalScale is set below.
    const float fLoad = k_fBaseSize;

    if (sFace == "classic_console")
    {
        // ---- Classic Console (monospace) ---------------------------------
        m_pRegular = TryLoad({
            // Windows
            "C:/Windows/Fonts/consola.ttf",
            // Linux
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        }, fLoad);

        m_pBold = TryLoad({
            "C:/Windows/Fonts/consolab.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
            "/usr/share/fonts/liberation/LiberationMono-Bold.ttf",
        }, fLoad, true);

        m_pItalic = TryLoad({
            "C:/Windows/Fonts/consolai.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Oblique.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono-Oblique.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Italic.ttf",
        }, fLoad);

        m_pBoldItalic = TryLoad({
            "C:/Windows/Fonts/consolaz.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-BoldOblique.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono-BoldOblique.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-BoldItalic.ttf",
        }, fLoad, true);
    }
    else if (sFace == "arial_nova")
    {
        // ---- Arial Nova (Windows 10+ system font) -------------------------
        // Arial Nova has Light / Regular / Cond variants.
        // Fall back through Arial -> Liberation Sans -> DejaVu Sans.
        m_pRegular = TryLoad({
            "C:/Windows/Fonts/arialnova.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        }, fLoad);

        m_pBold = TryLoad({
            "C:/Windows/Fonts/arialnovabd.ttf",
            "C:/Windows/Fonts/arialnova.ttf",   // no dedicated bold on some systems
            "C:/Windows/Fonts/arialbd.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        }, fLoad, true);

        m_pItalic = TryLoad({
            "C:/Windows/Fonts/arialnovai.ttf",
            "C:/Windows/Fonts/ariali.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Italic.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Italic.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
        }, fLoad);

        m_pBoldItalic = TryLoad({
            "C:/Windows/Fonts/arialnovabi.ttf",
            "C:/Windows/Fonts/arialbi.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-BoldItalic.ttf",
            "/usr/share/fonts/liberation/LiberationSans-BoldItalic.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf",
        }, fLoad, true);
    }
    else if (sFace == "aptos")
    {
        // ---- Aptos (Windows 11 default UI font, replaces Segoe UI) -------
        // Aptos ships with spaces in filenames on some Windows versions.
        m_pRegular = TryLoad({
            "C:/Windows/Fonts/Aptos.ttf",
            "C:/Windows/Fonts/aptos.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        }, fLoad);

        m_pBold = TryLoad({
            "C:/Windows/Fonts/AptosBold.ttf",
            "C:/Windows/Fonts/aptosBold.ttf",
            "C:/Windows/Fonts/segoeuib.ttf",
            "C:/Windows/Fonts/arialbd.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        }, fLoad, true);

        m_pItalic = TryLoad({
            "C:/Windows/Fonts/AptosItalic.ttf",
            "C:/Windows/Fonts/aptosItalic.ttf",
            "C:/Windows/Fonts/segoeuii.ttf",
            "C:/Windows/Fonts/ariali.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Italic.ttf",
        }, fLoad);

        m_pBoldItalic = TryLoad({
            "C:/Windows/Fonts/AptosBoldItalic.ttf",
            "C:/Windows/Fonts/aptosBoldItalic.ttf",
            "C:/Windows/Fonts/segoeuiz.ttf",
            "C:/Windows/Fonts/arialbi.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-BoldItalic.ttf",
        }, fLoad, true);
    }
    else if (sFace == "gothic")
    {
        // ---- Gothic / Blackletter (Old English Text MT / Gabriola) ----------
        // Old English Text MT ships with most Windows installs.
        // Gabriola is a decorative display font (Windows 7+).
        // True bold/italic variants are rare for blackletter; reuse regular.
        m_pRegular = TryLoad({
            "C:/Windows/Fonts/OLDENGL.TTF",
            "C:/Windows/Fonts/Gabriola.ttf",
            "/usr/share/fonts/truetype/unifraktur-maguntia/UnifrakturMaguntia.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
            "/usr/share/fonts/dejavu/DejaVuSerif.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
        }, fLoad);

        m_pBold = TryLoad({
            "C:/Windows/Fonts/Gabriola.ttf",   // heavier stroke than Old English
            "C:/Windows/Fonts/OLDENGL.TTF",
            "/usr/share/fonts/truetype/unifraktur-maguntia/UnifrakturMaguntia.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSerif-Bold.ttf",
        }, fLoad, true);

        // Blackletter has no standard italic -- fall back to regular via nullptr
        m_pItalic     = nullptr;
        m_pBoldItalic = nullptr;
    }
    else if (sFace == "script")
    {
        // ---- Script / Handwriting (Segoe Script / Lucida Handwriting) --------
        m_pRegular = TryLoad({
            "C:/Windows/Fonts/segoesc.ttf",    // Segoe Script
            "C:/Windows/Fonts/LHANDW.TTF",     // Lucida Handwriting
            "C:/Windows/Fonts/FREESCPT.TTF",   // Freestyle Script
            "C:/Windows/Fonts/BRUSHSCI.TTF",   // Brush Script MT Italic
            "/usr/share/fonts/truetype/urw-base35/URWChanceryL-MediItal.t1",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans-Oblique.ttf",
        }, fLoad);

        m_pBold = TryLoad({
            "C:/Windows/Fonts/segoescb.ttf",   // Segoe Script Bold
            "C:/Windows/Fonts/segoesc.ttf",
            "C:/Windows/Fonts/LHANDW.TTF",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        }, fLoad, true);

        // Lucida Handwriting is inherently italic; treat it as the italic slot
        m_pItalic = TryLoad({
            "C:/Windows/Fonts/LHANDW.TTF",
            "C:/Windows/Fonts/segoesc.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
        }, fLoad);

        m_pBoldItalic = nullptr;
    }
    else
    {
        // ---- Sans Serif (default) -- Segoe UI / DejaVu Sans -------------
        m_pRegular = TryLoad({
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        }, fLoad);

        m_pBold = TryLoad({
            "C:/Windows/Fonts/segoeuib.ttf",
            "C:/Windows/Fonts/arialbd.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        }, fLoad, true);

        m_pItalic = TryLoad({
            "C:/Windows/Fonts/segoeuii.ttf",
            "C:/Windows/Fonts/ariali.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans-Oblique.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Italic.ttf",
        }, fLoad);

        m_pBoldItalic = TryLoad({
            "C:/Windows/Fonts/segoeuiz.ttf",
            "C:/Windows/Fonts/arialbi.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans-BoldOblique.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-BoldItalic.ttf",
        }, fLoad, true);
    }

    // If no system font was found at all, fall back to ImGui's embedded default.
    if (!m_pRegular)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImFontConfig cfg;
        cfg.SizePixels = fLoad;
        m_pRegular = io.Fonts->AddFontDefault(&cfg);
        fprintf(stderr,
                "[FontManager] No system font found for face '%s' "
                "-- using built-in default.\n", sFace.c_str());
    }

    // Set the regular font as the ImGui default.
    // FontGlobalScale scales the baked atlas size to the user-chosen logical
    // size, applying uniformly to every widget in the entire UI.
    ImGuiIO& io = ImGui::GetIO();
    io.FontDefault      = m_pRegular;
    io.FontGlobalScale  = fSize / k_fBaseSize;

    m_sActiveFace = sFace;
    m_fActiveSize = fSize;
}

// ---------------------------------------------------------------------------
// Build -- public entry; adds fonts to the atlas (must be empty / cleared
//          by caller beforehand for a rebuild).
// ---------------------------------------------------------------------------

void FontManager::Build(const std::string& sFace, float fSize)
{
    DoLoad(sFace, fSize);
}

// ---------------------------------------------------------------------------
// RequestRebuild -- deferred; queued between frames.
// Optimization: when only the size changes (same face, atlas already loaded)
// we skip the atlas rebuild entirely and just update FontGlobalScale -- the
// change takes effect on the very next frame for the entire UI.
// ---------------------------------------------------------------------------

void FontManager::RequestRebuild(const std::string& sFace, float fSize)
{
    m_sPendingFace  = sFace;
    m_fPendingSize  = fSize;

    // Size-only change: no atlas work needed, just rescale.
    if (sFace == m_sActiveFace && m_pRegular != nullptr)
    {
        ImGui::GetIO().FontGlobalScale = fSize / k_fBaseSize;
        m_fActiveSize  = fSize;
        m_bNeedsRebuild = false;
        return;
    }

    m_bNeedsRebuild = true;
}

// ---------------------------------------------------------------------------
// DoRebuild -- clear atlas, reload, build CPU bitmap.
//   Caller wraps with backend DestroyFontsTexture / CreateFontsTexture.
// ---------------------------------------------------------------------------

void FontManager::DoRebuild()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    DoLoad(m_sPendingFace, m_fPendingSize);

    // Do NOT call io.Fonts->Build() here.
    // This backend (ImGui 1.92+ with ImGuiBackendFlags_RendererHasTextures)
    // manages all texture creation/upload automatically in RenderDrawData().
    // Calling Build() manually is in the obsolete API path and must not be used.
    m_bNeedsRebuild = false;
}
