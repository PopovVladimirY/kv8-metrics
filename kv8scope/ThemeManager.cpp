// kv8scope -- Kv8 Software Oscilloscope
// ThemeManager.cpp -- Apply ImGui + ImPlot style colors.
//
// Four built-in themes (proposal section 8):
//   night_sky      -- deep blue-black (default)
//   sahara_desert  -- warm amber / brown
//   ocean_deep     -- deep navy / cyan
//   classic_dark   -- neutral dark gray

#include "ThemeManager.h"

#include "imgui.h"
#include "implot.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr ImVec4 HexColor(unsigned int hex, float a = 1.0f)
{
    return ImVec4(
        static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
        static_cast<float>((hex >>  8) & 0xFF) / 255.0f,
        static_cast<float>((hex      ) & 0xFF) / 255.0f,
        a);
}

// Brighten an ImVec4 by adding delta to each RGB channel (clamped to 1).
static ImVec4 Brighten(ImVec4 v, float delta)
{
    v.x = v.x + delta > 1.0f ? 1.0f : v.x + delta;
    v.y = v.y + delta > 1.0f ? 1.0f : v.y + delta;
    v.z = v.z + delta > 1.0f ? 1.0f : v.z + delta;
    return v;
}

// Darken an ImVec4 by subtracting delta from each RGB channel (clamped to 0).
static ImVec4 Darken(ImVec4 v, float delta)
{
    v.x = v.x - delta < 0.0f ? 0.0f : v.x - delta;
    v.y = v.y - delta < 0.0f ? 0.0f : v.y - delta;
    v.z = v.z - delta < 0.0f ? 0.0f : v.z - delta;
    return v;
}

// Return a copy of v with alpha replaced.
static ImVec4 WithAlpha(ImVec4 v, float a) { v.w = a; return v; }

// ---------------------------------------------------------------------------
// Apply -- dispatch on theme name
// ---------------------------------------------------------------------------

void ThemeManager::Apply(const std::string& sThemeName)
{
    m_sActive = sThemeName;

    if (sThemeName == "sahara_desert")
        ApplySaharaDesert();
    else if (sThemeName == "ocean_deep")
        ApplyOceanDeep();
    else if (sThemeName == "classic_dark")
        ApplyClassicDark();
    else if (sThemeName == "morning_light")
        ApplyMorningLight();
    else if (sThemeName == "blizzard")
        ApplyBlizzard();
    else if (sThemeName == "classic_light")
        ApplyClassicLight();
    else if (sThemeName == "paper_white")
        ApplyPaperWhite();
    else if (sThemeName == "inferno")
        ApplyInferno();
    else if (sThemeName == "neon_bubble_gum")
        ApplyNeonBubbleGum();
    else if (sThemeName == "psychedelic_delirium")
        ApplyPsychedelicDelirium();
    else
    {
        // "night_sky" or unknown -- fall back to default.
        m_sActive = "night_sky";
        ApplyNightSky();
    }
}

