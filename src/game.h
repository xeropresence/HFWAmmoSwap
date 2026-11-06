#pragma once

// Horizon Forbidden West engine structures and functions. Offsets come from the game's RTTI
// (odradek types.json) and from the decompiled callers that use them; they were taken from
// 1.5.80.0. Function and data addresses are found at startup (signatures.cpp).

#include <cstdint>
#include <cstring>

namespace game
{
    struct GGUUID
    {
        uint64_t lo = 0;
        uint64_t hi = 0;

        bool IsNull() const { return lo == 0 && hi == 0; }
        bool operator==(const GGUUID& o) const { return lo == o.lo && hi == o.hi; }
    };
    static_assert(sizeof(GGUUID) == 16);

    constexpr uint8_t HexNibble(char c)
    {
        return c >= '0' && c <= '9' ? uint8_t(c - '0') : c >= 'a' && c <= 'f' ? uint8_t(c - 'a' + 10) : uint8_t(c - 'A' + 10);
    }

    // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" in the usual GUID text form -> in-memory GGUUID bytes.
    constexpr GGUUID ParseGuid(const char* s)
    {
        uint8_t b[16] = {};
        int n = 0;
        for (int i = 0; s[i] && n < 32; i++)
        {
            if (s[i] == '-')
                continue;
            const uint8_t v = HexNibble(s[i]);
            b[n / 2] = uint8_t(n % 2 ? (b[n / 2] | v) : (v << 4));
            n++;
        }
        // Data1/Data2/Data3 are stored little-endian.
        const uint8_t m[16] = {b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]};
        GGUUID g;
        for (int i = 0; i < 8; i++)
            g.lo |= uint64_t(m[i]) << (8 * i);
        for (int i = 0; i < 8; i++)
            g.hi |= uint64_t(m[8 + i]) << (8 * i);
        return g;
    }

    template <class T>
    struct Array
    {
        int32_t count;
        int32_t capacity;
        T* data;

        T* begin() const { return data; }
        T* end() const { return data + count; }
    };
    static_assert(sizeof(Array<int>) == 16);

    // Target of a StreamingRef: +16 UUID of the referenced object, +32 the object once loaded.
    struct StreamingEntry
    {
        uint8_t pad0[16];
        GGUUID uuid;
        void* object;
    };

    // A StreamingRef<T> field stores a pointer to one of these.
    struct StreamingHandle
    {
        StreamingEntry* entry;
        uint64_t flags;  // bit 59 set => object is loaded
    };

    inline bool IsLoaded(const StreamingHandle* h)
    {
        return h && h->entry && ((h->flags >> 52) & 0x80) != 0;
    }

    inline void* Deref(const StreamingHandle* h) { return IsLoaded(h) ? h->entry->object : nullptr; }

    // UUID of the object a handle points at, or the null UUID.
    inline GGUUID HandleUUID(const StreamingHandle* h) { return (h && h->entry) ? h->entry->uuid : GGUUID{}; }

    template <class T>
    T& At(void* base, size_t offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
    }

    // ---- RTTIRefObject ---------------------------------------------------------------------------
    inline GGUUID& ObjectUUID(void* refObject) { return At<GGUUID>(refObject, 8); }

    // Decima RTTI descriptor of an object's type (vtable slot 0).
    inline void* RttiOf(void* object) { return (*static_cast<void* (***)(void*)>(object))[0](object); }

    // First half of the UUIDs of objects the mod makes ("AmmoSwap").
    constexpr uint64_t kModUuidLo = 0x706177536F6D6D41ull;

    // ---- InventoryItemResource ---------------------------------------------------------------------
    namespace InventoryItemResource
    {
        constexpr size_t EntityResourceRef = 32;  // StreamingHandle*
        constexpr size_t Settings = 56;           // Array<InventoryItemSettings*>
        constexpr size_t ItemName = 72;           // LocalizedTextResource*
        constexpr size_t Category = 210;          // EInventoryCategory (uint8)
        constexpr size_t Tags = 216;              // Array<InventoryItemTag*>
    }

    enum class EInventoryCategory : int8_t
    {
        Weapons = 0,
        Ammo = 3,
        Modifications = 4,
    };

    // ---- InventoryItem (runtime instance owned by an Inventory) ------------------------------------
    namespace InventoryItem
    {
        constexpr size_t WeakTarget = 40;     // WeakPtrRTTITarget base (what WeakPtr slots point at)
        constexpr size_t ResourceRef = 72;    // StreamingHandle* -> InventoryItemResource
        constexpr size_t Owner = 88;          // Entity* (player)
        constexpr size_t RuntimeData = 120;   // Array<InventoryItemRuntimeData*>
        constexpr size_t Entity = 136;        // Entity* while the item is spawned (wielded/stowed weapon)
        constexpr size_t LoadedRefs = 160;    // Array<StreamingHandle*> dependencies loaded with the item
    }

    // ---- AmmoSlotsSettings (InventoryItemSettings of weapons) ---------------------------------------
    namespace AmmoSlotsSettings
    {
        constexpr size_t SupportedAmmos = 40;     // Array<InventoryItemResource*>
        constexpr size_t InitialAmmoConfig = 56;  // Array<GGUUID> of ammo entity resources
    }

    // ---- UpgradableWeaponSettings (InventoryItemSettings of upgradable weapons) -------------------
    // Upgrading to level N applies UpgradePath[N]; its AmmoUnlocks each append an ammo slot.
    namespace UpgradableWeaponSettings
    {
        constexpr size_t UpgradePath = 48;  // Array<WeaponUpgradeDefinition*>, index = upgrade level
    }
    namespace WeaponUpgradeDefinition
    {
        constexpr size_t NewBonusResources = 48;  // Array<CombatStatsBonusResource*> the weapon has at this level
        constexpr size_t AmmoUnlocks = 120;       // Array<StreamingHandle*> of ammo entity resources
    }

    // ---- AmmoSlotsRuntimeData (saved per weapon item while it isn't spawned) ------------------------
    namespace AmmoSlotsRuntimeData
    {
        constexpr size_t SelectedSlotIndex = 32;  // int
        constexpr size_t AssignedAmmos = 40;      // Array<GGUUID> of ammo entity resources
    }

    // ---- Entity / components ----------------------------------------------------------------------
    namespace Entity
    {
        constexpr size_t Owner = 128;       // Entity* that gets an equipment entity's bonuses (the wielder)
        constexpr size_t Components = 160;  // container searched by GetComponent
    }

    namespace EntityComponent
    {
        constexpr size_t Resource = 48;
        constexpr size_t Entity = 72;
    }

    namespace InventoryItemComponent
    {
        constexpr size_t Item = 80;  // InventoryItem*
    }

    struct SupportedAmmo
    {
        void* ammoResource;         // AmmoResource* (resolved at runtime)
        void* ammoItemResource;     // InventoryItemResource*
        GGUUID weaponMode;          // UUIDRef<RangedWeaponModeResource>
    };
    static_assert(sizeof(SupportedAmmo) == 32);

    namespace AmmoSlotsComponentResource
    {
        constexpr size_t SupportedAmmo = 32;  // Array<game::SupportedAmmo>
    }

    // Engine weak pointer; the target clears `target` when it dies.
    struct WeakPtr
    {
        void* target;
        WeakPtr* prev;
        WeakPtr* next;
        WeakPtr* self;
    };

    struct AmmoSlot
    {
        WeakPtr stack;           // the player's ammo stack (InventoryItem + InventoryItem::WeakTarget)
        GGUUID weaponMode;
        void* ammoItemResource;  // InventoryItemResource*
        void* ammoResource;      // AmmoResource* (loaded entity resource)
    };
    static_assert(sizeof(AmmoSlot) == 64);

    namespace AmmoSlotsComponent
    {
        constexpr size_t SelectedSlot = 80;  // int
        constexpr size_t Slots = 88;         // Array<AmmoSlot>
    }

    // ---- Damage bonuses ---------------------------------------------------------------------------
    // A weapon raises the damage and build-up of damage types through CombatStatsDamageDealtBonus-
    // Resources: a definition (which damage types, under which conditions) and a value. Those of its
    // EquipmentStatsSettings and of its upgrade level are added to the wielder's CombatStatsComponent
    // while the weapon is wielded. Multipliers of one stack group add up (x2.29 and x1.87 make x3.16).
    namespace CombatStatsBonus
    {
        constexpr size_t Definition = 32;      // CombatStatsDamageDealtDefinition*
        constexpr size_t Value = 40;           // CombatStatsDamageValue*
        constexpr size_t AddToCollection = 4;  // vtable slot: void(Bonus*, BonusCollection*)
    }
    namespace CombatStatsDefinition
    {
        constexpr size_t ConditionOnAffectedEntity = 40;   // ConditionResource*
        constexpr size_t ConditionOnOwner = 48;            // ConditionResource*
        constexpr size_t CombatEventConditionFilter = 56;  // CombatEventConditionFilter*
        constexpr size_t DamageTypeFilter = 64;            // DamageTypeFilter*
    }
    namespace CombatStatsDamageValue
    {
        constexpr size_t StackGroup = 32;          // CombatStatsBonusStackGroup*
        constexpr size_t StackGroupModifier = 40;  // CombatStatsBonusStackGroupModifier*
        // float[5]: DamageAddition, DamageMultiplier, ImpactSeverityAddition, ImpactSeverityMultiplier,
        // CoreDamageMultiplier
        constexpr size_t Amounts = 48;
        constexpr int AmountCount = 5;
    }
    // The game works out the mask of matching damage types from DamageTypes and Group on first use.
    namespace DamageTypeFilter
    {
        constexpr size_t DamageTypes = 32;    // Array<DamageTypeResource*>
        constexpr size_t Group = 48;          // DamageTypeGroup*
        constexpr size_t FilterType = 56;     // uint8 EIncludeExcludeFilterType
        constexpr uint8_t Include = 0;
    }
    namespace EquipmentStatsSettings
    {
        constexpr size_t Bonuses = 32;  // Array<CombatStatsBonusResource*>, the weapon's base stats
    }
    // InventoryItemSettings of ammo items; DisplayStats are the damage lines of the ammo's stats.
    namespace AmmoSettings
    {
        constexpr size_t DisplayStats = 40;  // Array<CombatStatsDamageDisplayResource*>
    }
    namespace CombatStatsDamageDisplayResource
    {
        constexpr size_t BaseDamageSettings = 32;  // DamageSettings*
    }
    namespace DamageSettings
    {
        constexpr size_t DamageType = 32;  // DamageTypeResource*
    }
    // Components that add a weapon's bonuses to the wielder (`Activation` is passed along with them).
    namespace EquipmentStatsComponent
    {
        constexpr size_t Activation = 80;  // uint16
    }
    namespace ItemUpgradeComponent
    {
        constexpr size_t Level = 84;        // int, -1 before the first upgrade
        constexpr size_t Activation = 120;  // uint16
    }

    // ---- Game addresses -------------------------------------------------------------------------
    // RVAs relative to the image base, filled in by signatures::Resolve before anything uses them.
    namespace rva
    {
        inline uintptr_t GetComponent;                // void*(void* container, RTTI*)
        inline uintptr_t GetAmmoSlotsSettings;        // void*(InventoryItemResource*)
        inline uintptr_t FindSupportedAmmoItem;       // InventoryItemResource*(AmmoSlotsSettings*, GGUUID*)
        inline uintptr_t GetAmmoSlotsRuntimeData;     // void*(InventoryItem*), null while spawned
        inline uintptr_t InventoryFindItem;           // InventoryItem*(Inventory*, GGUUID* entity uuid)
        inline uintptr_t InventoryGetItems;           // void(Inventory*, Array<InventoryItem*>*)
        inline uintptr_t GetLoadedAmmoResource;       // AmmoResource*(AmmoSlotsComponent*, GGUUID*)
        inline uintptr_t GetUpgradableWeaponSettings; // void*(InventoryItemResource*)
        inline uintptr_t SelectAmmoSlot;              // bool(AmmoSlotsComponent*, int): sends MsgSelectAmmoSlot for that slot
        inline uintptr_t ReallocAmmoSlots;            // AmmoSlot*(Array<AmmoSlot>*, AmmoSlot* oldData, int capacity): moves slots, frees old, sets capacity
        inline uintptr_t ReserveGuids;                // GGUUID*(Array<GGUUID>*, int count): ensures capacity, returns data
        inline uintptr_t GetUpgradeCount;             // int(InventoryItem*, void* unused): upgrade level + 1, -1 if not upgradable
        inline uintptr_t BindSlotInventoryItem;       // bool(AmmoSlotsComponent*, int, InventoryItem*)
        inline uintptr_t StreamingRefCopy;            // void(StreamingHandle** dst, StreamingHandle** src)
        inline uintptr_t Realloc;                     // void*(void*, size_t); size 0 frees
        inline uintptr_t Alloc;                       // void*(size_t)
        inline uintptr_t GetAmmoSettings;             // AmmoSettings*(InventoryItemResource* of an ammo item)
        inline uintptr_t DamageTypeFilter_Matches;    // bool(DamageTypeFilter*, DamageTypeResource*)
        inline uintptr_t CombatStats_AddBonus;        // void(CombatStatsComponent*, Bonus*, GGUUID* key, uint16 activation)
        inline uintptr_t CombatStats_RemoveBonus;     // void(CombatStatsComponent*, Bonus*, GGUUID* key, uint16 activation)

        // Decima String (refcounted, pointer to chars)
        inline uintptr_t StringCtor;                  // void(String*, const char*, int length)
        inline uintptr_t StringRelease;               // void(String*)
        inline uintptr_t EmptyString;                 // chars of the shared empty String

        // Menu data tree (InventoryMenuController + 144); nodes are addressed by name + index
        inline uintptr_t NodeChild;                   // Node*(Tree*, Node* parent, String* name, int index), creates
        inline uintptr_t NodeCount;                   // int(Node* parent, String* name)
        inline uintptr_t NodeRemove;                  // void(Tree*, Node* parent, String* name, int index)
        inline uintptr_t NodeSetBool;                 // void(Node*, bool)
        inline uintptr_t NodeSetInt;                  // void(Node*, int)
        inline uintptr_t NodeSetObject;               // void(Node*, void*), raw pointer
        inline uintptr_t NodeSetString;               // bool(Tree*, Node*, String*)
        inline uintptr_t NodeSetText;                 // bool(Tree*, Node*, LocalizedTextResource*)
        inline uintptr_t NodeMarkDirty;               // void(DirtySet* (tree + 104), uint32 crc32c(node), Node**)
        inline uintptr_t NodeItemInfo;                // void(Tree*, Node*, InventoryItemResource*, bool): name, icon, description...
        inline uintptr_t NodeItemRarity;              // void(Tree*, Node*, uint8 rarity)
        inline uintptr_t NodeItemOwned;               // void(Tree*, Node*, Inventory*, InventoryItemResource*, int, int)
        inline uintptr_t ItemIcon;                    // UITexture*(InventoryItemResource*)

        // Hook targets
        inline uintptr_t InventoryItem_LoadDependencies; // void(InventoryItem*, Callback* onLoaded)
        inline uintptr_t AmmoSlotsSettings_CollectRefs;  // void(AmmoSlotsSettings*, Array<StreamingHandle*>*)
        inline uintptr_t LocalizedText_GetString;        // String*(LocalizedTextResource*, String* out), vtable slot 4
        inline uintptr_t MenuInputBinding_Matches;       // bool(MenuInputEventBinding*, Event*), vtable slot 4
        inline uintptr_t MenuGraphAction_Execute;        // void(MenuActionGraphProgram*, Menu*, Sender*, Event*), vtable slot 4
        inline uintptr_t InvMenu_RefreshItemSelected;    // void(InventoryMenuController*)
        inline uintptr_t InvMenu_PopulateModsExport;     // thunk called by the menu's "PopulateModsForEditTool" action
        inline uintptr_t InvMenu_PopulateMods;           // void(InventoryMenuController*)
        inline uintptr_t InvMenu_SelectMod;              // void(InventoryMenuController*, ModificationSocketResource*)
        inline uintptr_t InvMenu_SelectFirstModSocket;   // void(InventoryMenuController*)
        inline uintptr_t InvMenu_PreviewMod;             // void(InventoryMenuController*, InventoryItem*)
        inline uintptr_t InvMenu_PopulateWeaponsExport;  // thunk called by the menu's "PopulateWeapons" action
        inline uintptr_t InvMenu_PopulateWeapons;        // void(InventoryMenuController*), only used to find its thunk
        inline uintptr_t InvMenu_EquipSelectedMod;       // void(InventoryMenuController*, bool* result)
        inline uintptr_t InvMenu_UnequipSelectedMod;     // void(InventoryMenuController*)
        inline uintptr_t InvMenu_Destroy;                // void*(InventoryMenuController*, int flags), vtable slot 1
        // Item details "ammo" list (ammo types with damage): written from the weapon's default ammo
        inline uintptr_t ItemPanel_WriteAmmo;       // void(Tree*, Node*, InventoryItemResource*, InventoryItem* (may be null), UpgradableWeaponSettings*, int level, int previewLevel, int maxLevel, 3 more)
        inline uintptr_t ItemPanel_WriteAmmoEntry;  // bool(Tree*, Node*, int* entryIndex, GGUUID* ammo, AmmoSlotsSettings*, 5 more)
        // Menu text elements
        inline uintptr_t TextTag_BindText;    // void(TextTag*, MenuDataBinding*, MemberFn* setter): evaluates static bindings now, subscribes dynamic ones
        inline uintptr_t TextTag_SetText;     // void(TextTag*, const String&), the setter TextTag passes to BindText
        inline uintptr_t WeakPtrLink;         // void(WeakPtr*) with target set
        inline uintptr_t WeakPtrUnlink;       // void(WeakPtr*), clears it
        inline uintptr_t VT_MenuLocalizedTextBinding;
        inline uintptr_t InventoryUiSettingsPtr;  // data: InventoryUiSettings*
        inline uintptr_t Tag_OnEvent;             // void(Tag*, TagEvent*), base handler every tag runs
        inline uintptr_t Tag_AddEventHandler;     // void(Tag::EventHandlers set*, MenuEventBinding** ref, Tag*)
        // Weapon damage bonuses
        inline uintptr_t EquipmentStats_Apply;           // void(EquipmentStatsComponent*): adds its bonuses to the wielder
        inline uintptr_t EquipmentStats_Remove;          // void(EquipmentStatsComponent*)
        inline uintptr_t ItemUpgrade_Apply;              // void(ItemUpgradeComponent*): adds the bonuses of its upgrade level
        inline uintptr_t ItemUpgrade_Remove;             // void(ItemUpgradeComponent*)
        inline uintptr_t CombatStats_PrepareForRemoval;  // void(CombatStatsComponent*, Msg*)
        // bool(BonusCollection*, InventoryItem*, int level, InventoryItem* addedCoil, InventoryItem* removedCoil):
        // bonuses of a weapon item at an upgrade level with its coils, as the item details and the HUD
        // list them to work out ammo damage
        inline uintptr_t CollectItemBonuses;

        // RTTI descriptors (type id/range at +0 are filled in by the engine at startup)
        inline uintptr_t RTTI_Inventory;
        inline uintptr_t RTTI_AmmoSlotsComponent;
        inline uintptr_t RTTI_InventoryItemComponent;
        inline uintptr_t RTTI_ModificationSocketResource;
        inline uintptr_t RTTI_InventoryItem;
        inline uintptr_t RTTI_MenuInputEventBinding;
        inline uintptr_t RTTI_MenuLocalizedTextBinding;
        inline uintptr_t RTTI_LocalizedTextResource;
        inline uintptr_t RTTI_CombatStatsComponent;
        inline uintptr_t RTTI_CombatStatsDamageDealtBonusResource;
        inline uintptr_t RTTI_CombatStatsDamageDealtDefinition;
        inline uintptr_t RTTI_CombatStatsDamageValue;
        inline uintptr_t RTTI_DamageTypeFilter;
        inline uintptr_t RTTI_EquipmentStatsSettings;
    }

    // ---- RTTI descriptor (compound) ---------------------------------------------------------------
    namespace RTTI
    {
        constexpr size_t Size = 0x10;         // uint32 instance size
        constexpr size_t Constructor = 0x18;  // void(RTTI*, void* object), placement construction
    }

    // ---- RTTIRefObject ------------------------------------------------------------------------------
    namespace RefObject
    {
        constexpr size_t RefCount = 24;  // int32, low 22 bits are the count
    }

    // ---- LocalizedTextResource ----------------------------------------------------------------------
    namespace LocalizedTextResource
    {
        constexpr size_t Text = 32;    // const char*, UTF-8 text of the current language
        constexpr size_t Length = 40;  // uint16
    }

    // ---- Menu objects -------------------------------------------------------------------------------
    namespace MenuEventBinding
    {
        constexpr size_t Actions = 56;  // Array<MenuActionResource*>
    }

    namespace MenuAction
    {
        constexpr size_t ExecuteSlot = 4;  // vtable slot: void(Action*, Menu*, Sender*, Event*)
    }

    namespace TagEvent
    {
        constexpr size_t Type = 16;  // int; 1 = the tag is being set up from its resource
    }

    namespace MenuElementResource
    {
        constexpr size_t EventHandlers = 416;  // Array<MenuEventBinding*>
    }

    namespace MenuInputEventBinding
    {
        constexpr size_t Actions = 56;          // Array<MenuActionResource*>
        constexpr size_t DisabledActions = 72;  // Array<MenuActionResource*>
        constexpr size_t Enabled = 88;          // MenuDataBinding*
        constexpr size_t Function = 96;         // EMenuInputFunction
        constexpr size_t ButtonTrigger = 100;   // EMenuButtonTrigger (0 = on press)
        constexpr size_t Text = 112;            // MenuDataBinding*
        constexpr size_t Visible = 120;         // MenuDataBinding*
    }

    // Tag: runtime element built from a MenuElementResource.
    namespace Tag
    {
        constexpr size_t WeakTarget = 56;      // WeakPtrRTTITarget base
        constexpr size_t Resource = 80;        // MenuElementResource*
        constexpr size_t EventHandlers = 192;  // the resource's bindings, set up on TagEvent type 1
    }

    // MenuLocalizedTextBinding: a LocalizedTextResource with optional parameters. Without parameters
    // the binding is static: text elements evaluate it once, when the menu is built.
    namespace MenuLocalizedTextBinding
    {
        constexpr size_t LocalizedText = 32;    // LocalizedTextResource*
        constexpr size_t ParameterCount = 40;  // int, Array<MenuLocalizedTextParameter>
    }

    // TextTag: runtime text element built from a MenuTextResource.
    namespace TextTag
    {
        constexpr size_t WeakTarget = 56;  // WeakPtrRTTITarget base
        constexpr size_t Resource = 80;    // MenuTextResource*
    }
    namespace MenuTextResource
    {
        constexpr size_t Text = 464;  // MenuDataBinding*
    }

    namespace InvMenuResource
    {
        constexpr size_t EquipModText = 192;  // LocalizedTextResource* "Equip Coil"
    }

    // Menu data tree node: a variant value (int32 / object pointer / ... at Value) and its type.
    namespace MenuNode
    {
        constexpr size_t Value = 32;
    }

    namespace DamageTypeStatDescription
    {
        constexpr size_t Name = 32;             // LocalizedTextResource*
        constexpr size_t Icon = 56;             // UITexture*
        constexpr size_t ImageMapEntryID = 72;  // String, usable as <icon name=...> in texts
    }

    // Inventory UI settings (global pointer): +248 -> stat display settings, whose +80 is the
    // Array<DamageStatEntry> the item details ammo list walks.
    struct DamageStatEntry
    {
        void* stat;
        uint64_t mode;
        void* description;  // DamageTypeStatDescription*
    };
    static_assert(sizeof(DamageStatEntry) == 24);
    namespace InventoryUiSettings
    {
        constexpr size_t StatDisplay = 248;
    }
    namespace StatDisplaySettings
    {
        constexpr size_t Entries = 80;  // Array<DamageStatEntry>
    }

    // ---- InventoryMenuController (runtime instance) ------------------------------------------------
    namespace InvMenu
    {
        constexpr size_t Resource = 80;         // InventoryMenuControllerResource*
        constexpr size_t Tree = 144;            // menu data tree; its root node is at Tree + 8
        constexpr size_t DirtySet = 248;        // Tree + 104
        constexpr size_t Category = 289;        // int8 EInventoryCategory of the shown tab
        constexpr size_t SelectedItem = 312;    // InventoryItem* shown in the detail panel / being modded
        constexpr size_t SelectedSocket = 320;  // ModificationSocketResource* chosen in the edit-coils view
        constexpr size_t PreviewedMod = 328;    // InventoryItem* highlighted in the coil list
        constexpr size_t Inventory = 392;       // Inventory* + 32
    }

    namespace ModificationSocketResource
    {
        constexpr size_t IsDefaultActivated = 49;  // bool
    }

    namespace InventoryItemResourceUI
    {
        constexpr size_t ItemDescription = 88;  // LocalizedTextResource*
        constexpr size_t Rarity = 209;          // EInventoryItemRarity (uint8)
    }
}
