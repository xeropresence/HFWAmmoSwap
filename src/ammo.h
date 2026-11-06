#pragma once

#include "game.h"

#include <vector>

namespace ammo
{
    // Resolves engine functions; must be called once before anything else here.
    void Init(uintptr_t imageBase);

    // Weapon item -> its InventoryItemResource (null if not loaded).
    void* ItemResource(void* item);

    // True for weapons that support more than one ammo type (the weapons this mod edits).
    bool HasSwappableAmmo(void* itemResource);

    // InventoryItem a component of a spawned item entity (weapon) belongs to, or null.
    void* ItemOfComponent(void* component);

    // Player inventory component of an item's owner, or null.
    void* OwnerInventory(void* item);

    // Ammo entity-resource UUIDs currently assigned to the weapon, in slot order.
    std::vector<game::GGUUID> GetAssigned(void* item, int* selectedSlot);

    // Puts `ammo` into slot `slotIndex` of the weapon, in place. Fails if the weapon doesn't support
    // that ammo or another slot already holds it.
    bool SetSlotAmmo(void* item, int slotIndex, const game::GGUUID& ammo);

    // Appends a slot holding `ammo` (up to SlotCapacity). Same rules as SetSlotAmmo.
    bool AddSlot(void* item, const game::GGUUID& ammo);

    // Removes a slot; later slots move down one. The last remaining slot can't be removed.
    bool RemoveSlot(void* item, int slotIndex);

    // Gives the weapon its DefaultAmmo again, in default slot order.
    bool RestoreDefaults(void* item);

    // How many ammo slots the weapon has at its upgrade level (removed slots included).
    int SlotCapacity(void* weapon);

    // Ammo the weapon may take: entries of its SupportedAmmos that some weapon the owner carries holds
    // or comes with (ammo is "extracted" by copy from those weapons). Ordered like SupportedAmmos.
    std::vector<game::GGUUID> Candidates(void* item);

    // Ammo InventoryItemResource for an ammo entity UUID on this weapon, or null.
    void* SupportedAmmoItem(void* item, const game::GGUUID& ammo);

    // Ammo entity UUID of an ammo InventoryItemResource.
    game::GGUUID AmmoOfItemResource(void* ammoItemResource);

    // The owner's stack (InventoryItem) of an ammo type, or null if they never had any.
    void* AmmoStack(void* weapon, const game::GGUUID& ammo);

    // The ammo a weapon comes with at its upgrade level, in slot order: AmmoSlotsSettings.InitialAmmoConfig,
    // then the AmmoUnlocks of each upgrade applied so far (upgrading appends a slot per unlocked ammo).
    std::vector<game::GGUUID> DefaultAmmo(void* weapon);

    // Upgrade level of a weapon: -1 before the first upgrade.
    int UpgradeLevel(void* weapon);

    // Hook bodies (installed by hooks.cpp)
    void OnLoadItemDependenciesBegin(void* item);
    void OnLoadItemDependenciesEnd();
    void OnCollectAmmoRefs(void* settings, game::Array<game::StreamingHandle*>* out);
}
