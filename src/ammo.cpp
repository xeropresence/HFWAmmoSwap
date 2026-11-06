#include "ammo.h"

#include "log.h"

#include <algorithm>

using namespace game;

namespace
{
    using GetComponent_t = void* (*)(void* container, void* rtti);
    using GetAmmoSlotsSettings_t = void* (*)(void* itemResource);
    using GetUpgradableWeaponSettings_t = void* (*)(void* itemResource);
    using FindSupportedAmmoItem_t = void* (*)(void* settings, const GGUUID* ammo);
    using GetAmmoSlotsRuntimeData_t = void* (*)(void* item);
    using InventoryFindItem_t = void* (*)(void* inventory, const GGUUID* entity);
    using InventoryGetItems_t = void (*)(void* inventory, Array<void*>* out);
    using GetLoadedAmmoResource_t = void* (*)(void* component, const GGUUID* ammo);
    using BindSlotInventoryItem_t = bool (*)(void* component, int slot, void* ammoStack);
    using StreamingRefCopy_t = void (*)(StreamingHandle** dst, StreamingHandle* const* src);
    using Realloc_t = void* (*)(void* ptr, size_t size);
    using SelectAmmoSlot_t = bool (*)(void* component, int slot);
    using ReallocAmmoSlots_t = AmmoSlot* (*)(Array<AmmoSlot>* slots, AmmoSlot* oldData, int capacity);
    using ReserveGuids_t = GGUUID* (*)(Array<GGUUID>* array, int count);
    using GetUpgradeCount_t = int (*)(void* item, void* unused);
    using WeakPtrOp_t = void (*)(WeakPtr* ptr);

    GetComponent_t GetComponent;
    GetAmmoSlotsSettings_t GetAmmoSlotsSettings;
    GetUpgradableWeaponSettings_t GetUpgradableWeaponSettings;
    FindSupportedAmmoItem_t FindSupportedAmmoItem;
    GetAmmoSlotsRuntimeData_t GetAmmoSlotsRuntimeData;
    InventoryFindItem_t InventoryFindItem;
    InventoryGetItems_t InventoryGetItems;
    GetLoadedAmmoResource_t GetLoadedAmmoResource;
    BindSlotInventoryItem_t BindSlotInventoryItem;
    StreamingRefCopy_t StreamingRefCopy;
    Realloc_t Realloc;
    SelectAmmoSlot_t SelectAmmoSlot;
    ReallocAmmoSlots_t ReallocAmmoSlots;
    ReserveGuids_t ReserveGuids;
    GetUpgradeCount_t GetUpgradeCount;
    WeakPtrOp_t WeakPtrLink;
    WeakPtrOp_t WeakPtrUnlink;

    void* RTTI_Inventory;
    void* RTTI_AmmoSlotsComponent;
    void* RTTI_InventoryItemComponent;

    // Item whose dependencies are being collected on this thread (see OnLoadItemDependenciesBegin).
    thread_local void* t_loadingItem;

    template <class F>
    F Resolve(uintptr_t base, uintptr_t rva)
    {
        return reinterpret_cast<F>(base + rva);
    }

    // Ammo entity-resource UUID of an ammo InventoryItemResource.
    GGUUID AmmoUUID(void* ammoItemResource)
    {
        return HandleUUID(At<StreamingHandle*>(ammoItemResource, InventoryItemResource::EntityResourceRef));
    }

    void* AmmoSlotsComponentOf(void* item)
    {
        void* entity = At<void*>(item, InventoryItem::Entity);
        if (!entity)
            return nullptr;
        return GetComponent(static_cast<uint8_t*>(entity) + Entity::Components, RTTI_AmmoSlotsComponent);
    }

    void* AmmoSettingsOf(void* item)
    {
        void* resource = ammo::ItemResource(item);
        return resource ? GetAmmoSlotsSettings(resource) : nullptr;
    }

    void FreeArray(Array<void*>& array)
    {
        if (array.data)
            Realloc(array.data, 0);
        array = {};
    }
}