// ---------------------------------------------------------------------------
// ApplyCommonStyle -- apply m_colors to ImGui + ImPlot
//
// Each palette-specific function fills m_colors, then calls this.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyCommonStyle(bool bLight)
{
    const ThemeColors& tc = m_colors;

    // ---- ImGui style vars -------------------------------------------------
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 0.0f;
    style.FrameRounding     = 2.0f;
    style.GrabRounding      = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.TabRounding       = 2.0f;

    // ---- ImGui colors -----------------------------------------------------
    ImVec4* c = style.Colors;

    c[ImGuiCol_WindowBg]             = tc.background;
    c[ImGuiCol_ChildBg]              = tc.backgroundAlt;
    c[ImGuiCol_PopupBg]              = WithAlpha(tc.backgroundAlt, 0.95f);
    c[ImGuiCol_Text]                 = tc.textPrimary;
    c[ImGuiCol_TextDisabled]         = tc.textSecondary;
    c[ImGuiCol_FrameBg]              = tc.backgroundAlt;
    c[ImGuiCol_FrameBgHovered]       = WithAlpha(tc.selection, 0.60f);
    c[ImGuiCol_FrameBgActive]        = WithAlpha(tc.selection, 0.80f);
    c[ImGuiCol_Header]               = tc.selection;
    c[ImGuiCol_HeaderHovered]        = tc.selectionHover;
    c[ImGuiCol_HeaderActive]         = tc.selectionHover;
    c[ImGuiCol_MenuBarBg]            = tc.background;
    c[ImGuiCol_TitleBg]              = tc.background;
    c[ImGuiCol_TitleBgActive]        = tc.backgroundAlt;
    c[ImGuiCol_TitleBgCollapsed]     = WithAlpha(tc.background, 0.60f);
    c[ImGuiCol_Tab]                  = tc.backgroundAlt;
    c[ImGuiCol_TabSelected]          = tc.selection;
    c[ImGuiCol_TabHovered]           = tc.selectionHover;
    c[ImGuiCol_TabDimmed]            = tc.background;
    c[ImGuiCol_TabDimmedSelected]    = tc.backgroundAlt;
    c[ImGuiCol_Border]               = tc.border;
    c[ImGuiCol_BorderShadow]         = HexColor(0x000000, 0.0f);
    c[ImGuiCol_ScrollbarBg]          = tc.background;
    c[ImGuiCol_ScrollbarGrab]        = tc.border;
    c[ImGuiCol_ScrollbarGrabHovered] = Brighten(tc.border, 0.10f);
    c[ImGuiCol_ScrollbarGrabActive]  = tc.textSecondary;
    c[ImGuiCol_Button]               = tc.selection;
    c[ImGuiCol_ButtonHovered]        = tc.selectionHover;
    // Light themes: active = darker than hover.  Dark themes: lighter.
    c[ImGuiCol_ButtonActive]         = bLight
                                           ? Darken(tc.selectionHover, 0.08f)
                                           : Brighten(tc.selectionHover, 0.08f);
    c[ImGuiCol_CheckMark]            = tc.accent;
    c[ImGuiCol_SliderGrab]           = tc.accent;
    c[ImGuiCol_SliderGrabActive]     = tc.accentLight;
    c[ImGuiCol_Separator]            = tc.border;
    c[ImGuiCol_SeparatorHovered]     = tc.accent;
    c[ImGuiCol_SeparatorActive]      = tc.accentLight;
    c[ImGuiCol_ResizeGrip]           = WithAlpha(tc.border, 0.40f);
    c[ImGuiCol_ResizeGripHovered]    = WithAlpha(tc.accent, 0.60f);
    c[ImGuiCol_ResizeGripActive]     = WithAlpha(tc.accent, 0.90f);
    c[ImGuiCol_DockingPreview]       = WithAlpha(tc.accent, 0.70f);
    c[ImGuiCol_DockingEmptyBg]       = tc.background;
    c[ImGuiCol_TableHeaderBg]        = tc.backgroundAlt;
    c[ImGuiCol_TableBorderStrong]    = tc.border;
    c[ImGuiCol_TableBorderLight]     = WithAlpha(tc.border, 0.50f);
    c[ImGuiCol_TableRowBg]           = HexColor(0x000000, 0.0f);
    // Light themes need a faint dark tint for alt rows;
    // dark themes use a faint white tint.
    c[ImGuiCol_TableRowBgAlt]        = bLight
                                           ? HexColor(0x000000, 0.05f)
                                           : HexColor(0xffffff, 0.02f);

    // ---- ImPlot colors ----------------------------------------------------
    ImPlotStyle& ps = ImPlot::GetStyle();
    ps.Colors[ImPlotCol_PlotBg]       = tc.background;
    ps.Colors[ImPlotCol_PlotBorder]   = tc.border;
    ps.Colors[ImPlotCol_AxisGrid]     = tc.plotGrid;
    ps.Colors[ImPlotCol_AxisTick]     = tc.border;
    ps.Colors[ImPlotCol_AxisText]     = tc.textSecondary;
    ps.Colors[ImPlotCol_Selection]    = WithAlpha(tc.accent, 0.50f);
    ps.Colors[ImPlotCol_Crosshairs]   = WithAlpha(tc.accent, 0.60f);
    ps.Colors[ImPlotCol_FrameBg]      = WithAlpha(tc.background, 0.0f);
    ps.Colors[ImPlotCol_LegendBg]     = WithAlpha(tc.backgroundAlt, 0.90f);
    ps.Colors[ImPlotCol_LegendBorder] = tc.border;
    ps.Colors[ImPlotCol_LegendText]   = tc.textPrimary;
}

