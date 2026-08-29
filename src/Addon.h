///----------------------------------------------------------------------------------------------------
/// Addon.h: the Nexus handles, the identifiers and the small helpers the whole module shares.
///----------------------------------------------------------------------------------------------------
#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

#include "nexus/Nexus.h"

/* Set once in AddonLoad, valid until AddonUnload. */
extern AddonAPI_t*      APIDefs;
extern HMODULE          hSelf;
extern NexusLinkData_t* NexusLink;

namespace Addon
{
    constexpr const char* NAME             = "GW2.app";
    /* One above the Blish module's 38473, so both can run at the same time. */
    constexpr uint16_t    PORT             = 38474;
    constexpr int         PROTOCOL_VERSION = 2;   /* we accept clients 1..2 */

    /* Sent to the website on `subscribe`. The Blish module speaks the same protocol, so this
       is how the website knows which one answered. */
    constexpr const char* MODULE_ID = "nexus";

    /* Nexus registry identifiers. Keep every one of these unique and stable: they are
       keys in Nexus' own persisted files (InputBinds.json, imgui.ini). */
    constexpr const char* QA_SHORTCUT      = "QA_GW2APP";
    constexpr const char* QA_CONTEXT_MENU  = "QA_GW2APP_MENU";
    constexpr const char* KB_TOGGLE_LISTS  = "KB_GW2APP_TOGGLE_LISTS";

    constexpr const char* TEX_ICON         = "TEX_GW2APP_ICON";
    constexpr const char* TEX_ICON_HOVER   = "TEX_GW2APP_ICON_HOVER";
    constexpr const char* TEX_ICON_HIDDEN  = "TEX_GW2APP_ICON_HIDDEN";
    constexpr const char* TEX_LOGO         = "TEX_GW2APP_LOGO";

    constexpr const char* WINDOW_INFO      = "GW2.app";

    constexpr const char* URL_NEXUS        = "https://gw2.app/nexus";

    /* printf-style logging into Nexus.log, channel "GW2.app". Supports <c=#rrggbb>. */
    void Log(ELogLevel aLevel, const char* aFormat, ...);

    /* Transient on-screen toast (Nexus GUI_SendAlert). */
    void Alert(const std::string& aMessage);

    /* "<GW2>/addons/GW2app", created on first call. Our own config lives here. */
    const std::string& DataDir();

    /* Win32 clipboard. Returns false on failure; callers must surface that to the user. */
    bool SetClipboardText(const std::string& aText);

    /* Opens a URL in the default browser. Only http and https are accepted, and http is
       upgraded to https first, matching the Blish module. Anything else is logged and dropped. */
    void OpenUrl(const std::string& aUrl);
}
