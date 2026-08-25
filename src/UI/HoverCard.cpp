///----------------------------------------------------------------------------------------------------
/// HoverCard.cpp: the floating preview that appears beside a list window.
///
/// Dwell on a row for 200 ms and the website starts streaming a PNG of that entry's card; we blit
/// it, unscaled, next to the window that owns the row. Card content is the website's concern: the
/// module only ever draws the image it is handed.
///
/// Subscription model:
///     dwell past 200 ms   -> open_hover(listId, index)
///     move to another row -> open_hover(new)  (implicitly closes the previous one)
///     cursor leaves rows  -> close_hover(listId, index)
///
/// Every open_hover is balanced by exactly one close_hover, on every path out, so the website is
/// never left streaming a card that has gone.
///
/// Display states: hidden, a spinner while streaming, then the image, swapped in place on every
/// new frame. Trading Post cards keep re-streaming for as long as they are hovered, so a changing
/// image and a changing size are the normal case.
///
/// The card docks to one side of the window and takes its vertical anchor from the row, both
/// latched once per hover session. Keep them latched: a wider or taller incoming frame must not
/// snap the card across the window or make it jump.
///----------------------------------------------------------------------------------------------------

#include "UI.h"

#include <cmath>
#include <string>

#include "Addon.h"
#include "Catalog.h"
#include "Settings.h"

namespace UI::HoverCard
{
    namespace
    {
        constexpr double DWELL_MS      = 200.0;   /* hover this long before we open a subscription */
        constexpr double ANIM_MS       = 180.0;   /* entrance/exit length, bits-ui popover timing   */
        constexpr float  ANIM_SCALE    = 0.95f;   /* fade-in + zoom-in-95                           */
        constexpr float  WINDOW_GAP    = 5.0f;    /* window edge -> card                            */
        constexpr float  SPINNER_PX    = 64.0f;   /* placeholder square at 100% UI scale            */

        /* Phase of the one hover card that exists in the process.
             Idle     : nothing hovered, nothing open, nothing drawn.
             Dwelling : a row is hovered, timer running, nothing sent to the website yet.
             Open     : open_hover is outstanding; we draw the spinner, then the image.
             Fading   : the hover ended, close_hover is already sent, the last image fades out. */
        enum class Phase { Idle, Dwelling, Open, Fading };

        Phase       s_phase = Phase::Idle;

        /* The hover request the list windows push at us during a frame. Every open window calls
           Update() each frame, and the ones with nothing hovered pass an empty id, so an empty
           call must never clobber a real one from a sibling window in the same frame. */
        int         s_reqFrame = -1;
        bool        s_hasReq   = false;
        std::string s_reqListId;
        int         s_reqIndex = -1;
        ImVec2      s_reqRowMin, s_reqRowMax;
        ImVec2      s_reqWinMin, s_reqWinMax;

        /* Session (one dwell -> one card). Ids are kept through Fading so the fading image can
           still be looked up; `s_open` alone tracks whether the website owes us a close. */
        std::string s_sesListId;
        int         s_sesIndex   = -1;
        bool        s_open       = false;
        double      s_dwellStart = 0.0;

        /* Latched once per session (see the header note). The window rect is refreshed every frame
           so dragging the window carries the card with it; only the side is latched. */
        float       s_anchorY  = 0.0f;
        bool        s_dockRight = true;
        ImVec2      s_winMin, s_winMax;

        /* Entrance animation: armed when the subscription opens, fired when the first image of the
           session actually appears (cache hit, or the first hover_image frame after the spinner).
           Later frames in the same session swap in place with no re-pop. */
        bool        s_animPending = false;
        bool        s_animActive  = false;
        double      s_animStart   = 0.0;

        /* Last image rect drawn at full scale, so the exit fade has something to fade from without
           needing the owning window to still exist. */
        bool        s_hadImage = false;
        ImVec2      s_imageMin, s_imageMax;

        ///--------------------------------------------------------------------------------------------
        /// Helpers
        ///--------------------------------------------------------------------------------------------