// ---------------------------------------------------------------------------
// Night Sky (default) -- proposal section 8.1
//   Deep blue-black background, cool blue accents.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyNightSky()
{
    m_colors.background      = HexColor(0x07090d);  // darker base
    m_colors.backgroundAlt   = HexColor(0x0e1420);  // clearly distinct panel
    m_colors.textPrimary     = HexColor(0xeaf2fc);  // brighter primary text
    m_colors.textSecondary   = HexColor(0xa0b0c4);  // brighter secondary text
    m_colors.border          = HexColor(0x3a4255);  // more visible border
    m_colors.selection       = HexColor(0x1e3d65);
    m_colors.selectionHover  = HexColor(0x2a5080);
    m_colors.accent          = HexColor(0x58a6ff);
    m_colors.accentLight     = HexColor(0x79c0ff);
    m_colors.onlineBadge     = HexColor(0x4ade40);  // bright grass green
    m_colors.offlineBadge    = HexColor(0x708090);
    m_colors.plotGrid        = HexColor(0x111a28);  // darker plot bg
    m_colors.plotGridMajor   = HexColor(0x1e2d40);

    ApplyCommonStyle(false);
}

// ---------------------------------------------------------------------------
// Sahara Desert -- proposal section 8.1
//   Warm amber / brown tones, amber online badge.
// ---------------------------------------------------------------------------

void ThemeManager::ApplySaharaDesert()
{
    m_colors.background      = HexColor(0x0e0a05);  // darker, richer base
    m_colors.backgroundAlt   = HexColor(0x1c1408);  // clearly distinct warm panel
    m_colors.textPrimary     = HexColor(0xf5ecd8);  // brighter warm text
    m_colors.textSecondary   = HexColor(0xbaa888);  // brighter secondary text
    m_colors.border          = HexColor(0x5a4228);  // more visible warm border
    m_colors.selection       = HexColor(0x4e381a);
    m_colors.selectionHover  = HexColor(0x6a4e28);
    m_colors.accent          = HexColor(0xf0a030);
    m_colors.accentLight     = HexColor(0xffbf50);
    m_colors.onlineBadge     = HexColor(0x4ade40);  // bright grass green
    m_colors.offlineBadge    = HexColor(0x8a7a60);
    m_colors.plotGrid        = HexColor(0x18110a);  // darker plot bg
    m_colors.plotGridMajor   = HexColor(0x2c1e0e);

    ApplyCommonStyle(false);
}

// ---------------------------------------------------------------------------
// Ocean Deep -- proposal section 8.1
//   Deep navy background, cyan accents, aquatic palette.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyOceanDeep()
{
    m_colors.background      = HexColor(0x060c18);  // darker abyss base
    m_colors.backgroundAlt   = HexColor(0x0b1628);  // clearly distinct deep panel
    m_colors.textPrimary     = HexColor(0xe8f4ff);  // brighter cool-white text
    m_colors.textSecondary   = HexColor(0x90b8d8);  // brighter secondary text
    m_colors.border          = HexColor(0x2a5070);  // more visible blue border
    m_colors.selection       = HexColor(0x0c3860);
    m_colors.selectionHover  = HexColor(0x124878);
    m_colors.accent          = HexColor(0x00c8ff);
    m_colors.accentLight     = HexColor(0x33d8ff);
    m_colors.onlineBadge     = HexColor(0x4ade40);  // bright grass green
    m_colors.offlineBadge    = HexColor(0x6080a0);
    m_colors.plotGrid        = HexColor(0x0a1830);  // darker plot bg
    m_colors.plotGridMajor   = HexColor(0x142848);

    ApplyCommonStyle(false);
}

// ---------------------------------------------------------------------------
// Classic Dark -- proposal section 8.1
//   Neutral dark gray, no color tint, green online badge.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyClassicDark()
{
    m_colors.background      = HexColor(0x101010);  // darker neutral base
    m_colors.backgroundAlt   = HexColor(0x1c1c1c);  // clearly distinct panel
    m_colors.textPrimary     = HexColor(0xeeeeee);  // brighter primary text
    m_colors.textSecondary   = HexColor(0xa8a8a8);  // brighter secondary text
    m_colors.border          = HexColor(0x4e4e4e);  // more visible border
    m_colors.selection       = HexColor(0x28394a);
    m_colors.selectionHover  = HexColor(0x38506a);
    m_colors.accent          = HexColor(0x00cc66);
    m_colors.accentLight     = HexColor(0x33dd80);
    m_colors.onlineBadge     = HexColor(0x4ade40);  // bright grass green
    m_colors.offlineBadge    = HexColor(0x707070);
    m_colors.plotGrid        = HexColor(0x181818);  // darker plot bg
    m_colors.plotGridMajor   = HexColor(0x282828);

    ApplyCommonStyle(false);
}

