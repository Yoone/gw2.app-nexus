///----------------------------------------------------------------------------------------------------
/// ListWindow.cpp: one window per open list, and the rows inside it.
///
/// Every list is drawn from the catalog's current state each frame, so there is no per-list cache
/// to keep in sync.
///
/// Two product rules hold throughout:
///   The website owns completion state, so do not flip a checkbox locally.
///   Escape must stay with the game's menu, so do not register these windows for close-on-escape.
///----------------------------------------------------------------------------------------------------

#include "UI/UI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "imgui/imgui_internal.h"   /* PushItemFlag and GetWindowResizeID live here in ImGui 1.80 */

#include "Addon.h"
#include "Catalog.h"
#include "Features.h"
#include "Settings.h"

namespace
{
    ///------------------------------------------------------------------------------------------------
    /// Tokens
    ///
    /// Only the numbers that describe content are scaled; hairlines stay at 1 px.
    ///------------------------------------------------------------------------------------------------
    constexpr float BASE_CHECKBOX        = 22.0f;
    constexpr float BASE_CHECKBOX_LEFT   = 12.0f;   /* content edge -> checkbox */
    /* Timers have an opaque background, so without this gap the row image looks welded to the
       checkbox. The column is derived from its three parts so they stay in step. */
    constexpr float BASE_CHECKBOX_GAP    = 8.0f;    /* checkbox -> row image */
    constexpr float BASE_CHECKBOX_COLUMN =          /* content edge -> row image */
        BASE_CHECKBOX_LEFT + BASE_CHECKBOX + BASE_CHECKBOX_GAP;
    constexpr float BASE_BUTTON_W        = 180.0f;
    constexpr float BASE_BUTTON_H        = 26.0f;
    /* List windows run with zero WindowPadding so the rows sit flush against the frame, which
       leaves the footer to carry its own padding. */
    constexpr float BASE_FOOTER_PAD      = 8.0f;
    constexpr float BASE_RESET_GAP       = 4.0f;    /* clock to countdown text */
    /* Digits have no descenders, so their ink sits above the middle of the text box and a
       geometrically centred clock reads high. Nudge it down to match by eye. */
    constexpr float BASE_RESET_GLYPH_NUDGE = 1.0f;
    constexpr float BASE_ROWS_BOTTOM     = 10.0f;   /* below the last row */
    constexpr float BASE_SECTION_GAP     = 6.0f;    /* above and below the completed toggle */
    /* Blish's 64 px spinner was a texture whose art carried its own padding, so the visible circle
       sat well inside the box. Our arc fills its box, so 40 matches the perceived size. */
    constexpr float BASE_LOAD_SPINNER    = 40.0f;
    constexpr float BASE_LOAD_TEXT_GAP   = 14.0f;   /* spinner -> label */
    /* Equal above the spinner and below the label, so the pair reads as vertically centred. */
    constexpr float BASE_LOAD_PAD        = 20.0f;
    constexpr float BASE_PEND_SPINNER    = 32.0f;   /* over a row while its toggle is in flight */
    constexpr float BASE_WINDOW_MIN_H    = 120.0f;
    constexpr float BASE_WINDOW_BASE_H   = 130.0f;  /* loading / failed / empty fit */
    constexpr float BASE_WINDOW_MAX_H    = 440.0f;  /* auto-fit ceiling */
    constexpr float BASE_WINDOW_DRAG_H   = 1200.0f; /* absolute ceiling for a deliberate drag */

    constexpr float BASE_COPY_SIDE_PAD   = 20.0f;
    constexpr float BASE_COPY_RIGHT_PAD  = 25.0f;
    constexpr float BASE_COPY_CHUNK_H    = 30.0f;
    constexpr float BASE_COPY_CHUNK_GAP  = 4.0f;

    /* White at 15 %. ImGui wants straight alpha, so the RGB stays at full white. */
    constexpr ImU32 COL_DIVIDER = IM_COL32(255, 255, 255, 38);

    constexpr ImU32 COL_MUTED   = IM_COL32(0xD3, 0xD3, 0xD3, 0xFF);
    constexpr ImU32 COL_ERROR   = IM_COL32(0xF2, 0x55, 0x5A, 0xFF);
    constexpr ImU32 COL_SPINNER = IM_COL32(0xE6, 0xE8, 0xEC, 0xFF);
    constexpr ImU32 COL_CHECK_DONE = IM_COL32(0x8A, 0x8F, 0x98, 0xFF);

