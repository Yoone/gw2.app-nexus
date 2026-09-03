#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "Addon.h"
#include "Catalog.h"
#include "imgui/imgui.h"
#include "json/json.hpp"

using json = nlohmann::json;

namespace
{
    ///------------------------------------------------------------------------------------------------
    /// Defaults, ranges and the open-list bounding policy
    ///------------------------------------------------------------------------------------------------
    constexpr bool DEF_SHOW_ACCOUNT_NAME    = true;
    constexpr bool DEF_SHOW_COPY_WAYPOINTS  = true;

    constexpr int  MIN_BG_OPACITY_PCT       = 75;
    constexpr int  MAX_BG_OPACITY_PCT       = 100;
    constexpr int  DEF_BG_OPACITY_PCT       = 85;

    constexpr int  MIN_UI_SCALE_PCT         = 75;
    constexpr int  MAX_UI_SCALE_PCT         = 125;
    constexpr int  DEF_UI_SCALE_PCT         = 100;

    constexpr int  MIN_WAYPOINTS_PER_COPY   = 1;
    constexpr int  MAX_WAYPOINTS_PER_COPY   = 15;

    constexpr int  BASE_DISPLAY_WIDTH       = 400;   /* row image width at 100% */

    /* A list absent from one connection keeps its place, because it may well come back on the
       next one. The set still has to stay bounded, so an id that stays missing across live
       connections for this long is finally dropped, and the array is capped regardless. */
    constexpr int    OPEN_LIST_EXPIRY_DAYS  = 30;
    constexpr size_t MAX_OPEN_LIST_ENTRIES  = 64;

    /* Mirrors the window's own limits, so a hand-edited file cannot ask for a window nobody can
       see or grab. */
    constexpr int    MIN_LIST_HEIGHT        = 120;
    constexpr int    MAX_LIST_HEIGHT        = 1200;

    /* Never write the file more often than this while a slider is being dragged. */
    constexpr double SAVE_THROTTLE_SECONDS  = 0.5;
    /* A UI-scale drag re-subscribes at the new render width; wait for the drag to settle first. */
    constexpr double SCALE_COMMIT_SECONDS   = 0.25;

    struct OpenEntry
    {
        std::string Id;
        int         LastSeenDay = 0;   /* days since the epoch, stamped while the id is live */
    };

    struct HeightEntry
    {
        std::string Id;
        int         Height = 0;   /* at 100% UI scale */
    };

    struct Data
    {
        bool ShowAccountName       = DEF_SHOW_ACCOUNT_NAME;
        bool ShowCopyWaypoints     = DEF_SHOW_COPY_WAYPOINTS;
        int  BgOpacityPct          = DEF_BG_OPACITY_PCT;
        int  UiScalePct            = DEF_UI_SCALE_PCT;
        int  MaxWaypointsPerCopy   = MAX_WAYPOINTS_PER_COPY;

        std::vector<OpenEntry> OpenLists;

        /* Ids only, and only the collapsed ones. Kept in the order they were collapsed. */
        std::vector<std::string> CollapsedCompleted;

        /* Only lists the user actually resized. Everything else auto-fits. */
        std::vector<HeightEntry> ListHeights;
    };

    Data                     s_data;
    std::vector<std::string> s_openIds;         /* flat view handed to callers */

    bool   s_dirty          = false;
    double s_lastSaveTime   = 0.0;

    /* UI-scale debounce state. Only touched from RenderOptions. */
    bool   s_scalePending   = false;
    double s_scaleTouchTime = 0.0;
    int    s_scaleCommitted = DEF_UI_SCALE_PCT;

    int Clamp(int aValue, int aMin, int aMax)
    {
        if (aValue < aMin) { return aMin; }
        if (aValue > aMax) { return aMax; }
        return aValue;
    }

    int TodayDay()
    {
        const std::time_t now = std::time(nullptr);
        if (now <= 0) { return 0; }
        return (int)(now / 86400);
    }

    double Now()
    {
        /* ImGui's clock is the only one we need here, and every caller is on the render thread. */
        return ImGui::GetCurrentContext() != nullptr ? ImGui::GetTime() : 0.0;
    }

    const std::string& SettingsPath()
    {
        static std::string path;
        if (path.empty())
        {
            const std::string& dir = Addon::DataDir();
            if (!dir.empty()) { path = dir + "/settings.json"; }
        }
        return path;
    }