// ---------------------------------------------------------------------------
// Morning Light -- warm parchment / cream.  Light theme.
//   Cream background, near-black warm-brown text, dark amber accents.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyMorningLight()
{
    m_colors.background      = HexColor(0xede4d0);  // darker warm parchment
    m_colors.backgroundAlt   = HexColor(0xd4bc94);  // darker amber-tan panel
    m_colors.textPrimary     = HexColor(0x140c04);  // deeper near-black
    m_colors.textSecondary   = HexColor(0x4a3020);  // darker warm brown
    m_colors.border          = HexColor(0x7a5c28);  // darker, more visible border
    m_colors.selection       = HexColor(0xc09020);  // darker amber selection
    m_colors.selectionHover  = HexColor(0xa07010);  // darker amber on hover
    m_colors.accent          = HexColor(0x7a3c00);  // deeper burnt-orange
    m_colors.accentLight     = HexColor(0xa05000);
    m_colors.onlineBadge     = HexColor(0x28b428);  // bright grass green (light bg)
    m_colors.offlineBadge    = HexColor(0x5e4430);
    m_colors.plotGrid        = HexColor(0xb09870);  // stronger minor grid
    m_colors.plotGridMajor   = HexColor(0x886840);  // stronger major grid

    ApplyCommonStyle(true);
}

// ---------------------------------------------------------------------------
// Blizzard -- crisp icy white / steel blue.  Light theme.
//   Ice-blue background, near-black navy text, strong blue accents.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyBlizzard()
{
    m_colors.background      = HexColor(0xedf2fb);  // pale ice blue
    m_colors.backgroundAlt   = HexColor(0xd0e0f4);  // clearly distinct steel-blue panel
    m_colors.textPrimary     = HexColor(0x060e1e);  // near-black navy
    m_colors.textSecondary   = HexColor(0x264a70);  // dark steel blue, readable
    m_colors.border          = HexColor(0x5888c0);  // strong visible blue border
    m_colors.selection       = HexColor(0x88bbe0);  // distinct mid-blue selection
    m_colors.selectionHover  = HexColor(0x6aaad8);  // darker on hover
    m_colors.accent          = HexColor(0x0840a0);  // deep navy-blue, high contrast
    m_colors.accentLight     = HexColor(0x1058c8);
    m_colors.onlineBadge     = HexColor(0x28b428);  // bright grass green (light bg)
    m_colors.offlineBadge    = HexColor(0x406080);
    m_colors.plotGrid        = HexColor(0xa4c0e4);  // clearly visible minor grid
    m_colors.plotGridMajor   = HexColor(0x7098cc);  // strong major grid

    ApplyCommonStyle(true);
}

// ---------------------------------------------------------------------------
// Classic Light -- MS Office / OpenOffice look and feel.  Light theme.
//   White background, mid-gray toolbar, near-black text, blue accents.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyClassicLight()
{
    m_colors.background      = HexColor(0xffffff);  // pure white canvas
    m_colors.backgroundAlt   = HexColor(0xe8e8e8);  // toolbar / panel gray
    m_colors.textPrimary     = HexColor(0x1a1a1a);  // near-black body text
    m_colors.textSecondary   = HexColor(0x505050);  // mid-gray secondary text
    m_colors.border          = HexColor(0xaaaaaa);  // visible medium-gray border
    m_colors.selection       = HexColor(0xb8d4f0);  // classic soft-blue selection
    m_colors.selectionHover  = HexColor(0x9ac0e8);  // slightly deeper on hover
    m_colors.accent          = HexColor(0x0054a6);  // Office classic mid-blue
    m_colors.accentLight     = HexColor(0x1a78cc);
    m_colors.onlineBadge     = HexColor(0x28b428);  // bright grass green (light bg)
    m_colors.offlineBadge    = HexColor(0x808080);
    m_colors.plotGrid        = HexColor(0xd8d8d8);  // light gray minor grid
    m_colors.plotGridMajor   = HexColor(0xbbbbbb);  // slightly darker major grid

    ApplyCommonStyle(true);
}

