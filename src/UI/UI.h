///----------------------------------------------------------------------------------------------------
/// UI.h: the addon's UI surfaces and the shared visual language.
///
/// The look follows the GW2.app website, and each list window carries its list colour in the
/// window header.
///----------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

#include "imgui/imgui.h"
#include "Protocol.h"

namespace UI
{
    ///------------------------------------------------------------------------------------------------
    /// Palette: the six list accents, from the website. Index by the list's settings["color"].
    /// Anything unrecognised falls back to a neutral header.
    ///------------------------------------------------------------------------------------------------
    ImU32 AccentFor(const std::string& aColorName);
    ImU32 HeaderTextOn(ImU32 aAccent);   /* readable title colour over that accent */

    /* The website's primary colour. */
    constexpr ImU32 COL_BRAND = IM_COL32(0xff, 0x7b, 0xc6, 0xff);

    /* The window styling shared by every GW2.app window. Always pair them. */
    void PushWindowStyle(ImU32 aAccent);
    void PopWindowStyle();

    /* A stable ImGui window label: "<display>###GW2APP_<id>". The "###" ties the id to the list
       id, so window geometry survives a rename on the website. */
    std::string WindowLabel(const std::string& aDisplayName, const std::string& aStableId);

    ///------------------------------------------------------------------------------------------------
    /// Surfaces: each called once per frame from AddonRender.
    ///------------------------------------------------------------------------------------------------
    namespace Shell
    {
        void Init();       /* register quick access shortcut, context menu, textures */
        void Shutdown();
        void Render();     /* info window */

        void ToggleInfoWindow();
        void RefreshShortcut();   /* icon + tooltip after a connection or catalog change */
    }

    namespace ListWindows
    {
        void Render();     /* all open list windows */

        /* "Peek": hide every open list without unsubscribing, so restoring costs no re-stream. */
        void ToggleAllHidden();
        bool AnyHidden();
        int  VisibleCount();
        int  HiddenCount();   /* for the shortcut tooltip's ", H hidden" segment */
        void RestoreHidden();
    }

    namespace HoverCard
    {
        /* Call while a row is hovered; handles the 200 ms dwell, open_hover/close_hover, and
           the card itself. Pass an empty listId when nothing is hovered this frame.
           Rects are screen-space min/max: the hovered row, and its owning window. */
        void Update(const std::string& aListId, int aIndex,
                    const ImVec2& aRowMin, const ImVec2& aRowMax,
                    const ImVec2& aWinMin, const ImVec2& aWinMax);
        void Render();
        void Reset();
    }
}
