// kv8scope -- Kv8 Software Oscilloscope
// ThemeManager.h -- Theme definitions and switching.

#pragma once

#include "imgui.h"

#include <string>

// Named color constants for programmatic use outside style application.
struct ThemeColors
{
    ImVec4 background;       // window background
    ImVec4 backgroundAlt;    // toolbar / child panels
    ImVec4 textPrimary;      // primary text
    ImVec4 textSecondary;    // secondary / dim text
    ImVec4 border;           // panel / widget borders
    ImVec4 selection;        // selected item background
    ImVec4 selectionHover;   // hovered selection
    ImVec4 accent;           // checkmark / slider / active highlight
    ImVec4 accentLight;      // lighter accent for active states
    ImVec4 onlineBadge;      // "Online" status color
    ImVec4 offlineBadge;     // "Offline" status color
    ImVec4 plotGrid;         // plot grid lines (minor)
    ImVec4 plotGridMajor;    // plot grid lines (major)
};

class ThemeManager
{
public:
    ThemeManager() = default;

    // Apply the named theme:
    //   Dark  : "night_sky" | "sahara_desert" | "ocean_deep" |
    //           "classic_dark" | "inferno"
    //   Light : "morning_light" | "blizzard" | "classic_light" | "paper_white"
    // Falls back to "night_sky" for unrecognised names.
    void Apply(const std::string& sThemeName);

    // Read-only access to the active color palette.
    const ThemeColors& Colors() const { return m_colors; }

    // Current theme name (lowercase_underscore, matches JSON key).
    const std::string& ActiveTheme() const { return m_sActive; }

private:
    void ApplyNightSky();
    void ApplySaharaDesert();
    void ApplyOceanDeep();
    void ApplyClassicDark();
    void ApplyMorningLight();
    void ApplyBlizzard();
    void ApplyInferno();
    void ApplyNeonBubbleGum();
    void ApplyPsychedelicDelirium();
    void ApplyClassicLight();
    void ApplyPaperWhite();

    // Shared helper: applies the fields of m_colors to ImGui / ImPlot styles.
    // Pass true for light (bright-background) themes so that table-row
    // alternate tint and button-active state are computed correctly.
    void ApplyCommonStyle(bool bLight = false);

    std::string m_sActive;
    ThemeColors m_colors{};
};