    /* Multiplied onto the row image as a vertex colour while a toggle is in flight. Tinting stays
       a draw-command colour, so the shared texture is never rewritten. */
    const ImVec4 TINT_PENDING = ImVec4(110.0f / 255.0f, 110.0f / 255.0f, 110.0f / 255.0f, 1.0f);
    const ImVec4 TINT_NORMAL  = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);


    ///------------------------------------------------------------------------------------------------
    /// Per-list UI state, private to this file.
    ///------------------------------------------------------------------------------------------------
    struct ListUI
    {
        bool  CompletedCollapsed = false;
        bool  CopyMode           = false;

        /* The window auto-fits its content until the user resizes it. Once this latches, the
           height is theirs and we stop forcing one. */
        bool  UserResized        = false;
        float NeededHeight       = 0.0f;   /* measured last frame; content + chrome */
        float RequestedHeight    = 0.0f;   /* what we asked ImGui for last frame */
        float ActualHeight       = 0.0f;   /* what ImGui gave us last frame */
        bool  Placed             = false;
        bool  Fresh              = true;   /* first frame: adopt the persisted height */
        /* The height to return to once a fit-to-content view (loading / failed / empty) ends.
           Read straight out of ImGui's persisted settings on the first frame, so a reload can
           recover it without ever having to display it. */
        float SavedHeight        = 0.0f;
        bool  WasFixedFit        = false;
        bool  Resizing           = false;   /* user had the resize grip last frame */
    };

    std::map<std::string, ListUI> s_state;

    /* Peek hide: purely visual. The list stays in Catalog::OpenLists(), so it stays subscribed
       and restoring costs no re-stream. */
    std::set<std::string>    s_hidden;
    std::vector<std::string> s_prevOpen;

    /* Where the next new window lands. It cascades, so opening several lists does not stack
       them on one spot. */
    ImVec2 s_spawn = ImVec2(300.0f, 300.0f);

    /* At most one row is hovered per frame; the hover card is told once, at the end. */
    struct HoverReq
    {
        bool        Active = false;
        std::string ListId;
        int         Index  = -1;
        ImVec2      RowMin, RowMax, WinMin, WinMax;
    };

    ///------------------------------------------------------------------------------------------------
    /// Small helpers
    ///------------------------------------------------------------------------------------------------
    float Scale()
    {
        const float s = Settings::UiScale();
        return (s > 0.1f && s < 4.0f) ? s : 1.0f;
    }

    float RowImageWidth(float aScale)
    {
        const int w = Settings::RenderWidth();
        return (w > 0) ? (float)w : std::round(400.0f * aScale);
    }

    void TextAt(ImVec2 aPos, ImU32 aColor, const char* aText)
    {
        ImGui::GetWindowDrawList()->AddText(ImVec2(std::round(aPos.x), std::round(aPos.y)),
                                            aColor, aText);
    }

    void DrawSpinner(ImDrawList* aDrawList, ImVec2 aCenter, float aRadius, float aThickness, ImU32 aColor)
    {
        if (aDrawList == nullptr || aRadius <= 1.0f) { return; }

        const float t     = (float)ImGui::GetTime();
        const float start = t * 3.2f;

        aDrawList->PathClear();
        aDrawList->PathArcTo(aCenter, aRadius, start, start + IM_PI * 1.45f, 24);
        aDrawList->PathStroke(aColor, false, aThickness);
    }

    void DrawResetGlyph(ImDrawList* aDrawList, ImVec2 aCenter, float aRadius, ImU32 aColor)
    {
        if (aDrawList == nullptr || aRadius <= 2.0f) { return; }

        const float th = std::max(1.0f, aRadius * 0.22f);
        aDrawList->AddCircle(aCenter, aRadius, aColor, 16, th);
        aDrawList->AddLine(aCenter, ImVec2(aCenter.x, aCenter.y - aRadius * 0.55f), aColor, th);
        aDrawList->AddLine(aCenter, ImVec2(aCenter.x + aRadius * 0.45f, aCenter.y), aColor, th);
    }

    float ResetGlyphRadius()
    {
        return std::round(ImGui::GetFontSize() * 0.36f);
    }

    /* Clock, gap, then the text. Both the title budget and the painter need this. */
    float ResetBlockWidth(const std::string& aText, float aScale)
    {
        if (aText.empty()) { return 0.0f; }
        return ResetGlyphRadius() * 2.0f
             + std::round(BASE_RESET_GAP * aScale)
             + ImGui::CalcTextSize(aText.c_str()).x;
    }

    std::string TruncateToWidth(const std::string& aText, float aMaxWidth)
    {
        if (aText.empty() || aMaxWidth <= 0.0f) { return std::string(); }
        if (ImGui::CalcTextSize(aText.c_str()).x <= aMaxWidth) { return aText; }

        const char* tail = "...";
        const float tailW = ImGui::CalcTextSize(tail).x;

        size_t fit = 0;
        for (size_t i = 1; i <= aText.size(); ++i)
        {
            /* Never cut a UTF-8 sequence in half. */
            if (i < aText.size() && ((unsigned char)aText[i] & 0xC0) == 0x80) { continue; }
            if (ImGui::CalcTextSize(aText.substr(0, i).c_str()).x + tailW > aMaxWidth) { break; }
            fit = i;
        }

        if (fit == 0) { return std::string(); }
        return aText.substr(0, fit) + tail;
    }

    bool IsWindowResizeActive(ImGuiWindow* aWindow)
    {
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (g == nullptr || aWindow == nullptr || g->ActiveId == 0) { return false; }

        for (int n = 0; n < 8; ++n)   /* 0..3 corner grips, 4..7 borders */
        {
            if (g->ActiveId == ImGui::GetWindowResizeID(aWindow, n)) { return true; }
        }
        return false;
    }

    bool CenteredButton(const char* aLabel, float aAreaX, float aAreaW, float aY, float aScale)
    {
        const float w = std::min(std::round(BASE_BUTTON_W * aScale), std::max(40.0f, aAreaW));
        const float h = std::round(BASE_BUTTON_H * aScale);

        ImGui::SetCursorScreenPos(ImVec2(std::round(aAreaX + (aAreaW - w) * 0.5f), std::round(aY)));
        return ImGui::Button(aLabel, ImVec2(w, h));
    }

    int CountCodes(const std::string& aGroup)
    {
        int  n     = 0;
        bool inTok = false;
        for (char c : aGroup)
        {
            if (c == ' ') { inTok = false; }
            else if (!inTok) { inTok = true; ++n; }
        }
        return n;
    }

    void CopyEntryName(const std::string& aName)
    {
        if (aName.empty()) { return; }

        /* Waypoints::CopyToClipboard toasts the chat-link wording, so a name copy goes straight
           to the clipboard here. A failure is shown to the user, not just logged. */
        if (Addon::SetClipboardText(aName)) { Addon::Alert("Name copied"); }
        else                                { Addon::Alert("Couldn't copy to clipboard"); }
    }

    ///------------------------------------------------------------------------------------------------
    /// Rows
    ///------------------------------------------------------------------------------------------------
    struct RowCtx
    {
        const Protocol::List* List         = nullptr;
        float                 X0           = 0.0f;   /* left edge of the row strip, screen space */
        float                 CheckboxCol  = 0.0f;   /* 0 for the loot bag: no checkbox column */
        float                 MaxImageW    = 0.0f;
        float                 DividerW     = 0.0f;
        float                 Scale        = 1.0f;
        ImVec2                WinMin, WinMax;
        HoverReq*             Hover        = nullptr;
    };

    /* Returns the vertical space consumed, or 0 when the row was skipped entirely. */
    float RenderRow(const RowCtx& aCtx, int aIndex, float aPenY, bool aDivider)
    {
        const Protocol::List& list = *aCtx.List;
        if (aIndex < 0 || (size_t)aIndex >= list.Entries.size()) { return 0.0f; }

        const Protocol::Entry& entry = list.Entries[(size_t)aIndex];

        Texture_t* tex = Catalog::EntryImage(list.Id, aIndex);
        if (tex == nullptr || tex->Resource == nullptr || tex->Width == 0 || tex->Height == 0)
        {
            /* A row is its image, so one with no image takes no vertical space. Bulk re-imaging
               shows the loading state instead, so this only fires for a single failed decode. */
            return 0.0f;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float       used     = 0.0f;

        if (aDivider)
        {
            const float y = std::floor(aPenY) + 0.5f;   /* crisp hairline */
            drawList->AddLine(ImVec2(aCtx.X0, y), ImVec2(aCtx.X0 + aCtx.DividerW, y), COL_DIVIDER, 1.0f);
            used += 1.0f;
        }

        const float rowTop = std::floor(aPenY + used);

        /* Row images are drawn at native size on integer coordinates, which keeps them pixel
           exact. The website captures at render_width, so the aspect-preserving shrink below
           only comes into play for an older website that captured wider than we asked for. */
        float w = (float)tex->Width;
        float h = (float)tex->Height;
        if (aCtx.MaxImageW > 0.0f && w > aCtx.MaxImageW)
        {
            h = std::round(h * aCtx.MaxImageW / w);
            w = aCtx.MaxImageW;
        }
        if (w < 1.0f) { w = 1.0f; }
        if (h < 1.0f) { h = 1.0f; }

        const bool pending  = Catalog::IsPending(list.Id, aIndex);
        const bool autoDone = entry.AutoCompleted;

        ImGui::PushID(aIndex);

        /* --- checkbox --------------------------------------------------------------------- */
        if (aCtx.CheckboxCol > 0.0f)
        {
            const float box = std::round(BASE_CHECKBOX * aCtx.Scale);
            const float pad = std::max(0.0f, (box - ImGui::GetFontSize()) * 0.5f);

            /* Centred against the row image. Row heights vary a lot, so a fixed offset drifts. */
            ImGui::SetCursorScreenPos(ImVec2(std::round(aCtx.X0 + BASE_CHECKBOX_LEFT * aCtx.Scale),
                                             std::round(rowTop + (h - box) * 0.5f)));

            const bool locked = autoDone || pending;

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad, pad));
            if (locked)
            {
                /* ImGui 1.80 spells BeginDisabled() out by hand: item flag plus a dimmed alpha. */
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
            }

            bool checked = entry.Completed || autoDone;

            /* The shared theme draws no frame border, which leaves an unticked box nearly
               invisible against a row image. The primary colour while there is something to do,
               grey once it is done. */
            const ImU32 boxInk     = checked ? COL_CHECK_DONE : UI::COL_BRAND;
            const ImU32 boxBorder  = (boxInk & ~IM_COL32_A_MASK)
                                   | ((ImU32)(0.70f * 255.0f) << IM_COL32_A_SHIFT);

            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, std::round(aCtx.Scale)));
            ImGui::PushStyleColor(ImGuiCol_Border,    boxBorder);
            ImGui::PushStyleColor(ImGuiCol_CheckMark, boxInk);

            const bool toggled = ImGui::Checkbox("##done", &checked);

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();

            if (toggled && !locked)
            {
                /* The protocol forbids completing a loot bag row or an auto-completed one, so
                   the send path is guarded here as well as in the drawing above. The website
                   owns completion state: the row settles when the website re-renders it. */
                if (!list.IsLootBag && !autoDone)
                {
                    Catalog::SetCompleted(list.Id, aIndex, checked);
                }
            }

            if (locked)
            {
                ImGui::PopStyleVar();
                ImGui::PopItemFlag();
            }
            ImGui::PopStyleVar();
        }

        /* --- image ------------------------------------------------------------------------ */
        const ImVec2 imgMin(std::floor(aCtx.X0 + aCtx.CheckboxCol), rowTop);
        const ImVec2 imgMax(imgMin.x + w, imgMin.y + h);

        ImGui::SetCursorScreenPos(imgMin);
        ImGui::Image((ImTextureID)tex->Resource, ImVec2(w, h), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                     pending ? TINT_PENDING : TINT_NORMAL);

        /* Image() adds no id of its own, so an invisible button over the same rect owns the
           hit test, the context menu and the hover. */
        ImGui::SetCursorScreenPos(imgMin);
        const bool clicked = ImGui::InvisibleButton("##hit", ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();

        if (pending)
        {
            DrawSpinner(drawList,
                        ImVec2(imgMin.x + w * 0.5f, imgMin.y + h * 0.5f),
                        std::round(BASE_PEND_SPINNER * aCtx.Scale) * 0.5f,
                        std::max(2.0f, 2.5f * aCtx.Scale),
                        COL_SPINNER);
        }

        if (clicked)
        {
            if (!entry.ChatLink.empty())
            {
                /* Copied verbatim, including every space-separated code: a PSNA row is meant to
                   copy all four vendor waypoints at once. */
                Waypoints::CopyToClipboard(entry.ChatLink);
            }
            else if (!entry.Link.empty())
            {
                /* Deliberate precedence: a chat link always wins. Per protocol `link` only ever
                   arrives on `custom` entries, so no type test is needed here. Addon::OpenUrl
                   owns the http/https-only policy, which is a security boundary. */
                Addon::OpenUrl(entry.Link);
            }
        }

        /* Created lazily, so a row with nothing to offer opens no menu at all. An ImGui popup
           with no items simply never opens, which makes that free. */
        if (ImGui::BeginPopupContextItem("##rowmenu"))
        {
            if (!entry.Name.empty() && ImGui::MenuItem("Copy name"))
            {
                CopyEntryName(entry.Name);
            }
            ImGui::EndPopup();
        }

        if (hovered)
        {
            const char* prefix = nullptr;
            const std::string* target = nullptr;

            if      (!entry.ChatLink.empty()) { prefix = "Click to copy: "; target = &entry.ChatLink; }
            else if (!entry.Link.empty())     { prefix = "Click to open ";  target = &entry.Link;     }

            if (prefix != nullptr)
            {
                /* A PSNA chat link is long enough to run off the screen, so the tooltip wraps. */
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(std::round(360.0f * aCtx.Scale));
                ImGui::TextUnformatted((std::string(prefix) + *target).c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            /* The protocol allows a hover only for an entry flagged HasHoverCard, so this reads
               the latest state every frame. */
            if (entry.HasHoverCard && aCtx.Hover != nullptr && !aCtx.Hover->Active)
            {
                aCtx.Hover->Active = true;
                aCtx.Hover->ListId = list.Id;
                aCtx.Hover->Index  = aIndex;
                aCtx.Hover->RowMin = imgMin;
                aCtx.Hover->RowMax = imgMax;
                aCtx.Hover->WinMin = aCtx.WinMin;
                aCtx.Hover->WinMax = aCtx.WinMax;
            }
        }

        ImGui::PopID();

        used += h;
        return used;
    }

    ///------------------------------------------------------------------------------------------------
    /// The entries view, and the completed grouping the module applies
    ///------------------------------------------------------------------------------------------------
    float RenderEntries(const Protocol::List& aList, ListUI& aState, const RowCtx& aCtx)
    {
        const float scale = aCtx.Scale;
        const ImVec2 start = ImGui::GetCursorScreenPos();

        float pen             = start.y;
        bool  sectionHasRows  = false;   /* no leading divider at the top of a section */

        if (!aList.SortEntries)
        {
            /* STATIC: wire order. The website owns sort and content, so keep sorting and
               filtering out of here. */
            for (size_t i = 0; i < aList.Entries.size(); ++i)
            {
                const float used = RenderRow(aCtx, (int)i, pen, sectionHasRows);
                if (used > 0.0f) { pen += used; sectionHasRows = true; }
            }
        }
        else
        {
            /* GROUP_COMPLETED. An entry counts as completed when either flag is set, and the (N)
               on the toggle includes the auto-completed ones. */
            int completed = 0;
            for (const Protocol::Entry& e : aList.Entries)
            {
                if (e.Completed || e.AutoCompleted) { ++completed; }
            }

            for (size_t i = 0; i < aList.Entries.size(); ++i)
            {
                const Protocol::Entry& e = aList.Entries[i];
                if (e.Completed || e.AutoCompleted) { continue; }

                const float used = RenderRow(aCtx, (int)i, pen, sectionHasRows);
                if (used > 0.0f) { pen += used; sectionHasRows = true; }
            }

            if (completed > 0)
            {
                const float gap = std::round(BASE_SECTION_GAP * scale);

                char label[64];
                std::snprintf(label, sizeof(label), "%s completed (%d)",
                              aState.CompletedCollapsed ? "Show" : "Hide", completed);

                pen += gap;
                if (CenteredButton(label, aCtx.X0, aCtx.DividerW, pen, scale))
                {
                    aState.CompletedCollapsed = !aState.CompletedCollapsed;
                }
                pen += std::round(BASE_BUTTON_H * scale) + gap;

                if (!aState.CompletedCollapsed)
                {
                    sectionHasRows = false;   /* the section starts without a leading divider */
                    for (size_t i = 0; i < aList.Entries.size(); ++i)
                    {
                        const Protocol::Entry& e = aList.Entries[i];
                        if (!(e.Completed || e.AutoCompleted)) { continue; }

                        const float used = RenderRow(aCtx, (int)i, pen, sectionHasRows);
                        if (used > 0.0f) { pen += used; sectionHasRows = true; }
                    }
                }
            }
        }

        pen += std::round(BASE_ROWS_BOTTOM * scale);

        /* Tell ImGui how tall the scrollable content is. */
        ImGui::SetCursorScreenPos(ImVec2(start.x, pen));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));

        return pen - start.y;
    }

    ///------------------------------------------------------------------------------------------------
    /// Copy-waypoints mode
    ///------------------------------------------------------------------------------------------------
    float RenderCopyMode(const std::vector<std::string>& aCodes, float aInnerW, float aScale)
    {
        const float sidePad = std::round(BASE_COPY_SIDE_PAD * aScale);
        const float topPad  = std::round(BASE_COPY_SIDE_PAD * aScale);
        const float chunkH  = std::round(BASE_COPY_CHUNK_H * aScale);
        const float chunkGp = std::round(BASE_COPY_CHUNK_GAP * aScale);
        const float width   = std::max(60.0f, aInnerW - sidePad - std::round(BASE_COPY_RIGHT_PAD * aScale));

        const ImVec2 start = ImGui::GetCursorScreenPos();
        float        pen   = start.y + topPad;
        const float  x     = start.x + sidePad;

        int maxPerCopy = Settings::MaxWaypointsPerCopy();
        if (maxPerCopy < 1)  { maxPerCopy = 1;  }
        if (maxPerCopy > 15) { maxPerCopy = 15; }

        /* At the top of the range the slider reads "Max": there the chunker packs by GW2's
           199-character chat budget instead of by count. */
        char header[64];
        if (maxPerCopy >= 15) { std::snprintf(header, sizeof(header), "Max waypoints per message: Max"); }
        else                  { std::snprintf(header, sizeof(header), "Max waypoints per message: %d", maxPerCopy); }

        TextAt(ImVec2(x, pen), COL_MUTED, header);
        pen += ImGui::GetTextLineHeight() + std::round(6.0f * aScale);

        ImGui::SetCursorScreenPos(ImVec2(std::round(x), std::round(pen)));
        ImGui::SetNextItemWidth(width);

        int slider = maxPerCopy;
        if (ImGui::SliderInt("##maxwp", &slider, 1, 15))
        {
            if (slider < 1)  { slider = 1;  }
            if (slider > 15) { slider = 15; }
            if (slider != maxPerCopy)
            {
                Settings::SetMaxWaypointsPerCopy(slider);
                maxPerCopy = slider;
            }
        }
        pen += ImGui::GetFrameHeight() + std::round(10.0f * aScale);

        const std::vector<std::string> groups = Waypoints::Chunk(aCodes, maxPerCopy);
        for (size_t i = 0; i < groups.size(); ++i)
        {
            char label[64];
            std::snprintf(label, sizeof(label), "Copy group %d (%d)", (int)i + 1, CountCodes(groups[i]));

            ImGui::SetCursorScreenPos(ImVec2(std::round(x), std::round(pen)));
            if (ImGui::Button(label, ImVec2(width, chunkH)))
            {
                Waypoints::CopyToClipboard(groups[i]);
            }
            pen += chunkH + chunkGp;
        }

        pen += std::round(BASE_COPY_SIDE_PAD * aScale);

        ImGui::SetCursorScreenPos(ImVec2(start.x, pen));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));

        return pen - start.y;
    }

    ///------------------------------------------------------------------------------------------------
    /// One list window
    ///------------------------------------------------------------------------------------------------
    void RenderList(const Protocol::List& aList, HoverReq& aHover)
    {
        /* Held by value: RetryList and friends can reshape the catalog mid-frame, and aList is a
           reference into it. A dangling read here would take Guild Wars 2 down with it. */
        const std::string listId = aList.Id;

        ListUI& state = s_state[listId];

        const float scale = Scale();
        const ImU32 accent = UI::AccentFor(aList.Color);

        UI::PushWindowStyle(accent);
        const ImGuiStyle& style = ImGui::GetStyle();   /* reflects what we just pushed */

        /* The account name goes in the title as "List name (Account.1234)": it qualifies which
           list this is, so it belongs with the name. WindowLabel's "###" form keeps the geometry
           stable even though the visible text changes. */
        const std::string listName = aList.Name.empty() ? listId : aList.Name;

        std::string account;
        if (Settings::ShowAccountName() && !aList.AccountName.empty())
        {
            account = " (" + aList.AccountName + ")";
        }

        const std::string reset   = Countdown::For(aList.Reset);
        const std::string stableId = "###GW2APP_" + listId;

        /* --- geometry --------------------------------------------------------------------- */
        const float imageW      = RowImageWidth(scale);
        const float checkboxCol = aList.IsLootBag ? 0.0f : std::round(BASE_CHECKBOX_COLUMN * scale);

        /* The width is locked: row images have a fixed width, so a resizable window would clip or
           letterbox them. Re-deriving it every frame also corrects a width that imgui.ini restored
           at a different UI scale.

           There is no WindowPadding term because the scrolling area sits flush to the edges. */
        const float windowW = std::round(checkboxCol + imageW + style.ScrollbarSize
                                         + 2.0f * scale);

        /* ImGui clips the title to the whole bar, so a long name would run under the countdown we
           paint on the right. Budget for the collapse arrow, the close button and the countdown,
           then shorten the title ourselves. */
        const float buttonW    = ImGui::GetFontSize() + style.FramePadding.x * 2.0f;
        const float countdownW = reset.empty()
            ? 0.0f
            : ResetBlockWidth(reset, scale) + style.ItemSpacing.x;
        const float titleAvail = windowW - buttonW * 2.0f - countdownW - style.FramePadding.x;

        /* Shorten the list name, not the account name: which account a list belongs to is the
           part that disambiguates two lists with the same name. */
        const float accountW = account.empty() ? 0.0f : ImGui::CalcTextSize(account.c_str()).x;

        std::string title = TruncateToWidth(listName, titleAvail - accountW) + account;
        if (ImGui::CalcTextSize(title.c_str()).x > titleAvail)
        {
            title = TruncateToWidth(listName + account, titleAvail);
        }

        const std::string label = UI::WindowLabel(title, listId);

        const float chromeH = ImGui::GetFrameHeight();
        const float baseH   = std::round(BASE_WINDOW_BASE_H * scale);
        const float maxH    = std::round(BASE_WINDOW_MAX_H * scale);
        const float minH    = std::round(BASE_WINDOW_MIN_H * scale);

        const Catalog::LoadState load = Catalog::StateOf(listId);
        const bool fixedFit = (load == Catalog::LoadState::Failed)
                           || (load == Catalog::LoadState::Loading)
                           || aList.Entries.empty();

        const float needed  = (state.NeededHeight > 0.0f) ? state.NeededHeight : baseH;
        const float fitH    = std::max(minH, std::min(std::max(needed, baseH), maxH));
        const float dragCap = std::min(std::round(BASE_WINDOW_DRAG_H * scale),
                                       std::max(baseH, needed));

        /* Read the persisted height out of ImGui's settings instead of watching the window: a
           reload's first frame is nearly always a Loading frame, which force-fits to the spinner,
           so the restored height is never on screen to observe. ImHashStr resets its seed at
           "###", so this is the id ImGui derives from the label whatever the display name is. */
        if (state.Fresh)
        {
            if (ImGuiWindowSettings* ws = ImGui::FindWindowSettings(ImHashStr(stableId.c_str())))
            {
                if (ws->Size.y > 0)
                {
                    state.SavedHeight = (float)ws->Size.y;
                    state.UserResized = true;   /* a persisted height IS a stated preference */
                }
            }
        }

        /* A height we did not ask for means the user dragged it. Runs before the branching so a
           fit-to-content frame cannot skip it. */
        if (!state.UserResized && !state.Fresh && state.ActualHeight > 0.0f
            && std::fabs(state.ActualHeight - state.RequestedHeight) > 2.0f)
        {
            state.UserResized = true;
            state.SavedHeight = state.ActualHeight;
        }

        /* Loading, failed and empty render compact whatever height the list normally has: a
           spinner and one line of text in a 600 px window looks broken. The height comes back
           with the content, in the branch below. */
        if (fixedFit)
        {
            ImGui::SetNextWindowSizeConstraints(ImVec2(windowW, fitH), ImVec2(windowW, fitH));
            ImGui::SetNextWindowSize(ImVec2(windowW, fitH), ImGuiCond_Always);
            state.RequestedHeight = fitH;
        }
        else if (state.WasFixedFit && state.UserResized && state.SavedHeight > 0.0f)
        {
            /* Content just arrived: restore the height the user had before the compact view. */
            const float restore = std::max(minH, std::min(state.SavedHeight,
                                                          std::round(BASE_WINDOW_DRAG_H * scale)));
            ImGui::SetNextWindowSizeConstraints(ImVec2(windowW, minH),
                                                ImVec2(windowW, std::round(BASE_WINDOW_DRAG_H * scale)));
            ImGui::SetNextWindowSize(ImVec2(windowW, restore), ImGuiCond_Always);
            state.RequestedHeight = restore;
        }
        else if (state.Fresh)
        {
            /* First sighting with content already there. FirstUseEver lets a persisted size win
               over the fit. */
            ImGui::SetNextWindowSizeConstraints(ImVec2(windowW, minH),
                                                ImVec2(windowW, std::round(BASE_WINDOW_DRAG_H * scale)));
            ImGui::SetNextWindowSize(ImVec2(windowW, fitH), ImGuiCond_FirstUseEver);
            state.RequestedHeight = fitH;
        }
        else
        {
            /* The content-height ceiling stops the user dragging a window taller than it has rows
               for, so it applies only while they are dragging.

               Applying it every frame was destructive. Just after a load some row images have not
               uploaded yet, so the measured content is briefly short, and the ceiling clamped the
               window down to that transient height, which ImGui then persisted to imgui.ini. */
            const float ceiling = state.Resizing
                ? std::max(minH, dragCap)
                : std::round(BASE_WINDOW_DRAG_H * scale);

            ImGui::SetNextWindowSizeConstraints(ImVec2(windowW, minH), ImVec2(windowW, ceiling));
            if (!state.UserResized)
            {
                ImGui::SetNextWindowSize(ImVec2(windowW, fitH), ImGuiCond_Always);
                state.RequestedHeight = fitH;
            }
        }

        if (!state.Placed)
        {
            state.Placed = true;

            ImVec2 spawn = s_spawn;
            if (NexusLink != nullptr && NexusLink->Width > 0 && NexusLink->Height > 0)
            {
                /* Clamp into the current screen, so a window saved on a monitor that is no longer
                   there still comes back reachable. */
                spawn.x = std::min(spawn.x, (float)NexusLink->Width  - windowW - 20.0f);
                spawn.y = std::min(spawn.y, (float)NexusLink->Height - fitH    - 20.0f);
                spawn.x = std::max(spawn.x, 0.0f);
                spawn.y = std::max(spawn.y, 0.0f);
            }
            ImGui::SetNextWindowPos(spawn, ImGuiCond_FirstUseEver);

            s_spawn.x += std::round(28.0f * scale);
            s_spawn.y += std::round(28.0f * scale);
            if (NexusLink != nullptr && NexusLink->Height > 0
                && s_spawn.y > (float)NexusLink->Height * 0.6f)
            {
                s_spawn = ImVec2(300.0f, 300.0f);
            }
        }

        /* --- the window ------------------------------------------------------------------- */
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar
                               | ImGuiWindowFlags_NoScrollWithMouse
                               | ImGuiWindowFlags_NoNavInputs;   /* never claim the keyboard */
        if (fixedFit) { flags |= ImGuiWindowFlags_NoResize; }


        /* Escape must stay with the game's menu, so do not register these windows for
           close-on-escape: a stray press would silently unsubscribe the list. */
        ImGui::PushStyleColor(ImGuiCol_Text, UI::HeaderTextOn(accent));
        /* Zero padding so the scrolling area is flush against the frame on all four sides. ImGui
           reads WindowPadding inside Begin, so it has to be pushed around the call and popped
           straight after. The shared window style keeps its padding for the info window. */
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        bool open    = true;
        bool visible = ImGui::Begin(label.c_str(), &open, flags);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        if (visible)
        {
            state.Fresh        = false;
            state.ActualHeight = ImGui::GetWindowSize().y;

            ImGuiWindow* window = ImGui::GetCurrentWindow();

            /* ImGui title bars are left-aligned text and nothing else, so the countdown is
               painted straight onto the bar. The window's clip rect covers the content area
               only, hence the temporary one. */
            if (!reset.empty())
            {
                const ImGuiStyle& st       = ImGui::GetStyle();
                const float       titleH   = ImGui::GetFrameHeight();
                const float       closeW   = ImGui::GetFontSize() + st.FramePadding.x * 2.0f;
                const ImVec2      textSize = ImGui::CalcTextSize(reset.c_str());
                const ImVec2      winSize  = ImGui::GetWindowSize();

                const ImVec2 barMin(window->Pos.x, window->Pos.y);
                const ImVec2 barMax(window->Pos.x + winSize.x, window->Pos.y + titleH);

                const float glyphR = ResetGlyphRadius();
                const float gap    = std::round(BASE_RESET_GAP * scale);
                const float blockW = ResetBlockWidth(reset, scale);
                const float blockX = barMax.x - closeW - st.FramePadding.x - blockW;
                const float y      = barMin.y + (titleH - textSize.y) * 0.5f;

                const ImU32 ink = UI::HeaderTextOn(accent);

                const float glyphY = std::round(y + textSize.y * 0.5f
                                                + BASE_RESET_GLYPH_NUDGE * scale);

                window->DrawList->PushClipRect(barMin, barMax, false);
                DrawResetGlyph(window->DrawList,
                               ImVec2(std::round(blockX + glyphR), glyphY), glyphR, ink);
                window->DrawList->AddText(ImVec2(std::floor(blockX + glyphR * 2.0f + gap),
                                                 std::floor(y)), ink, reset.c_str());
                window->DrawList->PopClipRect();
            }

            state.Resizing = !fixedFit && IsWindowResizeActive(window);
            if (state.Resizing)
            {
                /* The user has expressed a preference; stop force-fitting. ImGui keeps the size
                   from here on and the constraint above re-clamps it to real content. */
                state.UserResized = true;
            }

            /* Keep the restore target current, but only from frames showing real content. A
               fit-to-content frame would otherwise overwrite it with the spinner's height. */
            if (!fixedFit && state.UserResized && state.ActualHeight > 0.0f)
            {
                state.SavedHeight = state.ActualHeight;
            }
            state.WasFixedFit = fixedFit;

            const ImVec2 winMin = ImGui::GetWindowPos();
            const ImVec2 winMax = ImVec2(winMin.x + ImGui::GetWindowSize().x,
                                         winMin.y + ImGui::GetWindowSize().y);

            const float contentW = ImGui::GetContentRegionAvail().x;
            const ImVec2 bodyTop = ImGui::GetCursorScreenPos();

            float bodyH = 0.0f;

            /* Waypoints are gathered from the live list every frame; the chunker is the only part
               worth caching and it only runs in copy mode. */
            std::vector<std::string> codes;
            bool footer = false;
            if (!fixedFit && Settings::ShowCopyWaypointsButton())
            {
                codes  = Waypoints::Gather(aList);
                footer = !codes.empty();
            }
            if (!footer) { state.CopyMode = false; }   /* force-exit whenever it cannot apply */

            /* The footer supplies its own padding above and below the button. With no footer the
               scrolling area runs all the way to the bottom edge, flush like the other three. */
            const float footerPad = std::round(BASE_FOOTER_PAD * scale);
            const float footerH   = footer
                ? std::round(BASE_BUTTON_H * scale) + footerPad * 2.0f
                : 0.0f;

            ///--------------------------------------------------------------------------------
            /// The render states, in priority order: a failed list shows the retry path, and
            /// never a spinner.
            ///--------------------------------------------------------------------------------
            if (load == Catalog::LoadState::Failed)
            {
                const float pad = std::round(10.0f * scale);
                float pen = bodyTop.y + pad;

                const char* msg = "Failed to load list";
                const float msgW = ImGui::CalcTextSize(msg).x;
                TextAt(ImVec2(bodyTop.x + (contentW - msgW) * 0.5f, pen), COL_ERROR, msg);
                pen += ImGui::GetTextLineHeight() + std::round(8.0f * scale);

                if (CenteredButton("Retry", bodyTop.x, contentW, pen, scale))
                {
                    /* Re-subscribes without the id, then with it, which the website already
                       honours as a retry. */
                    Catalog::RetryList(listId);
                }
                pen += std::round(BASE_BUTTON_H * scale) + std::round(20.0f * scale);

                bodyH = pen - bodyTop.y;
            }
            else if (load == Catalog::LoadState::Loading)
            {
                int have = 0, total = 0;
                Catalog::LoadProgress(listId, have, total);

                const float pad     = std::round(BASE_LOAD_PAD * scale);
                const float spinner = std::round(BASE_LOAD_SPINNER * scale);
                float pen = bodyTop.y + pad;

                DrawSpinner(ImGui::GetWindowDrawList(),
                            ImVec2(bodyTop.x + contentW * 0.5f, pen + spinner * 0.5f),
                            spinner * 0.5f, std::max(2.0f, 3.0f * scale), COL_SPINNER);
                pen += spinner + std::round(BASE_LOAD_TEXT_GAP * scale);

                char msg[48];
                if (total > 0) { std::snprintf(msg, sizeof(msg), "Loading %d / %d", have, total); }
                else           { std::snprintf(msg, sizeof(msg), "Loading..."); }

                const float msgW = ImGui::CalcTextSize(msg).x;
                TextAt(ImVec2(bodyTop.x + (contentW - msgW) * 0.5f, pen), COL_MUTED, msg);
                pen += ImGui::GetTextLineHeight() + pad;

                bodyH = pen - bodyTop.y;
            }
            else if (aList.Entries.empty())
            {
                const char* msg  = "Empty list";
                const float msgW = ImGui::CalcTextSize(msg).x;
                const float boxH = std::max(std::round(60.0f * scale),
                                            ImGui::GetContentRegionAvail().y);

                TextAt(ImVec2(bodyTop.x + (contentW - msgW) * 0.5f,
                              bodyTop.y + (boxH - ImGui::GetTextLineHeight()) * 0.5f),
                       COL_MUTED, msg);

                bodyH = std::round(70.0f * scale);
            }
            else
            {
                float availH = ImGui::GetContentRegionAvail().y - footerH;
                if (availH < std::round(40.0f * scale)) { availH = std::round(40.0f * scale); }

                /* Rows are laid out flush inside the scrolling child; the scrollbar is always
                   reserved so a row's x never shifts when it appears. */
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::BeginChild("##body", ImVec2(0.0f, availH), false,
                                  ImGuiWindowFlags_AlwaysVerticalScrollbar
                                  | ImGuiWindowFlags_NoNavInputs);

                const float innerW = ImGui::GetContentRegionAvail().x;

                if (state.CopyMode)
                {
                    bodyH = RenderCopyMode(codes, innerW, scale);
                }
                else
                {
                    RowCtx ctx;
                    ctx.List        = &aList;
                    ctx.X0          = ImGui::GetCursorScreenPos().x;
                    ctx.CheckboxCol = checkboxCol;
                    ctx.MaxImageW   = std::max(1.0f, innerW - checkboxCol);
                    ctx.DividerW    = std::max(1.0f, innerW);
                    ctx.Scale       = scale;
                    ctx.WinMin      = winMin;
                    ctx.WinMax      = winMax;
                    ctx.Hover       = &aHover;

                    bodyH = RenderEntries(aList, state, ctx);
                }

                ImGui::EndChild();
                ImGui::PopStyleVar();

                if (footer)
                {
                    /* Anchor to the child's bottom edge, not to the post-EndChild cursor: ImGui
                       adds ItemSpacing.y there, which pushed the button down and ate the padding
                       that was meant to sit below it. This way above and below match exactly. */
                    const float y = bodyTop.y + availH + footerPad;
                    if (CenteredButton(state.CopyMode ? "Back" : "Copy waypoints",
                                       bodyTop.x + footerPad, contentW - footerPad * 2.0f,
                                       y, scale))
                    {
                        state.CopyMode = !state.CopyMode;
                    }
                }

                bodyH += footerH;
            }

            state.NeededHeight = chromeH + bodyH;
        }

        ImGui::End();
        UI::PopWindowStyle();

        if (!open)
        {
            /* The X is a real unsubscribe. Programmatic teardown has its own path, so keep the
               two apart. */
            Catalog::CloseList(listId);
        }
    }
}

