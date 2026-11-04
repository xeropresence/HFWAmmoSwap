#pragma once

namespace amlog
{
    // Opens HFWAmmoSwap.log next to the mod DLL (truncated each launch) and hooks up Native
    // Loader's console.
    void Open();
    void Write(const char* fmt, ...);
}

#define LOG(...) ::amlog::Write(__VA_ARGS__)
