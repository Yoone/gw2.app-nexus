///----------------------------------------------------------------------------------------------------
/// GW2.app: Nexus module
///
/// Addon entry point. Registers with Nexus, wires the render callbacks, and starts the server.
///
/// The work lives in:
///   Server.cpp    localhost HTTP and WebSocket transport, on its own threads
///   Protocol.cpp  the JSON wire format the website speaks
///   Catalog.cpp   lists, subscriptions, textures
///   Settings.cpp  settings file and the Nexus options panel
///   Features.cpp  waypoint chunking and reset countdowns
///   UI/*.cpp      quick access shell, list windows, hover cards
///----------------------------------------------------------------------------------------------------

#include <Windows.h>
#include <cstring>

#include "nexus/Nexus.h"
#include "imgui/imgui.h"

#include "Addon.h"
#include "Catalog.h"
#include "Server.h"
#include "Settings.h"
#include "UI/UI.h"

void AddonLoad(AddonAPI_t* aApi);
void AddonUnload();
void AddonRender();
void AddonOptions();
void OnInputBind(const char* aIdentifier, bool aIsRelease);

AddonDefinition_t AddonDef = {};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH: hSelf = hModule; break;
        default: break;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    /* Negative signature = not hosted on Raidcore. Swap for the assigned positive id if we
       ever publish through Nexus' library. */
    AddonDef.Signature = (uint32_t)-1645843523;
    AddonDef.APIVersion = NEXUS_API_VERSION;

    AddonDef.Name = "GW2.app";
    AddonDef.Version.Major = 0;
    AddonDef.Version.Minor = 1;
    AddonDef.Version.Build = 0;
    AddonDef.Version.Revision = 0;
    AddonDef.Author = "Yoone";
    AddonDef.Description =
        "Access your GW2.app lists in game to work on your goals. Open gw2.app/nexus to get "
        "started! Supports waypoint copying, achievement tracking, timers, story progress, "
        "equipped gear, Trading Post price/order/history watchers, daily fractals, "
        "Wizard's Vault, PSNA, and much more.";

    AddonDef.Load = AddonLoad;
    AddonDef.Unload = AddonUnload;
    AddonDef.Flags = AF_None;

    AddonDef.Provider = UP_GitHub;
    AddonDef.UpdateLink = "https://github.com/Yoone/gw2.app-nexus";

    return &AddonDef;
}

void AddonLoad(AddonAPI_t* aApi)
{
    APIDefs = aApi;

    /* ImGui keeps its context and allocators in per-DLL statics, so we adopt the host's.
       Without this our widgets go into a context nobody draws. */
    ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
    ImGui::SetAllocatorFunctions(
        (void* (*)(size_t, void*))APIDefs->ImguiMalloc,
        (void  (*)(void*, void*))APIDefs->ImguiFree);

    NexusLink = (NexusLinkData_t*)APIDefs->DataLink_Get("DL_NEXUS_LINK");

    Settings::Load();

    APIDefs->GUI_Register(RT_Render, AddonRender);
    APIDefs->GUI_Register(RT_OptionsRender, AddonOptions);

    /* Unbound by default, on purpose: it avoids colliding with a GW2 or Nexus binding on
       first run, and the same action is reachable from the quick access menu. */
    APIDefs->InputBinds_RegisterWithString(Addon::KB_TOGGLE_LISTS, OnInputBind, "(null)");

    /* Nexus labels the bind's row with Translate(identifier), so without this entry it reads
       "KB_GW2APP_TOGGLE_LISTS". Other locales fall back to the English string. */
    APIDefs->Localization_Set(Addon::KB_TOGGLE_LISTS, "en", "GW2.app: Show/hide all lists");

    UI::Shell::Init();

    if (!Server::Start(Addon::PORT))
    {
        Addon::Log(LOGL_CRITICAL,
            "Could not listen on port %u. Another GW2.app client may already be running.",
            (unsigned)Addon::PORT);
    }

    Addon::Log(LOGL_INFO, "GW2.app loaded.");
}

void AddonUnload()
{
    Server::Stop();

    Settings::Save();

    UI::Shell::Shutdown();
    UI::HoverCard::Reset();

    APIDefs->InputBinds_Deregister(Addon::KB_TOGGLE_LISTS);
    APIDefs->GUI_Deregister(AddonRender);
    APIDefs->GUI_Deregister(AddonOptions);

    Catalog::Reset();

    Addon::Log(LOGL_INFO, "GW2.app unloaded.");
}

void OnInputBind(const char* aIdentifier, bool aIsRelease)
{
    if (aIsRelease || aIdentifier == nullptr) { return; }

    if (strcmp(aIdentifier, Addon::KB_TOGGLE_LISTS) == 0)
    {
        /* Nexus also fires this bind on a left-click of the quick access icon. With no lists
           open, toggling their visibility would look like nothing happened, so a first-run
           user gets the instructions instead. */
        if (Catalog::OpenLists().empty())
        {
            UI::Shell::ToggleInfoWindow();
        }
        else
        {
            UI::ListWindows::ToggleAllHidden();
        }
    }
}

/* Drain the network before drawing, so every surface sees the same state this frame. */
void AddonRender()
{
    Catalog::Pump();

    UI::Shell::Render();
    UI::ListWindows::Render();
    UI::HoverCard::Render();

    Settings::Tick();
}

void AddonOptions()
{
    Settings::RenderOptions();
}