        ImVec2 ScreenSize()
        {
            if (NexusLink != nullptr && NexusLink->Width > 0 && NexusLink->Height > 0)
            {
                return ImVec2((float)NexusLink->Width, (float)NexusLink->Height);
            }

            /* Before the first Mumble tick NexusLink can still be empty; ImGui always knows. */
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            if (display.x > 0.0f && display.y > 0.0f) { return display; }
            return ImVec2(1920.0f, 1080.0f);
        }

        float Snap(float aValue) { return std::floor(aValue + 0.5f); }

        /* ease-out cubic, the same curve in both directions. */
        float Eased(double aStartedAt, bool& aOutDone)
        {
            const double elapsed = (ImGui::GetTime() - aStartedAt) * 1000.0;
            float t = (elapsed <= 0.0) ? 0.0f : (float)(elapsed / ANIM_MS);
            if (t >= 1.0f) { t = 1.0f; aOutDone = true; }
            return 1.0f - std::pow(1.0f - t, 3.0f);
        }

        /* The website's contract: a hover may only be opened for an entry flagged has_hover_card.
           Doubles as the "did the list or the entry disappear under us" check, which is why it is
           re-evaluated every frame and not just at open time. */
        bool CanHover(const std::string& aListId, int aIndex)
        {
            if (aListId.empty() || aIndex < 0) { return false; }

            const Protocol::List* list = Catalog::Find(aListId);
            if (list == nullptr) { return false; }
            if (aIndex >= (int)list->Entries.size()) { return false; }

            return list->Entries[(size_t)aIndex].HasHoverCard;
        }

        bool TextureUsable(const Texture_t* aTexture)
        {
            return aTexture != nullptr
                && aTexture->Resource != nullptr
                && aTexture->Width  > 0
                && aTexture->Height > 0;
        }

        ///--------------------------------------------------------------------------------------------
        /// Subscription lifetime
        ///
        /// `s_open` is the single flag that says "the website has a stream open for us". It is set
        /// before the open is sent and cleared before the close is sent, so no failure path can
        /// double-send a close or lose one. A stray close is harmless; a lost one leaves the
        /// website streaming forever.
        ///--------------------------------------------------------------------------------------------

        void OpenSubscription()
        {
            s_open        = true;
            s_animPending = true;   /* arm the entrance for whichever image lands first */
            s_animActive  = false;
            s_hadImage    = false;
            s_phase       = Phase::Open;

            Catalog::OpenHover(s_sesListId, s_sesIndex);
        }

        void CloseSubscription()
        {
            if (!s_open) { return; }

            s_open = false;
            Catalog::CloseHover(s_sesListId, s_sesIndex);
        }

        /* Ends whatever session is running. `aAnimated` only matters when an image was actually on
           screen: a row-to-row move hides the old card at once, so the next card's dwell is never
           gated on the previous card's fade-out. */
        void EndSession(bool aAnimated)
        {
            if (s_phase == Phase::Idle) { return; }

            const bool fade = aAnimated && s_phase == Phase::Open && s_hadImage;

            CloseSubscription();

            s_animPending = false;
            s_animActive  = false;

            if (fade)
            {
                s_phase     = Phase::Fading;
                s_animStart = ImGui::GetTime();
                return;     /* keep the session ids and s_imageMin/Max for the fade */
            }

            s_phase    = Phase::Idle;
            s_hadImage = false;
            s_sesListId.clear();
            s_sesIndex = -1;
        }

        void BeginSession()
        {
            s_sesListId = s_reqListId;
            s_sesIndex  = s_reqIndex;
            s_dwellStart = ImGui::GetTime();
            s_phase      = Phase::Dwelling;

            /* Latched for the whole session. The anchor keeps the card growing symmetrically about
               a fixed point as frames change size; the side keeps a wider frame from snapping the
               card across the window. */
            s_anchorY = (s_reqRowMin.y + s_reqRowMax.y) * 0.5f;

            const ImVec2 screen = ScreenSize();
            const float  right  = screen.x - s_reqWinMax.x;
            const float  left   = s_reqWinMin.x;
            s_dockRight = right >= left;

            s_winMin   = s_reqWinMin;
            s_winMax   = s_reqWinMax;
            s_hadImage = false;
        }

