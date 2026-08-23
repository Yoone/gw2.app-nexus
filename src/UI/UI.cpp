///----------------------------------------------------------------------------------------------------
/// UI.cpp: the shared visual language every GW2.app window is built from.
///
/// The look follows the GW2.app website. Each list's identity colour lives in the window header,
/// where ImGui gives it to us as three style colours.
///----------------------------------------------------------------------------------------------------

#include "UI/UI.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "Settings.h"

namespace
{
    ///------------------------------------------------------------------------------------------------
    /// The six website list accents. These are the website's own `ui/src/lib/listColors.ts`
    /// values, so keep them in step with that file.
    ///------------------------------------------------------------------------------------------------
    constexpr ImU32 ACCENT_PINK    = IM_COL32(0xC2, 0x19, 0x5D, 0xFF);
    constexpr ImU32 ACCENT_ORANGE  = IM_COL32(0xB0, 0x5C, 0x20, 0xFF);
    constexpr ImU32 ACCENT_YELLOW  = IM_COL32(0xCB, 0x8B, 0x00, 0xFF);
    constexpr ImU32 ACCENT_GREEN   = IM_COL32(0x1C, 0x91, 0x41, 0xFF);
    constexpr ImU32 ACCENT_BLUE    = IM_COL32(0x58, 0x65, 0xF2, 0xFF);
    constexpr ImU32 ACCENT_PURPLE  = IM_COL32(0x8F, 0x00, 0xFE, 0xFF);

    /* For an unknown or missing colour. A slate header reads as plain chrome, so it is never
       mistaken for one of the six accents. */
    constexpr ImU32 ACCENT_NEUTRAL = IM_COL32(0x3A, 0x41, 0x52, 0xFF);

    /* The website's own `bg.gw2app` panel colour. */
    constexpr ImU32 PANEL_BG       = IM_COL32(0x1C, 0x21, 0x2B, 0xFF);
    constexpr ImU32 PANEL_BG_DEEP  = IM_COL32(0x15, 0x19, 0x21, 0xFF);

    constexpr ImU32 TEXT_BODY      = IM_COL32(0xE6, 0xE8, 0xEC, 0xFF);
    constexpr ImU32 TEXT_MUTED     = IM_COL32(0x9A, 0xA1, 0xB0, 0xFF);

    constexpr ImU32 SURFACE        = IM_COL32(0x2A, 0x2F, 0x3C, 0xFF);
    constexpr ImU32 SURFACE_HOVER  = IM_COL32(0x3A, 0x41, 0x52, 0xFF);
    constexpr ImU32 SURFACE_ACTIVE = IM_COL32(0x4A, 0x52, 0x62, 0xFF);

    ///------------------------------------------------------------------------------------------------
    /// Colour maths
    ///------------------------------------------------------------------------------------------------
    ImVec4 ToVec4(ImU32 aColor)
    {
        return ImGui::ColorConvertU32ToFloat4(aColor);
    }

    ImU32 WithAlpha(ImU32 aColor, float aAlpha)
    {
        ImVec4 c = ToVec4(aColor);
        c.w *= std::max(0.0f, std::min(1.0f, aAlpha));
        return ImGui::ColorConvertFloat4ToU32(c);
    }

    /* Multiplies RGB, leaves alpha alone. Used for the unfocused title bar. */
    ImU32 Dim(ImU32 aColor, float aFactor)
    {
        ImVec4 c = ToVec4(aColor);
        c.x *= aFactor;
        c.y *= aFactor;
        c.z *= aFactor;
        return ImGui::ColorConvertFloat4ToU32(c);
    }

    ImU32 Lighten(ImU32 aColor, float aAmount)
    {
        ImVec4 c = ToVec4(aColor);
        c.x += (1.0f - c.x) * aAmount;
        c.y += (1.0f - c.y) * aAmount;
        c.z += (1.0f - c.z) * aAmount;
        return ImGui::ColorConvertFloat4ToU32(c);
    }

    /* sRGB -> linear, so the luminance below is perceptual rather than a naive channel average. */
    float Linearise(float aChannel)
    {
        return (aChannel <= 0.04045f)
            ? aChannel / 12.92f
            : std::pow((aChannel + 0.055f) / 1.055f, 2.4f);
    }

    float Luminance(ImU32 aColor)
    {
        ImVec4 c = ToVec4(aColor);
        return 0.2126f * Linearise(c.x) + 0.7152f * Linearise(c.y) + 0.0722f * Linearise(c.z);
    }

    std::string LowerAscii(const std::string& aValue)
    {
        std::string out = aValue;
        for (char& c : out)
        {
            if (c >= 'A' && c <= 'Z') { c = (char)(c - 'A' + 'a'); }
        }
        return out;
    }

    ///------------------------------------------------------------------------------------------------
    /// Style stack
    ///
    /// Each push records its own counts so the matching pop stays correct when the counts change.
    /// An unbalanced ImGui stack crashes Guild Wars 2.
    ///------------------------------------------------------------------------------------------------
    std::vector<std::pair<int, int>> s_styleStack;   /* (colours, vars) */
}

namespace UI
{
    ImU32 AccentFor(const std::string& aColorName)
    {
        const std::string key = LowerAscii(aColorName);

        if (key == "pink")   { return ACCENT_PINK;   }
        if (key == "orange") { return ACCENT_ORANGE; }
        if (key == "yellow") { return ACCENT_YELLOW; }
        if (key == "green")  { return ACCENT_GREEN;  }
        if (key == "blue")   { return ACCENT_BLUE;   }
        if (key == "purple") { return ACCENT_PURPLE; }

        return ACCENT_NEUTRAL;
    }

