///----------------------------------------------------------------------------------------------------
/// Catalog.h: the module's state model and texture cache.
///
/// Single source of truth for "what lists exist, what is open, what images we hold".
/// Call everything here from the render thread.
///----------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Protocol.h"
#include "nexus/Nexus.h"

namespace Catalog
{
    enum class LoadState { Idle, Loading, Failed };

    /* Applies queued server messages and events. Called once per frame, before any UI renders. */
    void Pump();

    void Reset();   /* connection lost: drop lists, images, open windows */

    ///------------------------------------------------------------------------------------------------
    /// Lists
    ///------------------------------------------------------------------------------------------------
    const std::vector<Protocol::List>& Lists();
    const Protocol::List*              Find(const std::string& aListId);
    bool                               IsConnected();

    ///------------------------------------------------------------------------------------------------
    /// Open windows / subscriptions
    ///
    /// The set of open lists IS the subscription set. Changing it re-sends `subscribe`.
    ///------------------------------------------------------------------------------------------------
    void OpenList(const std::string& aListId);
    void CloseList(const std::string& aListId);
    bool IsOpen(const std::string& aListId);
    const std::vector<std::string>& OpenLists();

    /* Re-subscribes without the id, then with it, which makes the website re-stream the list. */
    void RetryList(const std::string& aListId);

    /* Re-sends `subscribe` with the current render width, for when the UI scale changes.
       Leaves load state alone so the existing textures stay on screen: a scale nudge should cost
       a moment of softness, never a spinner. */
    void ResendSubscribe();

    ///------------------------------------------------------------------------------------------------
    /// Load state
    ///------------------------------------------------------------------------------------------------
    LoadState StateOf(const std::string& aListId);
    /* Progress while Loading: how many entry images we already hold, and the total. */
    void      LoadProgress(const std::string& aListId, int& aOutHave, int& aOutTotal);

    ///------------------------------------------------------------------------------------------------
    /// Entry images and pending toggles
    ///------------------------------------------------------------------------------------------------
    /* Row image, or nullptr while it has not arrived / failed to decode. */
    Texture_t* EntryImage(const std::string& aListId, int aIndex);
    Texture_t* HoverImage(const std::string& aListId, int aIndex);

    /* User ticked a checkbox: sends set_entry_completed and marks the row pending.
       The website owns completion state, so leave the local flag as it last reported it. The row
       settles when the website streams the re-rendered image back. */
    void SetCompleted(const std::string& aListId, int aIndex, bool aCompleted);
    bool IsPending(const std::string& aListId, int aIndex);

    /* Hover card streaming. */
    void OpenHover(const std::string& aListId, int aIndex);
    void CloseHover(const std::string& aListId, int aIndex);
}
