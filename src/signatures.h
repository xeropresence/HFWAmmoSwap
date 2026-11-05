#pragma once

#include <cstdint>

namespace signatures
{
    // Finds every game address the mod uses in the loaded image and stores it in game::rva.
    // Returns false (after logging what's missing) unless each one was found exactly once.
    bool Resolve(uintptr_t base);
}
