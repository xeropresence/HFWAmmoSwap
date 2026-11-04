#pragma once

#include <cstdint>

// "Edit Ammo" in the inventory: a prompt next to "Edit Coils" on ranged weapons that opens the game's
// edit-coils view, fed with the weapon's ammo slots and the compatible ammo the player's weapons have.
namespace menu
{
    // Resolves engine functions and creates the UI hooks (enabled together with all others).
    bool CreateHooks(uintptr_t imageBase);
}
