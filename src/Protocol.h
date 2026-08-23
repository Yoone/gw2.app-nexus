///----------------------------------------------------------------------------------------------------
/// Protocol.h: the wire contract with the GW2.app website.
///
/// Field names are fixed by the shipped website, so keep them byte identical.
/// Format: ../gw2.app-blishhud/docs/protocol.md
///
/// We add two optional fields to `subscribe`, `render_width` and `module`. An older website
/// ignores both, and without `render_width` we scale the image it sends instead.
///----------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Protocol
{
    ///------------------------------------------------------------------------------------------------
    /// Inbound: website -> module
    ///------------------------------------------------------------------------------------------------

    struct Entry
    {
        std::string Name;
        std::string ChatLink;      /* may hold several codes, space-separated (PSNA) */
        std::string Link;          /* custom entries only */
        std::string EntryType;     /* "location" | "dailypsna" | "vendoritem" | ... | "" */
        bool        Completed     = false;
        bool        AutoCompleted = false;
        bool        HasHoverCard  = false;
    };

    struct List
    {
        std::string        Id;
        std::string        Name;
        bool               IsLootBag = false;
        std::vector<Entry> Entries;

        /* From the list's opaque `settings` object. Unknown keys are ignored. */
        std::string AccountName;   /* settings["gw2AccountName"] */
        std::string Color;         /* settings["color"]: drives the window header accent */
        std::string Reset;         /* settings["reset"]: "DAILY" | "WEEKLY" | "NEVER" | "" */
        bool        SortEntries = false;
    };

    struct StateMsg
    {
        int               ProtocolVersion = 1;   /* absent => 1 */
        std::vector<List> Lists;
    };

    struct EntryMsg
    {
        std::string           ListId;
        int                   Index = -1;
        std::optional<std::string> ImageB64;     /* absent => flags-only update */
        std::string           Mime;
        std::string           Name;
        std::string           ChatLink;
        std::string           Link;
        bool                  Completed     = false;
        bool                  AutoCompleted = false;
        bool                  HasHoverCard  = false;
    };

    struct SyncedMsg   { std::vector<std::string> ListIds; };
    struct HoverImgMsg { std::string ListId; int Index = -1; std::string ImageB64, Mime; };

    enum class Kind { Unknown, State, Entry, Synced, HoverImage };

    struct Inbound
    {
        Kind        Type = Kind::Unknown;
        StateMsg    State;
        EntryMsg    EntryData;
        SyncedMsg   Synced;
        HoverImgMsg HoverImage;
    };

    /* Returns Kind::Unknown on malformed input. Never throws. */
    Inbound Parse(const std::string& aJson);

    ///------------------------------------------------------------------------------------------------
    /// Outbound: module -> website
    ///------------------------------------------------------------------------------------------------

    /* aRenderWidth <= 0 omits the field entirely. */
    std::string BuildSubscribe(const std::vector<std::string>& aListIds, int aRenderWidth);
    std::string BuildSetEntryCompleted(const std::string& aListId, int aIndex, bool aCompleted);
    std::string BuildOpenHover(const std::string& aListId, int aIndex);
    std::string BuildCloseHover(const std::string& aListId, int aIndex);
}