namespace ammo
{
    void Init(uintptr_t base)
    {
        GetComponent = Resolve<GetComponent_t>(base, rva::GetComponent);
        GetAmmoSlotsSettings = Resolve<GetAmmoSlotsSettings_t>(base, rva::GetAmmoSlotsSettings);
        GetUpgradableWeaponSettings = Resolve<GetUpgradableWeaponSettings_t>(base, rva::GetUpgradableWeaponSettings);
        FindSupportedAmmoItem = Resolve<FindSupportedAmmoItem_t>(base, rva::FindSupportedAmmoItem);
        GetAmmoSlotsRuntimeData = Resolve<GetAmmoSlotsRuntimeData_t>(base, rva::GetAmmoSlotsRuntimeData);
        InventoryFindItem = Resolve<InventoryFindItem_t>(base, rva::InventoryFindItem);
        InventoryGetItems = Resolve<InventoryGetItems_t>(base, rva::InventoryGetItems);
        GetLoadedAmmoResource = Resolve<GetLoadedAmmoResource_t>(base, rva::GetLoadedAmmoResource);
        BindSlotInventoryItem = Resolve<BindSlotInventoryItem_t>(base, rva::BindSlotInventoryItem);
        StreamingRefCopy = Resolve<StreamingRefCopy_t>(base, rva::StreamingRefCopy);
        Realloc = Resolve<Realloc_t>(base, rva::Realloc);
        SelectAmmoSlot = Resolve<SelectAmmoSlot_t>(base, rva::SelectAmmoSlot);
        ReallocAmmoSlots = Resolve<ReallocAmmoSlots_t>(base, rva::ReallocAmmoSlots);
        ReserveGuids = Resolve<ReserveGuids_t>(base, rva::ReserveGuids);
        GetUpgradeCount = Resolve<GetUpgradeCount_t>(base, rva::GetUpgradeCount);
        WeakPtrLink = Resolve<WeakPtrOp_t>(base, rva::WeakPtrLink);
        WeakPtrUnlink = Resolve<WeakPtrOp_t>(base, rva::WeakPtrUnlink);
        RTTI_Inventory = reinterpret_cast<void*>(base + rva::RTTI_Inventory);
        RTTI_AmmoSlotsComponent = reinterpret_cast<void*>(base + rva::RTTI_AmmoSlotsComponent);
        RTTI_InventoryItemComponent = reinterpret_cast<void*>(base + rva::RTTI_InventoryItemComponent);
    }

    void* ItemResource(void* item)
    {
        return item ? Deref(At<StreamingHandle*>(item, InventoryItem::ResourceRef)) : nullptr;
    }

    bool HasSwappableAmmo(void* itemResource)
    {
        void* settings = itemResource ? GetAmmoSlotsSettings(itemResource) : nullptr;
        return settings && At<Array<void*>>(settings, AmmoSlotsSettings::SupportedAmmos).count > 1;
    }

    void* ItemOfComponent(void* component)
    {
        void* entity = component ? At<void*>(component, EntityComponent::Entity) : nullptr;
        if (!entity)
            return nullptr;
        void* itemComponent = GetComponent(static_cast<uint8_t*>(entity) + Entity::Components, RTTI_InventoryItemComponent);
        return itemComponent ? At<void*>(itemComponent, InventoryItemComponent::Item) : nullptr;
    }

    void* OwnerInventory(void* item)
    {
        void* owner = item ? At<void*>(item, InventoryItem::Owner) : nullptr;
        return owner ? GetComponent(static_cast<uint8_t*>(owner) + Entity::Components, RTTI_Inventory) : nullptr;
    }

    GGUUID AmmoOfItemResource(void* ammoItemResource) { return ammoItemResource ? AmmoUUID(ammoItemResource) : GGUUID{}; }

    void* AmmoStack(void* weapon, const GGUUID& ammo)
    {
        void* inventory = OwnerInventory(weapon);
        return inventory ? InventoryFindItem(inventory, &ammo) : nullptr;
    }

