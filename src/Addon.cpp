#include "Addon.h"

#include <shellapi.h>
#include <cstdarg>
#include <cstdio>

#include "Util/Util.h"

AddonAPI_t*      APIDefs   = nullptr;
HMODULE          hSelf     = nullptr;
NexusLinkData_t* NexusLink = nullptr;

namespace Addon
{
    void Log(ELogLevel aLevel, const char* aFormat, ...)
    {
        if (APIDefs == nullptr) { return; }

        char buffer[2048];
        va_list args;
        va_start(args, aFormat);
        vsnprintf(buffer, sizeof(buffer), aFormat, args);
        va_end(args);

        APIDefs->Log(aLevel, NAME, buffer);
    }

    void Alert(const std::string& aMessage)
    {
        if (APIDefs == nullptr) { return; }
        APIDefs->GUI_SendAlert(aMessage.c_str());
    }

    const std::string& DataDir()
    {
        static std::string dir;
        if (dir.empty() && APIDefs != nullptr)
        {
            const char* p = APIDefs->Paths_GetAddonDirectory("GW2app");
            if (p != nullptr)
            {
                dir = p;
                CreateDirectoryA(dir.c_str(), nullptr);
            }
        }
        return dir;
    }

    bool SetClipboardText(const std::string& aText)
    {
        /* Chat codes are ASCII but list names are not, so go through UTF-16. */
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, aText.c_str(), -1, nullptr, 0);
        if (wideLen <= 0) { return false; }

        if (!OpenClipboard(nullptr)) { return false; }

        bool ok = false;
        if (EmptyClipboard())
        {
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wideLen * sizeof(wchar_t));
            if (mem != nullptr)
            {
                void* dst = GlobalLock(mem);
                if (dst != nullptr)
                {
                    MultiByteToWideChar(CP_UTF8, 0, aText.c_str(), -1, (LPWSTR)dst, wideLen);
                    GlobalUnlock(mem);

                    if (SetClipboardData(CF_UNICODETEXT, mem) != nullptr)
                    {
                        ok = true;   /* clipboard owns the handle now */
                    }
                    else
                    {
                        GlobalFree(mem);
                    }
                }
                else
                {
                    GlobalFree(mem);
                }
            }
        }

        CloseClipboard();
        return ok;
    }

    void OpenUrl(const std::string& aUrl)
    {
        std::string url = aUrl;

        if (url.rfind("http://", 0) == 0)
        {
            url = "https://" + url.substr(7);
        }
        else if (url.rfind("https://", 0) != 0)
        {
            Log(LOGL_WARNING, "Refusing to open non-http(s) URL: %s", aUrl.c_str());
            return;
        }

        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