    ImU32 HeaderTextOn(ImU32 aAccent)
    {
        /* Threshold picked so the one light accent (#cb8b00) takes ink and the other five plus
           the neutral fallback take white, which is what the website does too. */
        return (Luminance(aAccent) > 0.28f)
            ? IM_COL32(0x14, 0x04, 0x0D, 0xFF)   /* brand ink */
            : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
    }

    void PushWindowStyle(ImU32 aAccent)
    {
        /* One draw-time opacity factor shared by every window. Users asked for this setting. */
        const int   pct   = Settings::BackgroundOpacityPct();
        const float alpha = std::max(0.5f, std::min(1.0f, (pct > 0 ? (float)pct : 85.0f) / 100.0f));

        const ImU32 accent    = WithAlpha(aAccent, alpha);
        const ImU32 accentDim = WithAlpha(Dim(aAccent, 0.72f), alpha);

        int colors = 0;

        ImGui::PushStyleColor(ImGuiCol_WindowBg,         WithAlpha(PANEL_BG, alpha));      colors++;
        ImGui::PushStyleColor(ImGuiCol_ChildBg,          WithAlpha(PANEL_BG_DEEP, 0.35f)); colors++;
        ImGui::PushStyleColor(ImGuiCol_PopupBg,          WithAlpha(PANEL_BG, 0.98f));      colors++;
        ImGui::PushStyleColor(ImGuiCol_Border,           IM_COL32(255, 255, 255, 26));     colors++;
        ImGui::PushStyleColor(ImGuiCol_BorderShadow,     IM_COL32(0, 0, 0, 0));            colors++;

        ImGui::PushStyleColor(ImGuiCol_TitleBg,          accentDim);                       colors++;
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,    accent);                          colors++;
        ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, accentDim);                       colors++;

        ImGui::PushStyleColor(ImGuiCol_Text,             TEXT_BODY);                       colors++;
        ImGui::PushStyleColor(ImGuiCol_TextDisabled,     TEXT_MUTED);                      colors++;

        ImGui::PushStyleColor(ImGuiCol_FrameBg,          SURFACE);                         colors++;
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   SURFACE_HOVER);                   colors++;
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    SURFACE_ACTIVE);                  colors++;
        ImGui::PushStyleColor(ImGuiCol_CheckMark,        IM_COL32(0xFA, 0xFA, 0xFA, 0xFF));colors++;

        ImGui::PushStyleColor(ImGuiCol_Button,           SURFACE);                         colors++;
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,    SURFACE_HOVER);                   colors++;
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,     SURFACE_ACTIVE);                  colors++;

        /* The one place the accent leaves the header: it makes the copy-mode slider read as part
           of this list. */
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       aAccent);                         colors++;
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Lighten(aAccent, 0.25f));         colors++;

        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,      IM_COL32(0, 0, 0, 0));            colors++;
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        IM_COL32(255, 255, 255, 36)); colors++;
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(255, 255, 255, 56)); colors++;
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  IM_COL32(255, 255, 255, 76)); colors++;

        ImGui::PushStyleColor(ImGuiCol_Separator,        IM_COL32(255, 255, 255, 38));     colors++;
        ImGui::PushStyleColor(ImGuiCol_Header,           SURFACE);                         colors++;
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,    SURFACE_HOVER);                   colors++;
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,     SURFACE_ACTIVE);                  colors++;

        ImGui::PushStyleColor(ImGuiCol_ResizeGrip,        IM_COL32(255, 255, 255, 26));    colors++;
        ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, IM_COL32(255, 255, 255, 56));    colors++;
        ImGui::PushStyleColor(ImGuiCol_ResizeGripActive,  IM_COL32(255, 255, 255, 86));    colors++;

        /* Every metric comes off the scale setting, so they all move together with it. */
        const float scale = std::max(0.5f, std::min(2.0f, Settings::UiScale()));
        const float pad   = std::round(8.0f * scale);

        int vars = 0;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,    std::round(6.0f * scale));       vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,  1.0f);                           vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,     ImVec2(pad, pad));               vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign,  ImVec2(0.0f, 0.5f));             vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,     std::round(4.0f * scale));       vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize,   0.0f);                           vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,     std::round(4.0f * scale));       vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,     std::round(3.0f * scale));       vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,   0.0f);                           vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding,      std::round(3.0f * scale));       vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, std::round(4.0f * scale));       vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,     std::round(12.0f * scale));      vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,       ImVec2(std::round(6.0f * scale),
                                                                   std::round(6.0f * scale))); vars++;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,  ImVec2(std::round(6.0f * scale),
                                                                   std::round(4.0f * scale))); vars++;

        s_styleStack.push_back(std::make_pair(colors, vars));
    }

    void PopWindowStyle()
    {
        if (s_styleStack.empty()) { return; }   /* never underflow the host's own stack */

        const std::pair<int, int> counts = s_styleStack.back();
        s_styleStack.pop_back();

        ImGui::PopStyleVar(counts.second);
        ImGui::PopStyleColor(counts.first);
    }

    std::string WindowLabel(const std::string& aDisplayName, const std::string& aStableId)
    {
        /* "###" replaces the whole ImGui id with what follows, so the id stays tied to the list
           id and imgui.ini keeps the window's geometry across a rename on the website. */
        std::string display = aDisplayName.empty() ? aStableId : aDisplayName;

        /* A literal "##" in a list name would silently swallow the rest of the label. Break the
           run rather than dropping the name. */
        std::string safe;
        safe.reserve(display.size() + 4);
        for (size_t i = 0; i < display.size(); ++i)
        {
            safe += display[i];
            if (display[i] == '#' && i + 1 < display.size() && display[i + 1] == '#')
            {
                safe += ' ';
            }
        }

        return safe + "###GW2APP_" + aStableId;
    }
}
