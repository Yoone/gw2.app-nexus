///----------------------------------------------------------------------------------------------------
/// Features.cpp: the module's GW2 domain logic.
///
/// Two things live here: the waypoint chat-code pipeline (eligibility, gather, chunk, copy) and
/// the reset countdown. Both follow the website's implementations, by way of the Blish module:
///   chunking:  ui/src/lib/components/locations/copy-locations.svelte:chunkLocations
///   countdown: ui/src/lib/components/timers/countdown.svelte
///
/// A crash here takes Guild Wars 2 down with it, so every entry point is safe on its own: no
/// exception escapes, empty and absurd inputs are handled, and every loop is one forward pass.
///----------------------------------------------------------------------------------------------------
#include "Features.h"

#include <cstdio>
#include <ctime>

#include "Addon.h"

namespace
{
    /* The slider's maximum, mirroring the website's MaxWaypointsPerMessage. At exactly this
       value the chunker switches from counting items to counting characters. */
    constexpr int MAX_WAYPOINTS_PER_MESSAGE = 15;

    /* GW2 resets, hardcoded game facts: daily at 00:00 UTC, weekly Monday 07:30 UTC. */
    constexpr int MINUTES_PER_DAY   = 1440;
    constexpr int SECONDS_PER_DAY   = 86400;
    constexpr int WEEKLY_RESET_MINS = 7 * 60 + 30;   /* 07:30 UTC */
    constexpr int MONDAY            = 1;             /* tm_wday: Sunday = 0 */

    /* ASCII upper-case. Reset strings are protocol tokens, never localised text. */
    std::string ToUpperAscii(const std::string& aValue)
    {
        std::string out = aValue;
        for (char& c : out)
        {
            if (c >= 'a' && c <= 'z') { c = (char)(c - 'a' + 'A'); }
        }
        return out;
    }

    /* Current UTC broken-down time. Returns false if the CRT refuses, and callers then show
       no countdown rather than a wrong one. */
    bool NowUtc(std::tm& aOut)
    {
        std::time_t now = std::time(nullptr);
        if (now == (std::time_t)-1) { return false; }

        /* gmtime hands back a shared static buffer, so copy it out immediately. */
        const std::tm* utc = std::gmtime(&now);
        if (utc == nullptr) { return false; }

        aOut = *utc;
        return true;
    }
}

namespace Waypoints
{
    ///------------------------------------------------------------------------------------------------
    /// IsEligible
    ///
    /// Completed entries are excluded because their waypoints route you to things you are already
    /// done with. An absent entry_type is accepted on purpose: older website clients did not send
    /// the field at all, and dropping those entries would empty the copy panel for them.
    ///------------------------------------------------------------------------------------------------
    bool IsEligible(const Protocol::Entry& aEntry)
    {
        if (aEntry.Completed || aEntry.AutoCompleted) { return false; }

        const std::string& type = aEntry.EntryType;
        return type.empty()
            || type == "location"
            || type == "dailypsna"
            || type == "vendoritem";
    }

    ///------------------------------------------------------------------------------------------------
    /// Gather: flattens every eligible entry's codes in list order.
    ///
    /// A single chat_link may bundle several codes separated by spaces; PSNA entries typically
    /// carry four vendor waypoints in one string. Split on ' ' and drop the empties, exactly as
    /// the Blish module's String.Split(RemoveEmptyEntries) does. No dedup, no sorting.
    ///------------------------------------------------------------------------------------------------
    std::vector<std::string> Gather(const Protocol::List& aList)
    {
        std::vector<std::string> codes;

        try
        {
            for (const Protocol::Entry& entry : aList.Entries)
            {
                if (!IsEligible(entry)) { continue; }
                if (entry.ChatLink.empty()) { continue; }

                const std::string& link = entry.ChatLink;
                size_t start = 0;
                while (start < link.size())
                {
                    size_t end = link.find(' ', start);
                    if (end == std::string::npos) { end = link.size(); }
                    if (end > start) { codes.emplace_back(link, start, end - start); }
                    start = end + 1;
                }
            }
        }
        catch (...)
        {
            /* Only allocation can throw here. Return whatever we managed to collect. */
            Addon::Log(LOGL_WARNING, "Gather: failed while collecting waypoints.");
        }

        return codes;
    }