    std::vector<GGUUID> DefaultAmmo(void* weapon)
    {
        std::vector<GGUUID> result;
        void* resource = ItemResource(weapon);
        if (void* settings = resource ? GetAmmoSlotsSettings(resource) : nullptr)
        {
            for (const GGUUID& id : At<Array<GGUUID>>(settings, AmmoSlotsSettings::InitialAmmoConfig))
                result.push_back(id);
        }
        // Upgrading to level N applies UpgradePath[N].
        const int level = UpgradeLevel(weapon);
        if (void* upgrades = resource ? GetUpgradableWeaponSettings(resource) : nullptr)
        {
            const auto& path = At<Array<void*>>(upgrades, UpgradableWeaponSettings::UpgradePath);
            for (int i = 0; i <= level && i < path.count; i++)
            {
                if (!path.data[i])
                    continue;
                for (StreamingHandle* ammo : At<Array<StreamingHandle*>>(path.data[i], WeaponUpgradeDefinition::AmmoUnlocks))
                    result.push_back(HandleUUID(ammo));
            }
        }
        return result;
    }

    int UpgradeLevel(void* weapon) { return GetUpgradeCount(weapon, nullptr) - 1; }

    int SlotCapacity(void* weapon)
    {
        return int(std::max(DefaultAmmo(weapon).size(), GetAssigned(weapon, nullptr).size()));
    }

    void* SupportedAmmoItem(void* item, const GGUUID& ammo)
    {
        void* settings = AmmoSettingsOf(item);
        return settings ? FindSupportedAmmoItem(settings, &ammo) : nullptr;
    }

    std::vector<GGUUID> GetAssigned(void* item, int* selectedSlot)
    {
        std::vector<GGUUID> result;
        int selected = 0;

        if (void* component = AmmoSlotsComponentOf(item))
        {
            for (const AmmoSlot& slot : At<Array<AmmoSlot>>(component, AmmoSlotsComponent::Slots))
                result.push_back(slot.ammoItemResource ? AmmoUUID(slot.ammoItemResource) : GGUUID{});
            selected = At<int>(component, AmmoSlotsComponent::SelectedSlot);
        }
        else if (void* runtime = GetAmmoSlotsRuntimeData(item))
        {
            for (const GGUUID& id : At<Array<GGUUID>>(runtime, AmmoSlotsRuntimeData::AssignedAmmos))
                result.push_back(id);
            selected = At<int>(runtime, AmmoSlotsRuntimeData::SelectedSlotIndex);
        }

        if (selectedSlot)
            *selectedSlot = selected;
        return result;
    }
}

namespace
{
    bool CanTake(void* item, const GGUUID& ammo, int replacedSlot)
    {
        void* settings = AmmoSettingsOf(item);
        if (!settings || !FindSupportedAmmoItem(settings, &ammo))
        {
            LOG("Ammo not supported by weapon");
            return false;
        }
        const std::vector<GGUUID> assigned = ammo::GetAssigned(item, nullptr);
        for (int i = 0; i < int(assigned.size()); i++)
        {
            if (i != replacedSlot && assigned[i] == ammo)
                return false;  // an ammo type may occupy only one slot per weapon
        }
        return true;
    }

    // Points a spawned weapon's slot at `ammo`, binds it to the player's stack and reloads the weapon
    // if it is the selected slot.
    bool FillSlot(void* item, void* component, int slotIndex, const GGUUID& ammo)
    {
        void* resource = At<void*>(component, EntityComponent::Resource);
        const SupportedAmmo* match = nullptr;
        for (const SupportedAmmo& entry : At<Array<SupportedAmmo>>(resource, AmmoSlotsComponentResource::SupportedAmmo))
        {
            if (entry.ammoItemResource && AmmoUUID(entry.ammoItemResource) == ammo)
            {
                match = &entry;
                break;
            }
        }
        void* ammoResource = match ? GetLoadedAmmoResource(component, &ammo) : nullptr;
        if (!ammoResource)
        {
            LOG("Ammo resource not loaded for weapon (supported=%d)", match != nullptr);
            return false;
        }

        AmmoSlot& slot = At<Array<AmmoSlot>>(component, AmmoSlotsComponent::Slots).data[slotIndex];
        slot.weaponMode = match->weaponMode;
        slot.ammoItemResource = match->ammoItemResource;
        slot.ammoResource = ammoResource;
        void* inventory = ammo::OwnerInventory(item);
        BindSlotInventoryItem(component, slotIndex, inventory ? InventoryFindItem(inventory, &ammo) : nullptr);
        return true;
    }

