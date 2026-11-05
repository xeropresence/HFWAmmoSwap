#include "ammo.h"
#include "crashlog.h"
#include "hooks.h"
#include "log.h"
#include "signatures.h"

#include <Windows.h>

#ifndef AMMOSWAP_VERSION
#define AMMOSWAP_VERSION "dev"
#endif

// Native Loader (nexusmods.com/horizonforbiddenwest/mods/223) loads nativemods\HFWAmmoSwap\HFWAmmoSwap.dll
// from its WinMain hook and calls NativeLoaderInit before the game's WinMain runs. The callback table
// lets mods run code at game start or around RTTI type registration; this mod needs none of them.
struct NativeLoaderCallbacks
{
    void (*onGameStart)();
    void (*onTypesRegistering)();
    void (*onTypesRegistered)();
    void (*onTypeRegistering)(void* type);
    void (*onTypeRegistered)(void* type);
};

namespace
{
    uint32_t ImageTimestamp(uintptr_t base)
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return nt->FileHeader.TimeDateStamp;
    }
}

extern "C" __declspec(dllexport) void NativeLoaderInit(NativeLoaderCallbacks*)
{
    amlog::Open();
    crashlog::Install();

    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    LOG("Game build timestamp 0x%08X", ImageTimestamp(base));
    if (!signatures::Resolve(base))
    {
        LOG("This game build isn't supported; ammo swap disabled");
        return;
    }

    ammo::Init(base);
    if (!hooks::Install(base))
    {
        LOG("Hook installation failed; ammo swap disabled");
        return;
    }
    LOG("HFW Ammo Swap %s loaded", AMMOSWAP_VERSION);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