namespace UI
{
    namespace ListWindows
    {
        void Render()
        {
            /* Copy: closing a window mutates the open set mid-iteration. */
            const std::vector<std::string> open = Catalog::OpenLists();

            /* Opening a list cancels peek hide and brings everything back. A list id that was not
               open last frame is a new one, so the catalog needs no hook for this. */
            for (const std::string& id : open)
            {
                if (std::find(s_prevOpen.begin(), s_prevOpen.end(), id) == s_prevOpen.end())
                {
                    s_hidden.clear();
                    break;
                }
            }
            s_prevOpen = open;

            /* Drop view state and peek flags for lists that are no longer open. */
            for (auto it = s_state.begin(); it != s_state.end(); )
            {
                if (std::find(open.begin(), open.end(), it->first) == open.end())
                {
                    it = s_state.erase(it);
                }
                else { ++it; }
            }
            for (auto it = s_hidden.begin(); it != s_hidden.end(); )
            {
                if (std::find(open.begin(), open.end(), *it) == open.end())
                {
                    it = s_hidden.erase(it);
                }
                else { ++it; }
            }

            HoverReq hover;

            for (const std::string& id : open)
            {
                if (s_hidden.count(id) != 0) { continue; }

                /* A list can vanish from the catalog between frames. */
                const Protocol::List* list = Catalog::Find(id);
                if (list == nullptr) { continue; }

                RenderList(*list, hover);
            }

            if (hover.Active)
            {
                HoverCard::Update(hover.ListId, hover.Index,
                                  hover.RowMin, hover.RowMax, hover.WinMin, hover.WinMax);
            }
            else
            {
                HoverCard::Update(std::string(), -1,
                                  ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f),
                                  ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f));
            }
        }

        void ToggleAllHidden()
        {
            if (!s_hidden.empty()) { RestoreHidden(); return; }

            /* Visual only. The open set (and so the subscription set) is untouched, so restoring
               costs no re-stream. */
            for (const std::string& id : Catalog::OpenLists())
            {
                s_hidden.insert(id);
            }
        }

        bool AnyHidden()
        {
            return !s_hidden.empty();
        }

        int VisibleCount()
        {
            int n = 0;
            for (const std::string& id : Catalog::OpenLists())
            {
                if (s_hidden.count(id) == 0) { ++n; }
            }
            return n;
        }

        int HiddenCount()
        {
            /* Count against the open set, not s_hidden.size(): a hidden list that has since
               left the catalog must not inflate the tooltip. */
            int n = 0;
            for (const std::string& id : Catalog::OpenLists())
            {
                if (s_hidden.count(id) != 0) { ++n; }
            }
            return n;
        }

        void RestoreHidden()
        {
            s_hidden.clear();
        }
    }
}
