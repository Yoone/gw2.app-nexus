///----------------------------------------------------------------------------------------------------
/// UI/Shell.cpp: the quick access icon, its menu, and the info window.
///
/// The icon is the module's only entry point: nothing opens on install. Left-clicking it invokes
/// KB_TOGGLE_LISTS (the peek toggle); right-clicking opens the menu Nexus anchors under the icon.
///
/// The menu is rebuilt from live state every frame, so it always shows the current catalog.
///----------------------------------------------------------------------------------------------------

#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "Addon.h"
#include "Catalog.h"
#include "Protocol.h"
#include "UI/UI.h"
#include "resource.h"

namespace
{
    ///------------------------------------------------------------------------------------------------
    /// State
    ///
    /// Render-thread only, like everything else in the UI layer.
    ///------------------------------------------------------------------------------------------------
    bool s_infoVisible = false;   /* also the bool* GUI_RegisterCloseOnEscape writes to */
    bool s_registered  = false;

    /* What Nexus is currently showing. Nexus copies the tooltip at QuickAccess_Add time, so the
       only way to change it is to re-register. We only do that when one of these changes. */
    std::string s_shortcutTexture;
    std::string s_shortcutTooltip;

    /* Brand colours, from the website's tokens. */
    constexpr ImU32 COL_BRAND        = IM_COL32(0xff, 0x7b, 0xc6, 0xff);
    constexpr ImU32 COL_BRAND_HOVER  = IM_COL32(0xff, 0x9a, 0xd5, 0xff);
    constexpr ImU32 COL_BRAND_ACTIVE = IM_COL32(0xe0, 0x62, 0xab, 0xff);
    constexpr ImU32 COL_BRAND_TEXT   = IM_COL32(0x14, 0x04, 0x0d, 0xff);
    constexpr ImU32 COL_CONNECTED    = IM_COL32(0x32, 0xcd, 0x32, 0xff);
    constexpr ImU32 COL_DISCONNECTED = IM_COL32(0xdc, 0x14, 0x3c, 0xff);

    ///------------------------------------------------------------------------------------------------
    /// Small helpers
    ///------------------------------------------------------------------------------------------------

    /* Nexus resolves a texture asynchronously, so the first calls return null. Ask again every
       frame rather than caching the miss. */
    Texture_t* GetTexture(const char* aIdentifier, uint32_t aResourceId)
    {
        if (APIDefs == nullptr) { return nullptr; }

        Texture_t* tex = APIDefs->Textures_Get(aIdentifier);
        if (tex == nullptr)
        {
            tex = APIDefs->Textures_GetOrCreateFromResource(aIdentifier, aResourceId, hSelf);
        }
        return tex;
    }

    /* ASCII-only, byte by byte. List and account names are UTF-8, but all we need is a stable
       case-insensitive order, not proper sorting for a locale. */
    int CompareNoCase(const std::string& aLhs, const std::string& aRhs)
    {
        const size_t shared = aLhs.size() < aRhs.size() ? aLhs.size() : aRhs.size();
        for (size_t i = 0; i < shared; ++i)
        {
            unsigned char l = (unsigned char)aLhs[i];
            unsigned char r = (unsigned char)aRhs[i];
            if (l >= 'A' && l <= 'Z') { l = (unsigned char)(l - 'A' + 'a'); }
            if (r >= 'A' && r <= 'Z') { r = (unsigned char)(r - 'A' + 'a'); }
            if (l != r) { return l < r ? -1 : 1; }
        }
        if (aLhs.size() == aRhs.size()) { return 0; }
        return aLhs.size() < aRhs.size() ? -1 : 1;
    }

    float UiScaling()
    {
        if (NexusLink != nullptr && NexusLink->Scaling > 0.0f) { return NexusLink->Scaling; }
        return 1.0f;
    }

