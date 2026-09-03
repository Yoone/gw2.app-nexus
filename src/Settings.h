///----------------------------------------------------------------------------------------------------
/// Settings.h: user settings, persisted to our own JSON file.
///
/// Nexus gives addons no settings store, so we own "<GW2>/addons/GW2app/settings.json" and
/// render our own widgets into the Nexus options panel.
///----------------------------------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace Settings
{
    /* Bumped when the on-disk shape changes. */
    constexpr int SCHEMA_VERSION = 1;

    void Load();
    void Save();

    /* Once per frame: flushes a throttled edit once its window has elapsed, so a crash can
       only lose the last moment of changes rather than everything since the panel closed. */
    void Tick();

    /* Rendered inside Nexus' addon options panel (RT_OptionsRender). */
    void RenderOptions();

    ///------------------------------------------------------------------------------------------------
    /// Values
    ///------------------------------------------------------------------------------------------------
    bool  ShowAccountName();          /* default true  */
    bool  ShowCopyWaypointsButton();  /* default true  */
    int   BackgroundOpacityPct();     /* 75..100, default 85 */
    int   UiScalePct();               /* 75..125, default 100 */
    int   MaxWaypointsPerCopy();      /* 1..15,   default 15  */

    void  SetMaxWaypointsPerCopy(int aValue);

    /* Derived: the device-pixel width row images are drawn at. Base 400 * scale.
       Sent to the website as `render_width` so images arrive 1:1. */
    int   RenderWidth();
    float UiScale();

    /* Lists open at shutdown, restored on the first `state` of a connection. A list missing from
       one connection keeps its place; the set is bounded by age and size instead. */
    const std::vector<std::string>& OpenLists();
    void SetOpenLists(const std::vector<std::string>& aListIds);

    /* Lists whose completed section the user collapsed ("Hide completed"). Only collapsed ids
       are stored, so the default costs nothing and an unknown id reads as expanded. */
    bool IsCompletedCollapsed(const std::string& aListId);
    void SetCompletedCollapsed(const std::string& aListId, bool aCollapsed);

    /* Height the user dragged a list window to, at 100% UI scale, or 0 if they never resized it.
       The window treats it as a ceiling; see ListWindow.cpp for what that means. */
    int  ListHeight(const std::string& aListId);
    void SetListHeight(const std::string& aListId, int aHeight);
}