    ///------------------------------------------------------------------------------------------------
    /// Chunk: packs codes into chat-sized groups, joined by single spaces.
    ///
    /// Two modes, switched by the slider position:
    ///   aMaxPerGroup == 15 (the "Max" position): character budget. Keep appending while the
    ///     space-joined candidate stays <= GW2_CHAT_MAX_LENGTH. The test is `> 199`, so a group
    ///     may be exactly 199 characters but never 200.
    ///   aMaxPerGroup 1..14: item count. Break at `count >= aMaxPerGroup`, whatever the length.
    ///
    /// Greedy, single forward pass, and a code is never split across groups. A code that alone
    /// busts the budget becomes its own oversized group, the same choice the website and the
    /// Blish module make. GW2's chat will refuse it, but dropping a waypoint the user asked for
    /// would be worse. The loop cannot stall: every pass consumes exactly one code.
    ///------------------------------------------------------------------------------------------------
    std::vector<std::string> Chunk(const std::vector<std::string>& aCodes, int aMaxPerGroup)
    {
        std::vector<std::string> groups;
        if (aCodes.empty()) { return groups; }

        /* The slider is 1..15; clamp so a stray value cannot produce zero-sized groups (which
           would loop) or accidentally land outside both modes. Blish clamps at every read. */
        int maxPerGroup = aMaxPerGroup;
        if (maxPerGroup < 1)                       { maxPerGroup = 1; }
        if (maxPerGroup > MAX_WAYPOINTS_PER_MESSAGE) { maxPerGroup = MAX_WAYPOINTS_PER_MESSAGE; }

        const bool byCharacters = (maxPerGroup == MAX_WAYPOINTS_PER_MESSAGE);

        try
        {
            std::string current;
            int count = 0;

            for (const std::string& code : aCodes)
            {
                if (code.empty()) { continue; }

                std::string candidate = current;
                if (!candidate.empty()) { candidate += ' '; }
                candidate += code;

                const bool maxReached = byCharacters
                    ? (candidate.size() > (size_t)GW2_CHAT_MAX_LENGTH)
                    : (count >= maxPerGroup);

                if (!maxReached)
                {
                    current = candidate;
                    ++count;
                }
                else
                {
                    if (count > 0) { groups.push_back(current); }
                    current = code;   /* the offending code starts the next group */
                    count   = 1;
                }
            }

            if (count > 0) { groups.push_back(current); }
        }
        catch (...)
        {
            Addon::Log(LOGL_WARNING, "Chunk: failed while packing waypoint groups.");
        }

        return groups;
    }

    ///------------------------------------------------------------------------------------------------
    /// CopyToClipboard: the whole "auto-paste" story. We put the text on the clipboard and toast
    /// to tell the user to paste it into chat. A refused clipboard toasts as well, so the feature
    /// cannot fail invisibly.
    ///------------------------------------------------------------------------------------------------
    void CopyToClipboard(const std::string& aText)
    {
        try
        {
            /* Nothing to copy: no clipboard write, no toast. Not a failure, so say nothing. */
            if (aText.empty()) { return; }

            if (Addon::SetClipboardText(aText))
            {
                Addon::Alert("Copied! Paste into chat to use.");
                return;
            }

            Addon::Log(LOGL_WARNING, "Clipboard write refused for %d characters.", (int)aText.size());
            Addon::Alert("Copy failed. The clipboard is in use by another program.");
        }
        catch (...)
        {
            Addon::Log(LOGL_WARNING, "Clipboard write threw.");
            Addon::Alert("Copy failed.");
        }
    }
}