    void TextCentered(const char* aText, ImU32 aColor)
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float width = ImGui::CalcTextSize(aText).x;
        if (width < avail)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width) * 0.5f);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, aColor);
        ImGui::TextUnformatted(aText);
        ImGui::PopStyleColor();
    }

    void VerticalGap(float aPixels)
    {
        ImGui::Dummy(ImVec2(0.0f, aPixels));
    }

    ///------------------------------------------------------------------------------------------------
    /// Derived state
    ///------------------------------------------------------------------------------------------------

    int HiddenCount()
    {
        return UI::ListWindows::HiddenCount();
    }

    /* These strings shipped as they are, so keep the wording. The hidden segment stays
       unpluralised: users read it as a count. */
    std::string TooltipText()
    {
        if (!Catalog::IsConnected()) { return "GW2.app (not connected)"; }

        const int lists  = (int)Catalog::Lists().size();
        const int hidden = HiddenCount();

        std::string text = "GW2.app (connected, " + std::to_string(lists) + (lists == 1 ? " list" : " lists");
        if (hidden > 0)
        {
            text += ", " + std::to_string(hidden) + " hidden";
        }
        text += ")";
        return text;
    }

    /* The slashed variant while anything is peeked away, so the user can see why their lists
       vanished. If that asset never loaded, keep the normal icon rather than a broken square. */
    std::string DesiredTexture()
    {
        if (UI::ListWindows::AnyHidden() && GetTexture(Addon::TEX_ICON_HIDDEN, IDB_GW2APP_ICON_HIDDEN) != nullptr)
        {
            return Addon::TEX_ICON_HIDDEN;
        }
        return Addon::TEX_ICON;
    }

    ///------------------------------------------------------------------------------------------------
    /// Quick access registration
    ///------------------------------------------------------------------------------------------------

    void RenderContextMenu();   /* fwd */

    /* Re-registers the shortcut. MUST NOT be called from RenderContextMenu: that callback runs
       while Nexus is walking its own quick access registry. */
    void ApplyShortcut(const std::string& aTexture, const std::string& aTooltip)
    {
        if (APIDefs == nullptr) { return; }

        if (s_registered)
        {
            APIDefs->QuickAccess_RemoveContextMenu(Addon::QA_CONTEXT_MENU);
            APIDefs->QuickAccess_Remove(Addon::QA_SHORTCUT);
        }

        /* The hover slot re-uses the normal icon identifier: one piece of art covers both states.
           The keybind identifier is what makes a click do anything; entry.cpp registers it and
           Nexus invokes it on left-click. */
        APIDefs->QuickAccess_Add(Addon::QA_SHORTCUT, aTexture.c_str(), aTexture.c_str(),
                                 Addon::KB_TOGGLE_LISTS, aTooltip.c_str());
        APIDefs->QuickAccess_AddContextMenu(Addon::QA_CONTEXT_MENU, Addon::QA_SHORTCUT, RenderContextMenu);

        s_shortcutTexture = aTexture;
        s_shortcutTooltip = aTooltip;
        s_registered      = true;
    }

    /* Cheap enough to run every frame: two string compares, and a re-registration only when the
       icon or the tooltip actually changed. */
    void SyncShortcut()
    {
        const std::string texture = DesiredTexture();
        const std::string tooltip = TooltipText();

        if (s_registered && texture == s_shortcutTexture && tooltip == s_shortcutTooltip) { return; }

        ApplyShortcut(texture, tooltip);
    }

    ///------------------------------------------------------------------------------------------------
    /// The menu
    ///------------------------------------------------------------------------------------------------

    struct Bucket
    {
        std::string                        Account;   /* "" = the no-account bucket, sorts first */
        std::vector<const Protocol::List*> Lists;
    };

    std::vector<Bucket> GroupByAccount()
    {
        std::vector<Bucket> buckets;

        for (const Protocol::List& list : Catalog::Lists())
        {
            /* A list with no id cannot be opened, so it is not offered. */
            if (list.Id.empty()) { continue; }

            Bucket* bucket = nullptr;
            for (Bucket& candidate : buckets)
            {
                if (CompareNoCase(candidate.Account, list.AccountName) == 0)
                {
                    bucket = &candidate;
                    break;
                }
            }

            if (bucket == nullptr)
            {
                buckets.push_back(Bucket{ list.AccountName, {} });
                bucket = &buckets.back();
            }

            bucket->Lists.push_back(&list);
        }

        /* Accounts case-insensitively, the no-account bucket first (it is the shortest string).
           Lists by name inside each, falling back to the id when the name is empty. */
        for (size_t i = 0; i + 1 < buckets.size(); ++i)
        {
            for (size_t j = i + 1; j < buckets.size(); ++j)
            {
                if (CompareNoCase(buckets[j].Account, buckets[i].Account) < 0)
                {
                    std::swap(buckets[i], buckets[j]);
                }
            }
        }

        for (Bucket& bucket : buckets)
        {
            for (size_t i = 0; i + 1 < bucket.Lists.size(); ++i)
            {
                for (size_t j = i + 1; j < bucket.Lists.size(); ++j)
                {
                    const std::string& a = bucket.Lists[i]->Name.empty() ? bucket.Lists[i]->Id : bucket.Lists[i]->Name;
                    const std::string& b = bucket.Lists[j]->Name.empty() ? bucket.Lists[j]->Id : bucket.Lists[j]->Name;
                    if (CompareNoCase(b, a) < 0)
                    {
                        std::swap(bucket.Lists[i], bucket.Lists[j]);
                    }
                }
            }
        }

        return buckets;
    }

    /* Nexus opens the popup and calls this inside it: emit items, no Begin/End of our own. */
    void RenderContextMenu()
    {
        try
        {
            if (ImGui::MenuItem("Show instructions"))
            {
                UI::Shell::ToggleInfoWindow();
            }

            /* Only meaningful once something is open. It is also how a user finds the peek
               toggle, whose keybind ships unbound. */
            if (UI::ListWindows::VisibleCount() > 0 || UI::ListWindows::AnyHidden())
            {
                bool hidden = UI::ListWindows::AnyHidden();
                if (ImGui::MenuItem("Hide all subscribed lists", nullptr, &hidden))
                {
                    UI::ListWindows::ToggleAllHidden();
                }
            }

            ImGui::Separator();

            if (!Catalog::IsConnected())
            {
                ImGui::TextDisabled("Not connected. Open gw2.app/nexus");
                return;
            }

            const std::vector<Bucket> buckets = GroupByAccount();
            if (buckets.empty())
            {
                ImGui::TextDisabled("Connected, no lists yet");
                return;
            }

            /* A single unnamed account has nothing to disambiguate, so the header is noise. */
            const bool showHeaders = buckets.size() > 1 || !buckets[0].Account.empty();

            for (size_t i = 0; i < buckets.size(); ++i)
            {
                const Bucket& bucket = buckets[i];

                if (showHeaders)
                {
                    if (i > 0) { ImGui::Separator(); }
                    ImGui::TextDisabled("%s", bucket.Account.empty() ? "Lists with no account"
                                                                     : bucket.Account.c_str());
                    /* The separator and the dimmed header carry the grouping on their own, so the
                       names stay unindented and close to the pointer. */
                }

                for (const Protocol::List* list : bucket.Lists)
                {
                    /* ## keeps the ImGui id unique when two accounts hold a same-named list. */
                    const std::string label =
                        (list->Name.empty() ? list->Id : list->Name) + "##" + list->Id;

                    if (ImGui::MenuItem(label.c_str()))
                    {
                        /* The popup closes on click anyway, and returning here keeps us from
                           walking pointers into a catalog the open may have moved on from. */
                        Catalog::OpenList(list->Id);
                        return;
                    }
                }

            }
        }
        catch (...)
        {
            /* Nexus calls this through a C function pointer, and an escaping exception takes the
               game down with it. */
        }
    }

    ///------------------------------------------------------------------------------------------------
    /// The info window
    ///------------------------------------------------------------------------------------------------

    void RenderStatusRow()
    {
        const bool connected = Catalog::IsConnected();
        const ImU32 color    = connected ? COL_CONNECTED : COL_DISCONNECTED;

        std::string label;
        if (connected)
        {
            const int lists = (int)Catalog::Lists().size();
            label = "Connected (" + std::to_string(lists) + (lists == 1 ? " list)" : " lists)");
        }
        else
        {
            label = "Not connected";
        }

        /* The dot takes the same colour as its label, so the two always agree. */
        const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        const float  radius   = ImGui::GetFontSize() * 0.25f;
        const float  gap      = ImGui::GetStyle().ItemInnerSpacing.x;
        const float  total    = (radius * 2.0f) + gap + textSize.x;
        const float  avail    = ImGui::GetContentRegionAvail().x;

        if (total < avail)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - total) * 0.5f);
        }

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(origin.x + radius, origin.y + (textSize.y * 0.5f)), radius, color, 20);

        ImGui::Dummy(ImVec2((radius * 2.0f) + gap, textSize.y));
        ImGui::SameLine(0.0f, 0.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
    }

    void RenderInfoWindow()
    {
        if (!s_infoVisible) { return; }

        const float scale = UiScaling();

        /* Fixed width, and a 0 height means ImGui auto-fits that axis to the content. */
        ImGui::SetNextWindowSize(ImVec2(430.0f * scale, 0.0f), ImGuiCond_Always);

        static const std::string label = UI::WindowLabel("Connect GW2.app", Addon::WINDOW_INFO);

        UI::PushWindowStyle(UI::AccentFor(""));

        if (ImGui::Begin(label.c_str(), &s_infoVisible,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar))
        {
            VerticalGap(6.0f * scale);

            /* Drawn at its native size, so the logo stays pixel exact. */
            Texture_t* logo = GetTexture(Addon::TEX_LOGO, IDB_GW2APP_LOGO);
            if (logo != nullptr && logo->Resource != nullptr && logo->Width > 0)
            {
                const ImVec2 size((float)logo->Width, (float)logo->Height);
                const float  avail = ImGui::GetContentRegionAvail().x;
                if (size.x < avail)
                {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - size.x) * 0.5f);
                }
                ImGui::Image((ImTextureID)logo->Resource, size);
            }

            VerticalGap(12.0f * scale);

            const ImU32 body = ImGui::GetColorU32(ImGuiCol_Text);
            TextCentered("Visit gw2.app/nexus in a web browser", body);
            TextCentered("and click Connect to send your lists in-game.", body);

            VerticalGap(12.0f * scale);

            const ImVec2 button(200.0f * scale, 30.0f * scale);
            const float  avail = ImGui::GetContentRegionAvail().x;
            if (button.x < avail)
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - button.x) * 0.5f);
            }

            ImGui::PushStyleColor(ImGuiCol_Button,        COL_BRAND);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_BRAND_HOVER);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  COL_BRAND_ACTIVE);
            ImGui::PushStyleColor(ImGuiCol_Text,          COL_BRAND_TEXT);
            if (ImGui::Button("Open gw2.app/nexus", button))
            {
                Addon::OpenUrl(Addon::URL_NEXUS);
            }
            ImGui::PopStyleColor(4);

            VerticalGap(10.0f * scale);

            RenderStatusRow();

            /* The "what now" hint only shows up once there is something to open. */
            if (Catalog::IsConnected())
            {
                VerticalGap(8.0f * scale);
                TextCentered("Right click the GW2.app icon at the top", body);
                TextCentered("to open the list(s) you want to track.", body);
            }

            VerticalGap(4.0f * scale);
        }

        ImGui::End();

        UI::PopWindowStyle();
    }
}