    void RebuildOpenIds()
    {
        s_openIds.clear();
        s_openIds.reserve(s_data.OpenLists.size());
        for (const OpenEntry& entry : s_data.OpenLists)
        {
            s_openIds.push_back(entry.Id);
        }
    }

    bool ContainsId(const std::vector<OpenEntry>& aEntries, const std::string& aId)
    {
        for (const OpenEntry& entry : aEntries)
        {
            if (entry.Id == aId) { return true; }
        }
        return false;
    }

    ///------------------------------------------------------------------------------------------------
    /// Defensive JSON readers. The file is user-writable and may be anything at all.
    ///------------------------------------------------------------------------------------------------
    bool ReadBool(const json& aRoot, const char* aKey, bool aFallback)
    {
        auto it = aRoot.find(aKey);
        if (it == aRoot.end() || !it->is_boolean()) { return aFallback; }
        return it->get<bool>();
    }

    int ReadInt(const json& aRoot, const char* aKey, int aFallback, int aMin, int aMax)
    {
        auto it = aRoot.find(aKey);
        if (it == aRoot.end() || !it->is_number_integer()) { return aFallback; }
        return Clamp(it->get<int>(), aMin, aMax);
    }

    void ReadOpenLists(const json& aRoot)
    {
        s_data.OpenLists.clear();

        auto it = aRoot.find("openLists");
        if (it == aRoot.end() || !it->is_array()) { return; }

        const int today = TodayDay();

        for (const json& item : *it)
        {
            OpenEntry entry;

            if (item.is_string())
            {
                /* Blish stored bare ids; accept that shape so a hand-migrated file still works. */
                entry.Id          = item.get<std::string>();
                entry.LastSeenDay = today;
            }
            else if (item.is_object())
            {
                auto id = item.find("id");
                if (id == item.end() || !id->is_string()) { continue; }
                entry.Id = id->get<std::string>();

                auto seen = item.find("lastSeen");
                entry.LastSeenDay = (seen != item.end() && seen->is_number_integer())
                                    ? seen->get<int>()
                                    : today;
            }
            else
            {
                continue;
            }

            if (entry.Id.empty()) { continue; }
            if (ContainsId(s_data.OpenLists, entry.Id)) { continue; }

            /* A stamp in the future means the clock moved. Treat it as "seen now" so it can
               still expire later. */
            if (entry.LastSeenDay > today || entry.LastSeenDay < 0) { entry.LastSeenDay = today; }

            s_data.OpenLists.push_back(entry);
            if (s_data.OpenLists.size() >= MAX_OPEN_LIST_ENTRIES) { break; }
        }
    }

    void ReadCollapsedCompleted(const json& aRoot)
    {
        s_data.CollapsedCompleted.clear();

        auto it = aRoot.find("collapsedCompleted");
        if (it == aRoot.end() || !it->is_array()) { return; }

        for (const json& item : *it)
        {
            if (!item.is_string()) { continue; }

            std::string id = item.get<std::string>();
            if (id.empty()) { continue; }

            const auto& ids = s_data.CollapsedCompleted;
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) { continue; }

            s_data.CollapsedCompleted.push_back(std::move(id));
        }
    }

    void ReadListHeights(const json& aRoot)
    {
        s_data.ListHeights.clear();

        auto it = aRoot.find("listHeights");
        if (it == aRoot.end() || !it->is_object()) { return; }

        for (auto item = it->begin(); item != it->end(); ++item)
        {
            if (item.key().empty() || !item.value().is_number_integer()) { continue; }

            const int height = item.value().get<int>();
            if (height <= 0) { continue; }   /* "no preference", same as being absent */

            s_data.ListHeights.push_back({ item.key(),
                                           Clamp(height, MIN_LIST_HEIGHT, MAX_LIST_HEIGHT) });
        }
    }

    /* Drop the least recently seen ids that are no longer open, until the array fits the cap.
       Ids the user has open right now are never dropped, however many there are. */
    void BoundOpenLists(std::vector<OpenEntry>& aEntries, size_t aOpenCount)
    {
        while (aEntries.size() > MAX_OPEN_LIST_ENTRIES && aEntries.size() > aOpenCount)
        {
            size_t oldest = aOpenCount;
            for (size_t i = aOpenCount; i < aEntries.size(); ++i)
            {
                if (aEntries[i].LastSeenDay < aEntries[oldest].LastSeenDay) { oldest = i; }
            }
            aEntries.erase(aEntries.begin() + (std::ptrdiff_t)oldest);
        }
    }

    /* Writes only if the throttle window has elapsed. Otherwise the file stays dirty and is
       written on the next chance, or by Save() on unload. */
    void SaveThrottled()
    {
        s_dirty = true;

        const double now = Now();
        if (now - s_lastSaveTime < SAVE_THROTTLE_SECONDS) { return; }

        Settings::Save();
    }

    void FlushIfDirty()
    {
        if (!s_dirty) { return; }

        const double now = Now();
        if (now - s_lastSaveTime < SAVE_THROTTLE_SECONDS) { return; }

        Settings::Save();
    }
}

