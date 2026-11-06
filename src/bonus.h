#pragma once

#include "game.h"

#include <cstdint>
#include <vector>

// Damage bonuses for ammo a weapon doesn't come with (see bonus.cpp).
namespace bonus
{
    // Resolves engine functions and creates (doesn't enable) the hooks that add the bonuses.
    bool CreateHooks(uintptr_t imageBase);

    // Brings the bonuses of a wielded weapon in line with its ammo slots after they changed.
    void Refresh(void* weapon);

    // While one lives, the bonus lists the game collects on this thread for a weapon treat it as
    // holding `ammo` instead of what its slots hold, so the damage an ammo would do on a weapon can be
    // shown before it is swapped in.
    class PreviewAmmo
    {
    public:
        explicit PreviewAmmo(const std::vector<game::GGUUID>& ammo);
        ~PreviewAmmo();
        PreviewAmmo(const PreviewAmmo&) = delete;
        PreviewAmmo& operator=(const PreviewAmmo&) = delete;

    private:
        const std::vector<game::GGUUID>* m_outer;
    };
}
