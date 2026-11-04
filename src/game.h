#pragma once

// Horizon Forbidden West 1.5.80.0 (PE timestamp 0x667B1777) engine structures and functions.
// Offsets come from the game's RTTI (odradek types.json) and from the decompiled callers that
// use them; function addresses are RVAs relative to the image base.

#include <cstdint>
#include <cstring>

namespace game
{
    constexpr uint32_t kExpectedTimestamp = 0x667B1777;

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
        constexpr size_t AmmoUnlocks = 120;  // Array<StreamingHandle*> of ammo entity resources
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

    // ---- Function RVAs --------------------------------------------------------------------------
    namespace rva
    {
        constexpr uintptr_t GetComponent = 0x08FA230;               // void*(void* container, RTTI*)
        constexpr uintptr_t GetAmmoSlotsSettings = 0x129EFA0;       // void*(InventoryItemResource*)
        constexpr uintptr_t FindSupportedAmmoItem = 0x1233C50;      // InventoryItemResource*(AmmoSlotsSettings*, GGUUID*)
        constexpr uintptr_t GetAmmoSlotsRuntimeData = 0x129F570;    // void*(InventoryItem*), null while spawned
        constexpr uintptr_t InventoryFindItem = 0x1206550;          // InventoryItem*(Inventory*, GGUUID* entity uuid)
        constexpr uintptr_t InventoryGetItems = 0x12064A0;          // void(Inventory*, Array<InventoryItem*>*)
        constexpr uintptr_t GetLoadedAmmoResource = 0x129B690;      // AmmoResource*(AmmoSlotsComponent*, GGUUID*)
        constexpr uintptr_t GetUpgradableWeaponSettings = 0x159C820; // void*(InventoryItemResource*)
        constexpr uintptr_t SelectAmmoSlot = 0x129C770;             // bool(AmmoSlotsComponent*, int): sends MsgSelectAmmoSlot for that slot
        constexpr uintptr_t ReallocAmmoSlots = 0x12A1160;           // AmmoSlot*(Array<AmmoSlot>*, AmmoSlot* oldData, int capacity): moves slots, frees old, sets capacity
        constexpr uintptr_t ReserveGuids = 0x00E7E60;               // GGUUID*(Array<GGUUID>*, int count): ensures capacity, returns data
        constexpr uintptr_t GetUpgradeCount = 0x123D560;            // int(InventoryItem*, void* unused): upgrade level + 1, -1 if not upgradable
        constexpr uintptr_t BindSlotInventoryItem = 0x129C840;      // bool(AmmoSlotsComponent*, int, InventoryItem*)
        constexpr uintptr_t StreamingRefCopy = 0x06AEDF0;           // void(StreamingHandle** dst, StreamingHandle** src)
        constexpr uintptr_t Realloc = 0x01E6260;                    // void*(void*, size_t); size 0 frees
        constexpr uintptr_t Alloc = 0x01E6100;                      // void*(size_t)

        // Decima String (refcounted, pointer to chars)
        constexpr uintptr_t StringCtor = 0x0108010;                 // void(String*, const char*, int length)
        constexpr uintptr_t StringRelease = 0x0107850;              // void(String*)
        constexpr uintptr_t EmptyString = 0x1CAABF0;                // chars of the shared empty String

        // Menu data tree (InventoryMenuController + 144); nodes are addressed by name + index
        constexpr uintptr_t NodeChild = 0x0380460;                  // Node*(Tree*, Node* parent, String* name, int index), creates
        constexpr uintptr_t NodeCount = 0x03A1DD0;                  // int(Node* parent, String* name)
        constexpr uintptr_t NodeRemove = 0x0380690;                 // void(Tree*, Node* parent, String* name, int index)
        constexpr uintptr_t NodeSetBool = 0x0D2BBE0;                // void(Node*, bool)
        constexpr uintptr_t NodeSetInt = 0x037BF20;                 // void(Node*, int)
        constexpr uintptr_t NodeSetObject = 0x03A3770;              // void(Node*, void*), raw pointer
        constexpr uintptr_t NodeSetString = 0x03869A0;              // bool(Tree*, Node*, String*)
        constexpr uintptr_t NodeSetText = 0x0380310;                // bool(Tree*, Node*, LocalizedTextResource*)
        constexpr uintptr_t NodeMarkDirty = 0x0198370;              // void(DirtySet* (tree + 104), uint32 crc32c(node), Node**)
        constexpr uintptr_t NodeItemInfo = 0x15D2700;               // void(Tree*, Node*, InventoryItemResource*, bool): name, icon, description...
        constexpr uintptr_t NodeItemRarity = 0x15D3610;             // void(Tree*, Node*, uint8 rarity)
        constexpr uintptr_t NodeItemOwned = 0x15D4130;              // void(Tree*, Node*, Inventory*, InventoryItemResource*, int, int)
        constexpr uintptr_t ItemIcon = 0x120A250;                   // UITexture*(InventoryItemResource*)