        ///--------------------------------------------------------------------------------------------
        /// Per-frame state machine
        ///--------------------------------------------------------------------------------------------

        void Advance()
        {
            const int frame = ImGui::GetFrameCount();

            /* A request is live if it landed this frame or the previous one. The one-frame slack
               makes us independent of whether the list windows render before or after us; two
               frames of silence means the owning window stopped drawing (closed, unsubscribed,
               peek-hidden) and the session must end. */
            bool hovered = s_hasReq && (frame - s_reqFrame) <= 1;

            /* With no live client no hover_image can arrive, so an open would leave the spinner
               stuck to the cursor forever. This also tears down an open session the moment the
               connection drops. */
            if (!Catalog::IsConnected())  { hovered = false; }
            if (hovered && !CanHover(s_reqListId, s_reqIndex)) { hovered = false; }

            if (!hovered)
            {
                if (s_phase == Phase::Dwelling || s_phase == Phase::Open) { EndSession(true); }
                return;
            }

            const bool sameTarget = (s_phase == Phase::Dwelling || s_phase == Phase::Open)
                                 && s_sesIndex  == s_reqIndex
                                 && s_sesListId == s_reqListId;

            if (!sameTarget)
            {
                EndSession(false);      /* also collapses a running fade, unanimated */
                BeginSession();
            }
            else
            {
                s_winMin = s_reqWinMin; /* the window may have been dragged since we latched */
                s_winMax = s_reqWinMax;
            }

            if (s_phase == Phase::Dwelling && (ImGui::GetTime() - s_dwellStart) * 1000.0 >= DWELL_MS)
            {
                OpenSubscription();
            }
        }

        ///--------------------------------------------------------------------------------------------
        /// Drawing
        ///
        /// Everything goes onto the foreground draw list. The card sits above every window and
        /// takes no input at all: a real ImGui window would steal the mouse from the row under it.
        ///
        /// The card is a bare image. The website already drew the card's own chrome.
        ///--------------------------------------------------------------------------------------------

        /* Places a w*h card against the latched side of the owning window and clamps it on screen. */
        void PlaceCard(float aWidth, float aHeight, ImVec2& aOutMin, ImVec2& aOutMax)
        {
            const ImVec2 screen = ScreenSize();

            float x = s_dockRight ? (s_winMax.x + WINDOW_GAP)
                                  : (s_winMin.x - WINDOW_GAP - aWidth);
            float y = s_anchorY - aHeight * 0.5f;

            const float maxX = screen.x - aWidth;
            const float maxY = screen.y - aHeight;
            if (x > maxX) { x = maxX; }
            if (y > maxY) { y = maxY; }
            if (x < 0.0f) { x = 0.0f; }
            if (y < 0.0f) { y = 0.0f; }

            aOutMin = ImVec2(Snap(x), Snap(y));
            aOutMax = ImVec2(aOutMin.x + Snap(aWidth), aOutMin.y + Snap(aHeight));
        }

        /* Scales a placed rect about its own centre, for the zoom part of the animation. The
           destination rect moves and the texture is left alone, so at scale 1.0 nothing shifts
           and the blit stays pixel exact. */
        void ScaleRect(const ImVec2& aMin, const ImVec2& aMax, float aScale,
                       ImVec2& aOutMin, ImVec2& aOutMax)
        {
            if (aScale >= 1.0f) { aOutMin = aMin; aOutMax = aMax; return; }

            const float cx = (aMin.x + aMax.x) * 0.5f;
            const float cy = (aMin.y + aMax.y) * 0.5f;
            const float hw = (aMax.x - aMin.x) * 0.5f * aScale;
            const float hh = (aMax.y - aMin.y) * 0.5f * aScale;

            aOutMin = ImVec2(Snap(cx - hw), Snap(cy - hh));
            aOutMax = ImVec2(Snap(cx + hw), Snap(cy + hh));
        }