    // Selected slot after slot `removed` is gone and `count` slots remain.
    int SelectionAfterRemoval(int selected, int removed, int count)
    {
        if (selected > removed)
            return selected - 1;
        return std::min(selected, count - 1);
    }
}

namespace ammo
{
    bool SetSlotAmmo(void* item, int slotIndex, const GGUUID& ammo)
    {
        if (slotIndex < 0 || slotIndex >= int(GetAssigned(item, nullptr).size()) || !CanTake(item, ammo, slotIndex))
            return false;

        if (void* component = AmmoSlotsComponentOf(item))
            return FillSlot(item, component, slotIndex, ammo);

        // Weapon isn't spawned: its ammo lives in the runtime data the next spawn restores from.
        void* runtime = GetAmmoSlotsRuntimeData(item);
        if (!runtime)
            return false;
        At<Array<GGUUID>>(runtime, AmmoSlotsRuntimeData::AssignedAmmos).data[slotIndex] = ammo;
        return true;
    }

    bool AddSlot(void* item, const GGUUID& ammo)
    {
        const int count = int(GetAssigned(item, nullptr).size());
        if (count >= SlotCapacity(item) || !CanTake(item, ammo, -1))
            return false;

        void* component = AmmoSlotsComponentOf(item);
        if (!component)
        {
            void* runtime = GetAmmoSlotsRuntimeData(item);
            if (!runtime)
                return false;
            auto& assigned = At<Array<GGUUID>>(runtime, AmmoSlotsRuntimeData::AssignedAmmos);
            GGUUID* data = ReserveGuids(&assigned, assigned.count + 1);
            if (!data)
                return false;
            data[assigned.count++] = ammo;
            return true;
        }

        // Like an upgrade's ammo unlock: grow the slot array (moving the slots' weak pointers),
        // append an empty slot, then fill it.
        auto& slots = At<Array<AmmoSlot>>(component, AmmoSlotsComponent::Slots);
        if (slots.count + 1 > slots.capacity)
        {
            AmmoSlot* data = ReallocAmmoSlots(&slots, slots.data, slots.count + 1 + (slots.count + 1) / 8 + 2);
            if (!data)
                return false;
            slots.data = data;
        }
        AmmoSlot& slot = slots.data[slots.count];
        slot = AmmoSlot{};
        slot.stack.self = &slot.stack;
        slots.count++;
        if (!FillSlot(item, component, slots.count - 1, ammo))
        {
            slots.count--;
            return false;
        }
        return true;
    }

    bool RemoveSlot(void* item, int slotIndex)
    {
        const int count = int(GetAssigned(item, nullptr).size());
        if (count <= 1 || slotIndex < 0 || slotIndex >= count)
            return false;  // a weapon keeps at least one ammo type

        void* component = AmmoSlotsComponentOf(item);
        if (!component)
        {
            void* runtime = GetAmmoSlotsRuntimeData(item);
            if (!runtime)
                return false;
            auto& assigned = At<Array<GGUUID>>(runtime, AmmoSlotsRuntimeData::AssignedAmmos);
            std::copy(assigned.data + slotIndex + 1, assigned.data + assigned.count, assigned.data + slotIndex);
            assigned.count--;
            int& selected = At<int>(runtime, AmmoSlotsRuntimeData::SelectedSlotIndex);
            selected = SelectionAfterRemoval(selected, slotIndex, assigned.count);
            return true;
        }

        // Shift the later slots down in place. Their weak pointers are linked into the ammo stacks'
        // lists by address, so each is unlinked and relinked at its new place.
        auto& slots = At<Array<AmmoSlot>>(component, AmmoSlotsComponent::Slots);
        WeakPtrUnlink(&slots.data[slotIndex].stack);
        for (int i = slotIndex + 1; i < slots.count; i++)
        {
            AmmoSlot& from = slots.data[i];
            AmmoSlot& to = slots.data[i - 1];
            void* stack = from.stack.target;
            WeakPtrUnlink(&from.stack);
            to = from;
            to.stack = WeakPtr{stack, nullptr, nullptr, &to.stack};
            if (stack)
                WeakPtrLink(&to.stack);
        }
        slots.data[slots.count - 1] = AmmoSlot{};
        slots.count--;

        int& selected = At<int>(component, AmmoSlotsComponent::SelectedSlot);
        const bool selectedRemoved = selected == slotIndex;
        selected = SelectionAfterRemoval(selected, slotIndex, slots.count);
        if (selectedRemoved)
            SelectAmmoSlot(component, selected);  // reload the weapon with the ammo now selected
        return true;
    }