namespace Countdown
{
    ///------------------------------------------------------------------------------------------------
    /// For: minutes until the list's next reset, formatted.
    ///
    ///   DAILY   -> next 00:00 UTC
    ///   WEEKLY  -> next Monday 07:30 UTC
    ///   NEVER / unknown / empty -> "" (the title bar draws no overlay at all)
    ///
    /// Always UTC. Reset times are the game's, not the player's timezone.
    ///------------------------------------------------------------------------------------------------
    std::string For(const std::string& aReset)
    {
        try
        {
            if (aReset.empty()) { return ""; }

            const std::string reset = ToUpperAscii(aReset);
            if (reset != "DAILY" && reset != "WEEKLY") { return ""; }   /* NEVER or unknown */

            std::tm utc{};
            if (!NowUtc(utc)) { return ""; }

            const int nowMins = utc.tm_hour * 60 + utc.tm_min;

            if (reset == "DAILY")
            {
                /* Minutes to the next midnight UTC. The % keeps it non-negative and yields 0
                   exactly at midnight, the same edge case the web client has. It settles
                   within a minute. */
                return FormatDuration((MINUTES_PER_DAY - nowMins) % MINUTES_PER_DAY);
            }

            /* WEEKLY. Pin today at 07:30 UTC, then walk forward to the next Monday occurrence. */
            const int nowSecOfDay = utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec;
            const int resetSecOfDay = WEEKLY_RESET_MINS * 60;
            const int dow = (utc.tm_wday >= 0 && utc.tm_wday <= 6) ? utc.tm_wday : MONDAY;

            int dayDiff = 0;
            if (dow != MONDAY || nowSecOfDay >= resetSecOfDay)
            {
                dayDiff = (MONDAY - dow + 7) % 7;
                if (dayDiff == 0) { dayDiff = 7; }   /* it is Monday, but 07:30 already passed */
            }

            const int spanSeconds = dayDiff * SECONDS_PER_DAY + resetSecOfDay - nowSecOfDay;

            if (spanSeconds < SECONDS_PER_DAY)
            {
                /* Under a day out: minute-of-day arithmetic, same as the daily path.
                   Inherited quirk, kept to match the website the user has open in a browser:
                   in the 59 seconds after Sunday 07:30:00 the span is just under 24h, so this
                   branch runs while the minute-of-day difference is 0 and the overlay reads
                   "0m". One minute per week, and it fixes itself at 07:31. */
                return FormatDuration(((WEEKLY_RESET_MINS - nowMins) + MINUTES_PER_DAY) % MINUTES_PER_DAY);
            }

            /* A day or more out: whole hours, floored. Seconds count here, unlike the sub-day
               branch above, which drops them. Rendered as days (+ hours). */
            return FormatDuration((spanSeconds / 3600) * 60);
        }
        catch (...)
        {
            return "";
        }
    }

    ///------------------------------------------------------------------------------------------------
    /// FormatDuration: the exact formatting the website uses.
    ///
    ///   330 -> "5h30"    305 -> "5h05"    300 -> "5h"
    ///    45 -> "45m"       0 -> "0m"
    ///  1440 -> "1d"     1740 -> "1d 5h"  4320 -> "3d"
    ///
    /// The asymmetry is in the original and is kept on purpose: the multi-day form has a space
    /// ("3d 5h") while the sub-day form does not ("5h30"), and minutes are zero-padded to two
    /// digits only in the combined hours+minutes form. Minutes are dropped entirely once we are
    /// a day or more out.
    ///------------------------------------------------------------------------------------------------
    std::string FormatDuration(int aTotalMinutes)
    {
        /* A negative or absurd value means our clock moved under us; show nothing silly. */
        if (aTotalMinutes < 0) { aTotalMinutes = 0; }

        const int totalHours = aTotalMinutes / 60;
        char buffer[32];

        if (totalHours >= 24)
        {
            const int days  = totalHours / 24;
            const int hours = totalHours % 24;

            if (hours > 0) { std::snprintf(buffer, sizeof(buffer), "%dd %dh", days, hours); }
            else           { std::snprintf(buffer, sizeof(buffer), "%dd", days); }

            return buffer;
        }

        const int mins = aTotalMinutes % 60;

        if (totalHours > 0 && mins > 0) { std::snprintf(buffer, sizeof(buffer), "%dh%02d", totalHours, mins); }
        else if (totalHours > 0)        { std::snprintf(buffer, sizeof(buffer), "%dh", totalHours); }
        else                            { std::snprintf(buffer, sizeof(buffer), "%dm", mins); }

        return buffer;
    }
}