namespace Settings
{
    ///------------------------------------------------------------------------------------------------
    /// Persistence
    ///------------------------------------------------------------------------------------------------
    void Load()
    {
        s_data = Data();
        s_openIds.clear();
        s_dirty = false;

        try
        {
            const std::string& path = SettingsPath();
            if (path.empty())
            {
                Addon::Log(LOGL_WARNING, "No addon directory yet; running on default settings.");
                return;
            }

            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
            {
                /* First run. Defaults are already in place; the file appears on the first Save. */
                Addon::Log(LOGL_INFO, "No settings.json yet, starting from defaults.");
                RebuildOpenIds();
                return;
            }

            /* allow_exceptions = false: a truncated or malformed file yields a discarded value
               instead of throwing, which is exactly the fallback we want. */
            const json root = json::parse(file, nullptr, false, true);
            if (root.is_discarded() || !root.is_object())
            {
                Addon::Log(LOGL_WARNING, "settings.json is unreadable; using defaults.");
                RebuildOpenIds();
                return;
            }

            const int version = ReadInt(root, "schemaVersion", SCHEMA_VERSION, 0, 100000);
            if (version > SCHEMA_VERSION)
            {
                /* Written by a newer build. Every field is validated below anyway, so read what
                   we understand rather than throwing the user's preferences away. */
                Addon::Log(LOGL_WARNING, "settings.json is from a newer version (%d); reading leniently.",
                           version);
            }

            s_data.ShowAccountName     = ReadBool(root, "showAccountName", DEF_SHOW_ACCOUNT_NAME);
            s_data.ShowCopyWaypoints   = ReadBool(root, "showCopyWaypointsButton", DEF_SHOW_COPY_WAYPOINTS);
            s_data.BgOpacityPct        = ReadInt(root, "bgOpacityPct", DEF_BG_OPACITY_PCT,
                                                 MIN_BG_OPACITY_PCT, MAX_BG_OPACITY_PCT);
            s_data.UiScalePct          = ReadInt(root, "uiScalePct", DEF_UI_SCALE_PCT,
                                                 MIN_UI_SCALE_PCT, MAX_UI_SCALE_PCT);
            s_data.MaxWaypointsPerCopy = ReadInt(root, "maxWaypointsPerCopy", MAX_WAYPOINTS_PER_COPY,
                                                 MIN_WAYPOINTS_PER_COPY, MAX_WAYPOINTS_PER_COPY);

            /* Loading never expires anything: after a long break every id looks missing, and the
               user would lose their windows for having been away. Expiry happens in SetOpenLists,
               where there is a live catalog to judge against. */
            ReadOpenLists(root);
            ReadCollapsedCompleted(root);
            ReadListHeights(root);
        }
        catch (const std::exception& ex)
        {
            s_data = Data();
            Addon::Log(LOGL_WARNING, "Failed to read settings.json (%s); using defaults.", ex.what());
        }
        catch (...)
        {
            s_data = Data();
            Addon::Log(LOGL_WARNING, "Failed to read settings.json; using defaults.");
        }

        s_scaleCommitted = s_data.UiScalePct;
        RebuildOpenIds();
    }

    void Tick()
    {
        FlushIfDirty();
    }

