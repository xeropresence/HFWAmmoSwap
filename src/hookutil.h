#pragma once

#include "log.h"

#include <MinHook.h>

#include <cstdint>

// Creates (but doesn't enable) a MinHook detour at image base + rva.
template <class T>
bool CreateHook(uintptr_t base, uintptr_t rva, T detour, T* original, const char* name)
{
    void* target = reinterpret_cast<void*>(base + rva);
    if (MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)) != MH_OK)
    {
        LOG("Failed to hook %s", name);
        return false;
    }
    return true;
}