        // Hook targets
        constexpr uintptr_t InventoryItem_LoadDependencies = 0x120A8B0;  // void(InventoryItem*, Callback* onLoaded)
        constexpr uintptr_t AmmoSlotsSettings_CollectRefs = 0x1233CD0;   // void(AmmoSlotsSettings*, Array<StreamingHandle*>*)
        constexpr uintptr_t LocalizedText_GetString = 0x06BEE20;         // String*(LocalizedTextResource*, String* out)
        constexpr uintptr_t MenuInputBinding_Matches = 0x038C410;        // bool(MenuInputEventBinding*, Event*)
        constexpr uintptr_t MenuGraphAction_Execute = 0x0373030;         // void(MenuActionGraphProgram*, Menu*, Sender*, Event*)
        constexpr uintptr_t InvMenu_RefreshItemSelected = 0x153B770;     // void(InventoryMenuController*)
        constexpr uintptr_t InvMenu_PopulateModsExport = 0x15AC350;      // thunk called by the menu's "PopulateModsForEditTool" action
        constexpr uintptr_t InvMenu_PopulateMods = 0x1541EC0;            // void(InventoryMenuController*)
        constexpr uintptr_t InvMenu_SelectMod = 0x153DBF0;               // void(InventoryMenuController*, ModificationSocketResource*)
        constexpr uintptr_t InvMenu_SelectFirstModSocket = 0x153EB60;    // void(InventoryMenuController*)
        constexpr uintptr_t InvMenu_PreviewMod = 0x153DD20;              // void(InventoryMenuController*, InventoryItem*)
        constexpr uintptr_t InvMenu_PopulateWeaponsExport = 0x15AC490;   // thunk called by the menu's "PopulateWeapons" action
        constexpr uintptr_t InvMenu_EquipSelectedMod = 0x153E730;        // void(InventoryMenuController*, bool* result)
        constexpr uintptr_t InvMenu_UnequipSelectedMod = 0x153EAC0;      // void(InventoryMenuController*)
        constexpr uintptr_t InvMenu_Destroy = 0x1535CB0;                 // void*(InventoryMenuController*, int flags), vtable slot 1
        // Item details "ammo" list (ammo types with damage): written from the weapon's default ammo
        constexpr uintptr_t ItemPanel_WriteAmmo = 0x15D9100;       // void(Tree*, Node*, InventoryItemResource*, InventoryItem* (may be null), UpgradableWeaponSettings*, int level, int previewLevel, int maxLevel, 3 more)
        constexpr uintptr_t ItemPanel_WriteAmmoEntry = 0x15DA500;  // bool(Tree*, Node*, int* entryIndex, GGUUID* ammo, AmmoSlotsSettings*, 5 more)
        // Menu text elements
        constexpr uintptr_t TextTag_BindText = 0x3C7E40;  // void(TextTag*, MenuDataBinding*, MemberFn* setter): evaluates static bindings now, subscribes dynamic ones
        constexpr uintptr_t TextTag_SetText = 0x3B86F0;   // void(TextTag*, const String&), the setter TextTag passes to BindText
        constexpr uintptr_t WeakPtrLink = 0x011E4F0;       // void(WeakPtr*) with target set
        constexpr uintptr_t WeakPtrUnlink = 0x011E570;     // void(WeakPtr*), clears it
        constexpr uintptr_t VT_MenuLocalizedTextBinding = 0x183D6E8;
        constexpr uintptr_t InventoryUiSettingsPtr = 0x8983128;  // data: InventoryUiSettings*
        constexpr uintptr_t Tag_OnEvent = 0x3BF800;              // void(Tag*, TagEvent*), base handler every tag runs
        constexpr uintptr_t Tag_AddEventHandler = 0x3BE2F0;      // void(Tag::EventHandlers set*, MenuEventBinding** ref, Tag*)

        // RTTI descriptors (type id/range at +0 are filled in by the engine at startup)
        constexpr uintptr_t RTTI_Inventory = 0x21E41D0;
        constexpr uintptr_t RTTI_AmmoSlotsComponent = 0x21F3C00;
        constexpr uintptr_t RTTI_InventoryItemComponent = 0x21E3570;
        constexpr uintptr_t RTTI_ModificationSocketResource = 0x21E6130;
        constexpr uintptr_t RTTI_InventoryItem = 0x21E3150;
        constexpr uintptr_t RTTI_MenuInputEventBinding = 0x2008F40;
        constexpr uintptr_t RTTI_MenuLocalizedTextBinding = 0x200A1D0;
        constexpr uintptr_t RTTI_LocalizedTextResource = 0x20D7D90;
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
