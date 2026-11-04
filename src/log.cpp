#include "log.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace amlog
{
    namespace
    {
        // Exported by Native Loader's winmm.dll; prints to its console when the mod folder has a
        // "console" file. Arguments: mod folder name, message.
        using NativeLoaderLog_t = void (*)(const char* mod, const char* message);

        FILE* g_file;
        NativeLoaderLog_t g_console;
        std::mutex g_mutex;
    }

    void Open()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Open), &self);

        wchar_t path[MAX_PATH];
        const DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
        if (len != 0 && len < MAX_PATH)
        {
            if (wchar_t* dot = wcsrchr(path, L'.'))
            {
                wcscpy_s(dot, MAX_PATH - (dot - path), L".log");
                g_file = _wfsopen(path, L"w", _SH_DENYWR);
            }
        }

        if (HMODULE loader = GetModuleHandleW(L"winmm.dll"))
            g_console = reinterpret_cast<NativeLoaderLog_t>(GetProcAddress(loader, "NativeLoaderLog"));
    }

    void Write(const char* fmt, ...)
    {
        char message[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(message, sizeof(message), fmt, args);
        va_end(args);

        SYSTEMTIME t;
        GetLocalTime(&t);

        std::lock_guard lock(g_mutex);
        if (g_file)
        {
            fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, message);
            fflush(g_file);
        }
        if (g_console)
            g_console("HFWAmmoSwap", message);
    }
}