    bool RestoreDefaults(void* item)
    {
        const std::vector<GGUUID> defaults = DefaultAmmo(item);
        if (defaults.empty())
            return false;
        // Down to one slot, which takes the first default (no duplicate can be in the way then),
        // then append the rest in order.
        for (int count = int(GetAssigned(item, nullptr).size()); count > 1; count--)
        {
            if (!RemoveSlot(item, count - 1))
                return false;
        }
        if (!SetSlotAmmo(item, 0, defaults[0]))
            return false;
        for (size_t i = 1; i < defaults.size(); i++)
        {
            if (!AddSlot(item, defaults[i]))
                return false;
        }
        return true;
    }

    std::vector<GGUUID> Candidates(void* item)
    {
        std::vector<GGUUID> result;
        void* settings = AmmoSettingsOf(item);
        void* inventory = OwnerInventory(item);
        if (!settings || !inventory || !HasSwappableAmmo(ItemResource(item)))
            return result;

        // Ammo a weapon of the owner holds or comes with. Filtering by this weapon's SupportedAmmos
        // below keeps it to ammo this weapon type can fire.
        std::vector<GGUUID> owned;
        Array<void*> items = {};
        InventoryGetItems(inventory, &items);
        for (void* other : items)
        {
            void* otherResource = ItemResource(other);
            if (!otherResource || !GetAmmoSlotsSettings(otherResource))
                continue;
            for (const GGUUID& id : GetAssigned(other, nullptr))
                owned.push_back(id);
            for (const GGUUID& id : DefaultAmmo(other))
                owned.push_back(id);
        }
        FreeArray(items);

        for (void* ammoItem : At<Array<void*>>(settings, AmmoSlotsSettings::SupportedAmmos))
        {
            if (!ammoItem)
                continue;
            const GGUUID id = AmmoUUID(ammoItem);
            if (std::find(owned.begin(), owned.end(), id) != owned.end() && std::find(result.begin(), result.end(), id) == result.end())
                result.push_back(id);
        }
        return result;
    }

    void OnLoadItemDependenciesBegin(void* item) { t_loadingItem = item; }

    void OnLoadItemDependenciesEnd() { t_loadingItem = nullptr; }

    void OnCollectAmmoRefs(void* settings, Array<StreamingHandle*>* out)
    {
        // Weapons keep every ammo type they support loaded so any of it can be swapped in at runtime
        // (vanilla only loads the initial ammo and upgrade unlocks).
        if (!t_loadingItem || !HasSwappableAmmo(ItemResource(t_loadingItem)))
            return;

        const auto& supported = At<Array<void*>>(settings, AmmoSlotsSettings::SupportedAmmos);
        for (void* ammoItem : supported)
        {
            if (!ammoItem)
                continue;
            StreamingHandle* const* source = &At<StreamingHandle*>(ammoItem, InventoryItemResource::EntityResourceRef);
            const GGUUID id = HandleUUID(*source);
            if (id.IsNull())
                continue;
            const bool present = std::any_of(out->begin(), out->end(), [&](StreamingHandle* h) { return HandleUUID(h) == id; });
            if (present)
                continue;

            if (out->count == out->capacity)
            {
                const int capacity = out->count + out->count / 8 + 4;
                auto* data = static_cast<StreamingHandle**>(Realloc(out->data, sizeof(StreamingHandle*) * capacity));
                if (!data)
                {
                    LOG("Out of memory preloading weapon ammo");
                    break;
                }
                out->data = data;
                out->capacity = capacity;
            }
            out->data[out->count] = nullptr;
            StreamingRefCopy(&out->data[out->count], source);
            out->count++;
        }
    }
}