        /* The ratio row images are displayed at, which the card must match.

           The website captures hover cards at the same device scale as rows, so a card blitted at
           native size is too large by exactly the factor rows are shrunk by. The ratio is read off
           the row this card belongs to, so two lists captured at different scales each get their
           own.

           Returns 1.0 when the website honoured `render_width` (rows already 1:1) or when the
           row image has not arrived yet. */
        float CaptureScale(const std::string& aListId, int aIndex)
        {
            const int renderWidth = Settings::RenderWidth();
            if (renderWidth <= 0) { return 1.0f; }

            const Texture_t* row = Catalog::EntryImage(aListId, aIndex);
            if (row == nullptr || row->Width == 0) { return 1.0f; }

            const float rowW = (float)row->Width;
            if (rowW <= (float)renderWidth) { return 1.0f; }   /* rows are drawn 1:1 */

            return (float)renderWidth / rowW;
        }

        /* Card size: matched to the rows' display scale, then capped to the screen, aspect kept. */
        void CardSize(const Texture_t* aTexture, const std::string& aListId, int aIndex,
                      float& aOutWidth, float& aOutHeight)
        {
            const ImVec2 screen = ScreenSize();
            const float  capture = CaptureScale(aListId, aIndex);

            float w = (float)aTexture->Width  * capture;
            float h = (float)aTexture->Height * capture;

            float fit = 1.0f;
            if (w > screen.x && w > 0.0f)       { fit = screen.x / w; }
            if (h * fit > screen.y && h > 0.0f) { fit = screen.y / h; }

            aOutWidth  = std::floor(w * fit);
            aOutHeight = std::floor(h * fit);
            if (aOutWidth  < 1.0f) { aOutWidth  = 1.0f; }
            if (aOutHeight < 1.0f) { aOutHeight = 1.0f; }
        }

        void DrawImage(const Texture_t* aTexture, const ImVec2& aMin, const ImVec2& aMax, float aAlpha)
        {
            int a = (int)(aAlpha * 255.0f + 0.5f);
            if (a <= 0)   { return; }
            if (a >  255) { a = 255; }

            /* The fade is a vertex colour on the draw command, so the shared texture is never
               touched. Straight alpha, standard src/1-src blending. */
            ImGui::GetForegroundDrawList()->AddImage(
                (ImTextureID)aTexture->Resource, aMin, aMax,
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                IM_COL32(255, 255, 255, a));
        }

        /* Shown while the card is still streaming. It appears at full size and full opacity: an
           entrance animation here would read as the card having arrived. */
        void DrawSpinner()
        {
            const float side = Snap(SPINNER_PX * Settings::UiScale());

            ImVec2 cardMin, cardMax;
            PlaceCard(side, side, cardMin, cardMax);

            const ImVec2 centre((cardMin.x + cardMax.x) * 0.5f, (cardMin.y + cardMax.y) * 0.5f);
            const float  radius    = side * 0.32f;
            float        thickness = side * 0.09f;
            if (thickness < 2.0f) { thickness = 2.0f; }

            ImDrawList* dl = ImGui::GetForegroundDrawList();

            /* Track, then an arc in the primary colour sweeping once every ~1.6 s. */
            const float twoPi = 6.28318530718f;
            dl->PathArcTo(centre, radius, 0.0f, twoPi, 32);
            dl->PathStroke(IM_COL32(255, 255, 255, 40), false, thickness);

            const float head = (float)std::fmod(ImGui::GetTime() * 4.0, (double)twoPi);
            dl->PathArcTo(centre, radius, head, head + twoPi * 0.28f, 20);
            dl->PathStroke(IM_COL32(255, 123, 198, 235), false, thickness);
        }