    void Save()
    {
        s_lastSaveTime = Now();
        s_dirty        = false;

        try
        {
            const std::string& path = SettingsPath();
            if (path.empty()) { return; }

            json root;
            root["schemaVersion"]           = SCHEMA_VERSION;
            root["showAccountName"]         = s_data.ShowAccountName;
            root["showCopyWaypointsButton"] = s_data.ShowCopyWaypoints;
            root["bgOpacityPct"]            = s_data.BgOpacityPct;
            root["uiScalePct"]              = s_data.UiScalePct;
            root["maxWaypointsPerCopy"]     = s_data.MaxWaypointsPerCopy;

            json open = json::array();
            for (const OpenEntry& entry : s_data.OpenLists)
            {
                json item;
                item["id"]       = entry.Id;
                item["lastSeen"] = entry.LastSeenDay;
                open.push_back(std::move(item));
            }
            root["openLists"] = std::move(open);

            json collapsed = json::array();
            for (const std::string& id : s_data.CollapsedCompleted)
            {
                collapsed.push_back(id);
            }
            root["collapsedCompleted"] = std::move(collapsed);

            json heights = json::object();
            for (const HeightEntry& entry : s_data.ListHeights)
            {
                heights[entry.Id] = entry.Height;
            }
            root["listHeights"] = std::move(heights);

            /* Write beside the real file and rename over it, so a crash mid-write leaves the
               previous settings intact instead of a truncated file. */
            const std::string temp = path + ".tmp";
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                if (!file.is_open())
                {
                    Addon::Log(LOGL_WARNING, "Could not open %s for writing.", temp.c_str());
                    return;
                }

                file << root.dump(4) << "\n";
                file.flush();
                if (!file.good())
                {
                    Addon::Log(LOGL_WARNING, "Failed to write %s.", temp.c_str());
                    return;
                }
            }

            if (MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) == 0)
            {
                Addon::Log(LOGL_WARNING, "Could not replace settings.json (error %lu).",
                           (unsigned long)GetLastError());
            }
        }
        catch (const std::exception& ex)
        {
            Addon::Log(LOGL_WARNING, "Failed to save settings (%s).", ex.what());
        }
        catch (...)
        {
            Addon::Log(LOGL_WARNING, "Failed to save settings.");
        }
    }

    ///------------------------------------------------------------------------------------------------
    /// Values
    ///------------------------------------------------------------------------------------------------
    bool ShowAccountName()         { return s_data.ShowAccountName; }
    bool ShowCopyWaypointsButton() { return s_data.ShowCopyWaypoints; }
    int  BackgroundOpacityPct()    { return s_data.BgOpacityPct; }
    int  UiScalePct()              { return s_data.UiScalePct; }
    int  MaxWaypointsPerCopy()     { return s_data.MaxWaypointsPerCopy; }

    void SetMaxWaypointsPerCopy(int aValue)
    {
        const int value = Clamp(aValue, MIN_WAYPOINTS_PER_COPY, MAX_WAYPOINTS_PER_COPY);
        if (value == s_data.MaxWaypointsPerCopy)
        {
            FlushIfDirty();
            return;
        }

        s_data.MaxWaypointsPerCopy = value;

        /* Edited by a slider inside copy mode, so throttle the write rather than the value. */
        SaveThrottled();
    }

    float UiScale()
    {
        return (float)s_data.UiScalePct / 100.0f;
    }

    int RenderWidth()
    {
        return (int)std::lround((double)BASE_DISPLAY_WIDTH * (double)s_data.UiScalePct / 100.0);
    }

    ///------------------------------------------------------------------------------------------------
    /// Open lists
    ///
    /// The caller hands us the ids that are open right now. An id that drops out of that set
    /// counts as closed only when the website is currently offering it. If it is missing from a
    /// live catalog we keep it, because it may well be back next connection, and those kept ids
    /// age out so the set stays bounded.
    ///
    /// Contract for callers: call this only when the open set actually changes, and never before
    /// the once-per-connection restore has run. An empty set sent while the catalog is live reads
    /// as "the user closed everything".
    ///------------------------------------------------------------------------------------------------
    const std::vector<std::string>& OpenLists()
    {
        return s_openIds;
    }

    void SetOpenLists(const std::vector<std::string>& aListIds)
    {
        const int today = TodayDay();

        /* With no connection every id looks missing, and a disconnect (or unload) must never cost
           the user their restored windows, so absences only count while a catalog is live. */
        const bool catalogLive = Catalog::IsConnected() && !Catalog::Lists().empty();

        std::vector<OpenEntry> merged;
        merged.reserve(aListIds.size() + s_data.OpenLists.size());

        for (const std::string& id : aListIds)
        {
            if (id.empty()) { continue; }
            if (ContainsId(merged, id)) { continue; }
            merged.push_back({ id, today });
        }

        const size_t openCount = merged.size();

        for (const OpenEntry& previous : s_data.OpenLists)
        {
            if (ContainsId(merged, previous.Id)) { continue; }

            if (catalogLive)
            {
                /* Present in the catalog but not open: the user closed it. */
                if (Catalog::Find(previous.Id) != nullptr) { continue; }

                /* Missing, and has been missing long enough to call it gone. */
                if (today - previous.LastSeenDay >= OPEN_LIST_EXPIRY_DAYS)
                {
                    Addon::Log(LOGL_DEBUG, "Forgetting open list %s, unseen for %d days.",
                               previous.Id.c_str(), today - previous.LastSeenDay);
                    continue;
                }
            }

            merged.push_back(previous);
        }

        BoundOpenLists(merged, openCount);

        bool changed = merged.size() != s_data.OpenLists.size();
        if (!changed)
        {
            for (size_t i = 0; i < merged.size(); ++i)
            {
                if (merged[i].Id != s_data.OpenLists[i].Id ||
                    merged[i].LastSeenDay != s_data.OpenLists[i].LastSeenDay)
                {
                    changed = true;
                    break;
                }
            }
        }

        if (!changed)
        {
            FlushIfDirty();
            return;
        }

        s_data.OpenLists = std::move(merged);
        RebuildOpenIds();

        SaveThrottled();
    }

    ///------------------------------------------------------------------------------------------------
    /// Collapsed "completed" sections
    ///
    /// A window is rebuilt from scratch every time it opens, so without this the section would come
    /// back expanded on every reopen. Ids of lists the website no longer offers are harmless: they
    /// are never read, and the user gets their preference back if the list ever returns.
    ///------------------------------------------------------------------------------------------------
    bool IsCompletedCollapsed(const std::string& aListId)
    {
        const auto& ids = s_data.CollapsedCompleted;
        return std::find(ids.begin(), ids.end(), aListId) != ids.end();
    }

    void SetCompletedCollapsed(const std::string& aListId, bool aCollapsed)
    {
        if (aListId.empty()) { return; }

        auto& ids = s_data.CollapsedCompleted;
        auto it = std::find(ids.begin(), ids.end(), aListId);

        if (aCollapsed == (it != ids.end())) { return; }

        if (aCollapsed) { ids.push_back(aListId); }
        else            { ids.erase(it); }

        Save();   /* a button click, not a drag: write it through */
    }

    ///------------------------------------------------------------------------------------------------
    /// Remembered window heights
    ///
    /// A window's own height is this value already clamped to whatever the list currently shows,
    /// so it cannot be read back off the window. It is stored at 100% scale.
    ///------------------------------------------------------------------------------------------------
    int ListHeight(const std::string& aListId)
    {
        for (const HeightEntry& entry : s_data.ListHeights)
        {
            if (entry.Id == aListId) { return entry.Height; }
        }
        return 0;
    }

    /* Called on every frame of a resize drag, so the writes below are throttled. */
    void SetListHeight(const std::string& aListId, int aHeight)
    {
        if (aListId.empty()) { return; }

        const int height = Clamp(aHeight, MIN_LIST_HEIGHT, MAX_LIST_HEIGHT);

        for (HeightEntry& entry : s_data.ListHeights)
        {
            if (entry.Id != aListId) { continue; }

            if (entry.Height == height)
            {
                FlushIfDirty();
                return;
            }

            entry.Height = height;
            SaveThrottled();
            return;
        }

        s_data.ListHeights.push_back({ aListId, height });
        SaveThrottled();
    }

    ///------------------------------------------------------------------------------------------------
    /// Options panel (RT_OptionsRender)
    ///
    /// Nexus owns the window; we only emit widgets. The chrome follows one website-styled look,
    /// and the opacity slider is the single knob the user has over it.
    ///------------------------------------------------------------------------------------------------
    void RenderOptions()
    {
        const float scale  = ImGui::GetIO().FontGlobalScale;
        const float indent = 4.0f;

        ///--------------------------------------------------------------------------------------------
        /// Appearance
        ///--------------------------------------------------------------------------------------------
        ImGui::TextUnformatted("Appearance");
        ImGui::Separator();
        ImGui::Indent(indent);

        int opacity = s_data.BgOpacityPct;
        ImGui::SetNextItemWidth(220.0f * scale);
        if (ImGui::SliderInt("Background opacity##gw2app", &opacity,
                             MIN_BG_OPACITY_PCT, MAX_BG_OPACITY_PCT, "%d%%"))
        {
            s_data.BgOpacityPct = Clamp(opacity, MIN_BG_OPACITY_PCT, MAX_BG_OPACITY_PCT);
            SaveThrottled();
        }

        bool showAccountName = s_data.ShowAccountName;
        if (ImGui::Checkbox("Show GW2 account name in list header##gw2app", &showAccountName))
        {
            s_data.ShowAccountName = showAccountName;
            Save();   /* discrete toggle: write it through immediately */
        }

        bool showCopyWaypoints = s_data.ShowCopyWaypoints;
        if (ImGui::Checkbox("Show \"Copy waypoints\" button in lists##gw2app", &showCopyWaypoints))
        {
            s_data.ShowCopyWaypoints = showCopyWaypoints;
            Save();
        }

        ImGui::Unindent(indent);
        ImGui::Spacing();

        ///--------------------------------------------------------------------------------------------
        /// Sizing
        ///
        /// The scale applies live, but the re-subscribe that makes the website re-rasterise images
        /// at the new render width waits for the drag to settle. It leaves load state alone, so a
        /// slider nudge costs a moment of softness rather than a spinner.
        ///--------------------------------------------------------------------------------------------
        ImGui::TextUnformatted("Sizing");
        ImGui::Separator();
        ImGui::Indent(indent);

        int uiScale = s_data.UiScalePct;
        ImGui::SetNextItemWidth(220.0f * scale);
        if (ImGui::SliderInt("List UI scale##gw2app", &uiScale,
                             MIN_UI_SCALE_PCT, MAX_UI_SCALE_PCT, "%d%%"))
        {
            s_data.UiScalePct = Clamp(uiScale, MIN_UI_SCALE_PCT, MAX_UI_SCALE_PCT);
            s_scalePending    = true;
            s_scaleTouchTime  = Now();
        }

        const bool released = ImGui::IsItemDeactivatedAfterEdit();

        ImGui::SameLine();
        if (ImGui::Button("Reset to 100%##gw2app"))
        {
            s_data.UiScalePct = DEF_UI_SCALE_PCT;
            s_scalePending    = true;
            s_scaleTouchTime  = 0.0;   /* a click is already a settled value */
        }

        if (s_scalePending && (released || Now() - s_scaleTouchTime >= SCALE_COMMIT_SECONDS))
        {
            s_scalePending = false;

            if (s_data.UiScalePct != s_scaleCommitted)
            {
                s_scaleCommitted = s_data.UiScalePct;
                Catalog::ResendSubscribe();
            }

            Save();
        }

        ImGui::Unindent(indent);
        ImGui::Spacing();

        ///--------------------------------------------------------------------------------------------
        /// Controls
        ///
        /// Nexus owns keybind assignment, so point the user at it.
        ///--------------------------------------------------------------------------------------------
        ImGui::TextUnformatted("Controls");
        ImGui::Separator();
        ImGui::Indent(indent);
        ImGui::TextDisabled("\"Show/hide all lists\" is assigned in Nexus' own Keybinds settings.");
        ImGui::Unindent(indent);
        ImGui::Spacing();

        ///--------------------------------------------------------------------------------------------
        /// Brand button, the same affordance as the info window's.
        ///--------------------------------------------------------------------------------------------
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(255, 123, 198, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(229, 110, 178, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(204,  98, 158, 255));
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32( 20,   4,  13, 255));

        if (ImGui::Button("Open gw2.app/nexus##gw2app", ImVec2(200.0f * scale, 30.0f * scale)))
        {
            Addon::OpenUrl(Addon::URL_NEXUS);
        }

        ImGui::PopStyleColor(4);

        /* Catch any edit that was throttled away while the panel stays open. */
        FlushIfDirty();
    }
}