// ---------------------------------------------------------------------------
// Paper White -- grayscale UI; color reserved for graphs and indicators.
//   Off-white background, cool-gray UI, black text, neutral blue accent.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyPaperWhite()
{
    m_colors.background      = HexColor(0xf5f5f5);  // off-white paper
    m_colors.backgroundAlt   = HexColor(0xe2e2e2);  // light gray panels
    m_colors.textPrimary     = HexColor(0x111111);  // near-black
    m_colors.textSecondary   = HexColor(0x606060);  // readable mid-gray
    m_colors.border          = HexColor(0xb0b0b0);  // neutral gray border
    m_colors.selection       = HexColor(0xcccccc);  // neutral selection
    m_colors.selectionHover  = HexColor(0xb8b8b8);
    m_colors.accent          = HexColor(0x404040);  // dark gray accent (neutral)
    m_colors.accentLight     = HexColor(0x585858);
    // Online / offline badges use color deliberately (color-only exception).
    m_colors.onlineBadge     = HexColor(0x28b428);  // bright grass green (light bg)
    m_colors.offlineBadge    = HexColor(0x808080);  // gray
    m_colors.plotGrid        = HexColor(0xd4d4d4);  // faint gray minor grid
    m_colors.plotGridMajor   = HexColor(0xb8b8b8);  // medium gray major grid

    ApplyCommonStyle(true);
}

// ---------------------------------------------------------------------------
// Inferno -- smoldering embers / deep fire.  Dark theme.
//   Near-black red-brown background, hot ash text, flame-orange accents.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyInferno()
{
    m_colors.background      = HexColor(0x0e0704);
    m_colors.backgroundAlt   = HexColor(0x180c06);
    m_colors.textPrimary     = HexColor(0xf8e8d0);
    m_colors.textSecondary   = HexColor(0xc09070);
    m_colors.border          = HexColor(0x5a1e08);
    m_colors.selection       = HexColor(0x7a2000);
    m_colors.selectionHover  = HexColor(0x9c2c00);
    m_colors.accent          = HexColor(0xff5c10);
    m_colors.accentLight     = HexColor(0xff7c30);
    m_colors.onlineBadge     = HexColor(0x4ade40);  // bright grass green
    m_colors.offlineBadge    = HexColor(0x6a3a28);
    m_colors.plotGrid        = HexColor(0x1e0c06);
    m_colors.plotGridMajor   = HexColor(0x301408);

    ApplyCommonStyle(false);
}

// ---------------------------------------------------------------------------
// Neon Bubble Gum -- retina-scorching hot pink / electric cyan.  Dark theme.
//   Do not look directly at screen.  Eye protection recommended.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyNeonBubbleGum()
{
    m_colors.background      = HexColor(0x1a0520);
    m_colors.backgroundAlt   = HexColor(0x240830);
    m_colors.textPrimary     = HexColor(0xff88ff);
    m_colors.textSecondary   = HexColor(0xcc44cc);
    m_colors.border          = HexColor(0xcc00aa);
    m_colors.selection       = HexColor(0x8800cc);
    m_colors.selectionHover  = HexColor(0xaa00ee);
    m_colors.accent          = HexColor(0xff00dd);
    m_colors.accentLight     = HexColor(0xff55ff);
    m_colors.onlineBadge     = HexColor(0x4ade40);  // bright grass green
    m_colors.offlineBadge    = HexColor(0x883399);
    m_colors.plotGrid        = HexColor(0x300a40);
    m_colors.plotGridMajor   = HexColor(0x440a55);

    ApplyCommonStyle(false);
}

// ---------------------------------------------------------------------------
// Psychedelic Delirium -- nuclear green / blinding yellow / void purple.
//   One tab of California sunshine.  Not for the faint of heart.
// ---------------------------------------------------------------------------

void ThemeManager::ApplyPsychedelicDelirium()
{
    m_colors.background      = HexColor(0x0a0014);
    m_colors.backgroundAlt   = HexColor(0x100020);
    m_colors.textPrimary     = HexColor(0xeeff00);
    m_colors.textSecondary   = HexColor(0x99dd00);
    m_colors.border          = HexColor(0x7700cc);
    m_colors.selection       = HexColor(0x4400aa);
    m_colors.selectionHover  = HexColor(0x6600cc);
    m_colors.accent          = HexColor(0x39ff14);
    m_colors.accentLight     = HexColor(0x77ff44);
    m_colors.onlineBadge     = HexColor(0x4ade40);  // bright grass green
    m_colors.offlineBadge    = HexColor(0x553388);
    m_colors.plotGrid        = HexColor(0x180030);
    m_colors.plotGridMajor   = HexColor(0x220044);

    ApplyCommonStyle(false);
}