namespace UI
{
    namespace Shell
    {
        ///--------------------------------------------------------------------------------------------
        /// Init
        ///--------------------------------------------------------------------------------------------
        void Init()
        {
            try
            {
                if (APIDefs == nullptr) { return; }

                /* Queue the uploads. They land a few frames later; Nexus re-resolves the shortcut's
                   textures by identifier every frame, so registering it right away is fine. */
                APIDefs->Textures_GetOrCreateFromResource(Addon::TEX_ICON,        IDB_GW2APP_ICON,        hSelf);
                APIDefs->Textures_GetOrCreateFromResource(Addon::TEX_ICON_HIDDEN, IDB_GW2APP_ICON_HIDDEN, hSelf);
                APIDefs->Textures_GetOrCreateFromResource(Addon::TEX_LOGO,        IDB_GW2APP_LOGO,        hSelf);

                ApplyShortcut(Addon::TEX_ICON, "GW2.app (not connected)");

                /* Escape closes this window only. Escape must stay with the game's menu for list
                   windows, so do not register those: a stray press would unsubscribe a list. */
                APIDefs->GUI_RegisterCloseOnEscape(Addon::WINDOW_INFO, &s_infoVisible);
            }
            catch (...)
            {
            }
        }

        ///--------------------------------------------------------------------------------------------
        /// Shutdown
        ///--------------------------------------------------------------------------------------------
        void Shutdown()
        {
            try
            {
                s_infoVisible = false;

                if (APIDefs == nullptr) { return; }

                APIDefs->GUI_DeregisterCloseOnEscape(Addon::WINDOW_INFO);

                if (s_registered)
                {
                    APIDefs->QuickAccess_RemoveContextMenu(Addon::QA_CONTEXT_MENU);
                    APIDefs->QuickAccess_Remove(Addon::QA_SHORTCUT);
                    s_registered = false;
                }

                s_shortcutTexture.clear();
                s_shortcutTooltip.clear();
            }
            catch (...)
            {
            }
        }

        ///--------------------------------------------------------------------------------------------
        /// Render
        ///
        /// Safe to re-register the shortcut from here: this runs as an RT_Render callback, not from
        /// inside Nexus' quick access pass.
        ///--------------------------------------------------------------------------------------------
        void Render()
        {
            try
            {
                if (APIDefs == nullptr) { return; }

                SyncShortcut();
                RenderInfoWindow();
            }
            catch (...)
            {
            }
        }

        ///--------------------------------------------------------------------------------------------
        /// ToggleInfoWindow
        ///--------------------------------------------------------------------------------------------
        void ToggleInfoWindow()
        {
            s_infoVisible = !s_infoVisible;
        }

        ///--------------------------------------------------------------------------------------------
        /// RefreshShortcut
        ///
        /// The connection or the catalog moved. Render() would pick this up on its own next frame;
        /// this exists so callers outside the render path do not have to wait for one.
        ///--------------------------------------------------------------------------------------------
        void RefreshShortcut()
        {
            try
            {
                SyncShortcut();
            }
            catch (...)
            {
            }
        }
    }
}
