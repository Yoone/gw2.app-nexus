///----------------------------------------------------------------------------------------------------
/// Features.h: the module's GW2 domain logic.
///
/// Two things live here: the waypoint chat-code pipeline and the reset countdown. Both encode
/// real game facts, so the values match Guild Wars 2 and the website rather than our own choice.
///----------------------------------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

#include "Protocol.h"

namespace Waypoints
{
    /* GW2 refuses chat messages longer than this. */
    constexpr int GW2_CHAT_MAX_LENGTH = 199;

    /* An entry contributes waypoints when its type is location/dailypsna/vendoritem (or absent
       entirely, for older website clients) and it is neither completed nor auto-completed.
       A chat_link may hold several codes, space-separated. */
    bool IsEligible(const Protocol::Entry& aEntry);

    /* Flattens every eligible entry's codes, in list order. */
    std::vector<std::string> Gather(const Protocol::List& aList);

    /* Packs codes into chat-sized groups. At aMaxPerGroup == 15 (the max) it packs by character
       budget instead of by count, which is what Blish and the website both do. */
    std::vector<std::string> Chunk(const std::vector<std::string>& aCodes, int aMaxPerGroup);

    /* Copies to clipboard and toasts, on failure as well as success, so a refused clipboard is
       visible to the user. */
    void CopyToClipboard(const std::string& aText);
}

namespace Countdown
{
    /* GW2 resets, hardcoded: daily 00:00 UTC, weekly Monday 07:30 UTC.
       aReset is the list's opaque settings["reset"] string.
       Returns "" when there is no countdown to show (NEVER / unknown / absent). */
    std::string For(const std::string& aReset);

    /* "5h30" / "5h" / "45m" / "3d" / "3d 5h" */
    std::string FormatDuration(int aTotalMinutes);
}