        void Draw()
        {
            if (s_phase == Phase::Idle || s_phase == Phase::Dwelling) { return; }

            /* Never cached across frames: the catalog owns hover textures and replaces them under
               us on every incoming frame, and the entry can vanish entirely between frames. */
            const Texture_t* tex = Catalog::HoverImage(s_sesListId, s_sesIndex);
            const bool       ok  = TextureUsable(tex);

            if (s_phase == Phase::Fading)
            {
                if (!ok) { s_phase = Phase::Idle; s_hadImage = false; s_sesListId.clear(); s_sesIndex = -1; return; }

                bool  done   = false;
                const float eased = Eased(s_animStart, done);
                if (done)
                {
                    s_phase    = Phase::Idle;
                    s_hadImage = false;
                    s_sesListId.clear();
                    s_sesIndex = -1;
                    return;
                }

                /* Mirror of the entrance: opacity 1 -> 0, scale 1.00 -> 0.95, same curve. */
                const float progress = 1.0f - eased;
                ImVec2 min, max;
                ScaleRect(s_imageMin, s_imageMax, ANIM_SCALE + (1.0f - ANIM_SCALE) * progress, min, max);
                DrawImage(tex, min, max, progress);
                return;
            }

            /* Phase::Open */
            if (!ok)
            {
                DrawSpinner();
                return;
            }

            /* First image of this session: a cache hit at open, or the first streamed frame after
               the spinner. Later swaps in the same session must not re-pop. */
            if (s_animPending)
            {
                s_animPending = false;
                s_animActive  = true;
                s_animStart   = ImGui::GetTime();
            }

            float w = 0.0f, h = 0.0f;
            CardSize(tex, s_sesListId, s_sesIndex, w, h);

            PlaceCard(w, h, s_imageMin, s_imageMax);
            s_hadImage = true;

            float alpha = 1.0f;
            float scale = 1.0f;
            if (s_animActive)
            {
                bool done = false;
                const float eased = Eased(s_animStart, done);
                if (done) { s_animActive = false; }
                else      { alpha = eased; scale = ANIM_SCALE + (1.0f - ANIM_SCALE) * eased; }
            }

            ImVec2 min, max;
            ScaleRect(s_imageMin, s_imageMax, scale, min, max);
            DrawImage(tex, min, max, alpha);
        }

        void ClearAll()
        {
            s_phase       = Phase::Idle;
            s_open        = false;
            s_hasReq      = false;
            s_reqFrame    = -1;
            s_reqIndex    = -1;
            s_reqListId.clear();
            s_sesListId.clear();
            s_sesIndex    = -1;
            s_animPending = false;
            s_animActive  = false;
            s_hadImage    = false;
        }
    }

    ///------------------------------------------------------------------------------------------------
    /// Public surface. Nothing below may let an exception reach Nexus: an unhandled fault here
    /// takes Guild Wars 2 down with it.
    ///------------------------------------------------------------------------------------------------

    void Update(const std::string& aListId, int aIndex,
                const ImVec2& aRowMin, const ImVec2& aRowMax,
                const ImVec2& aWinMin, const ImVec2& aWinMax)
    {
        try
        {
            const int frame = ImGui::GetFrameCount();
            if (frame != s_reqFrame)
            {
                s_reqFrame = frame;
                s_hasReq   = false;
            }

            /* "Nothing hovered here". A sibling window may still have a live row this frame, so
               an empty call only ever declines to set a request; it never clears one. */
            if (aListId.empty() || aIndex < 0) { return; }

            s_hasReq   = true;
            s_reqListId = aListId;
            s_reqIndex  = aIndex;
            s_reqRowMin = aRowMin;
            s_reqRowMax = aRowMax;
            s_reqWinMin = aWinMin;
            s_reqWinMax = aWinMax;
        }
        catch (...)
        {
            Addon::Log(LOGL_WARNING, "HoverCard::Update failed.");
        }
    }

    void Render()
    {
        try
        {
            Advance();
            Draw();
        }
        catch (...)
        {
            /* Leave the card in a state that cannot leak a subscription, then stay quiet. */
            try { Reset(); } catch (...) {}
            Addon::Log(LOGL_WARNING, "HoverCard::Render failed; hover card reset.");
        }
    }

    /* Addon unload, connection lost, client superseded. The close still goes out: a queued message
       on a dead connection is discarded harmlessly, and skipping it would leak the stream. */
    void Reset()
    {
        try
        {
            CloseSubscription();
        }
        catch (...)
        {
            s_open = false;
        }

        ClearAll();
    }
}
