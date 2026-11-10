#include "menu.h"

#include "ammo.h"
#include "bonus.h"
#include "game.h"
#include "hookutil.h"
#include "log.h"

#include <nmmintrin.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

using namespace game;

// How it fits into the game's inventory menu (data in package group 71865, logic in
// InventoryMenuController):
//
// * The inventory page panel has an input binding for the outfit "Apply Visuals" prompt. Its label
//   and visibility come from the menu data tree ("outfit_visuals_text", "current/show_visuals_option"),
//   which the controller only fills for outfits. For ranged weapons with several ammo types the mod
//   fills them, so the panel shows an "Edit Ammo" prompt next to "Edit Coils".
// * Pressing it executes the binding's graph action. The mod intercepts that action and runs the
//   Edit Coils binding's actions instead (populate list, switch to the edit-coils view, sound), with
//   ammo mode on.
// * In ammo mode the controller functions behind the edit-coils view are replaced: the slot row is
//   the weapon's ammo slots at its upgrade level (filled ones first, then empty ones), the list is the
//   ammo of that type the player's weapons hold or come with, equipping puts the ammo into the selected
//   slot and unequipping empties it. Ammo already on the weapon is shown as unavailable (greyed out).
//   Ammo descriptions get the weapon's damage with that ammo appended. The pointer entering a slot
//   runs the view's populate action again (as the game does for coils), which stays in ammo mode; the
//   pointer leaving the slot row doesn't close the view as it does for coils.
// * Leaving the view (cancel), opening another tab or selecting another item ends ammo mode.

namespace
{
    // ---- engine functions -----------------------------------------------------------------------
    struct String
    {
        const char* chars;
    };

    using StringCtor_t = void (*)(String*, const char*, int);
    using StringRelease_t = void (*)(String*);
    using NodeChild_t = void* (*)(void* tree, void* parent, const String* name, int index);
    using NodeCount_t = int (*)(void* parent, const String* name);
    using NodeRemove_t = void (*)(void* tree, void* parent, const String* name, int index);
    using NodeSetBool_t = void (*)(void* node, bool value);
    using NodeSetInt_t = void (*)(void* node, int value);
    using NodeSetObject_t = void (*)(void* node, void* object);
    using NodeSetString_t = bool (*)(void* tree, void* node, const String* value);
    using NodeSetText_t = bool (*)(void* tree, void* node, void* text);
    using NodeMarkDirty_t = void (*)(void* dirtySet, uint32_t hash, void** node);
    using NodeItemInfo_t = void (*)(void* tree, void* node, void* itemResource, bool flag);
    using NodeItemRarity_t = void (*)(void* tree, void* node, uint8_t rarity);
    using NodeItemOwned_t = void (*)(void* tree, void* node, void* inventory, void* itemResource, int, int equipped);
    using ItemIcon_t = void* (*)(void* itemResource);
    using Alloc_t = void* (*)(size_t);
    using ActionExecute_t = void (*)(void* action, void* menu, void* sender, void* event);
    using WeakPtrOp_t = void (*)(WeakPtr*);
    using AddEventHandler_t = void (*)(void* handlers, void** binding, void* tag);

    StringCtor_t StringCtor;
    StringRelease_t StringRelease;
    NodeChild_t NodeChild;
    NodeCount_t NodeCount;
    NodeRemove_t NodeRemove;
    NodeSetBool_t NodeSetBool;
    NodeSetInt_t NodeSetInt;
    NodeSetObject_t NodeSetObject;
    NodeSetString_t NodeSetString;
    NodeSetText_t NodeSetText;
    NodeMarkDirty_t NodeMarkDirty;
    NodeItemInfo_t NodeItemInfo;
    NodeItemRarity_t NodeItemRarity;
    NodeItemOwned_t NodeItemOwned;
    ItemIcon_t ItemIcon;
    Alloc_t Alloc;
    WeakPtrOp_t WeakPtrLink;
    WeakPtrOp_t WeakPtrUnlink;
    AddEventHandler_t AddEventHandler;
    const char* g_emptyString;              // engine's shared empty String data
    uint8_t* g_socketRtti;                  // RTTI of ModificationSocketResource
    uint8_t* g_inventoryItemRtti;           // RTTI of InventoryItem
    uint8_t* g_bindingRtti;                 // RTTI of MenuInputEventBinding
    uint8_t* g_textBindingRtti;             // RTTI of MenuLocalizedTextBinding
    uint8_t* g_textRtti;                    // RTTI of LocalizedTextResource
    uintptr_t g_localizedTextBindingVtable;  // MenuLocalizedTextBinding vtable
    void* g_textTagSetText;                 // TextTag::SetText(const String&)
    void** g_inventoryUiSettings;           // global holding the InventoryUiSettings*

    // "Restore Default Ammo" prompt and Edit Ammo (see hk_TagOnEvent). The inventory menu's resources
    // are streamed: some time after the menu closes they are unloaded, and when it opens again they are
    // new objects. The mod keeps no pointers to them: the page panel's bindings are looked up in its
    // live tag whenever they are used.
    void* g_restoreBinding;      // the mod's MenuInputEventBinding, owned for good
    WeakPtr g_pagePanel;         // inventory page panel tag last set up
    WeakPtr g_pendingSlotPanel;  // slot row tag set up before the page panel was seen

    // ---- objects of the inventory menu (package group 71865) ---------------------------------------
    constexpr GGUUID kVisualsAction = ParseGuid("b6f98ddd-5eeb-9140-a6dd-561427bdbe42");      // "HandleOutfitVisuals" graph action
    constexpr GGUUID kEditCoilsBinding = ParseGuid("069620ca-ee5b-cf4a-8e2f-0b9d918a1b91");   // "Edit Coils" input binding
    constexpr GGUUID kSlotViewAccept = ParseGuid("98a1c47a-cc9e-a343-a99d-49db849424ed");     // edit view, slot row: "Select Slot"
    constexpr GGUUID kSlotViewPanel = ParseGuid("ef42ee8e-0477-7b33-a29e-7605b465720d");      // edit view, slot row panel
    constexpr GGUUID kPagePanel = ParseGuid("e21869c5-2dd2-d546-b3c7-7af576c7d7c4");          // inventory page panel
    constexpr GGUUID kVisualsBinding = ParseGuid("2a2a9681-6a4e-9642-b268-a548680cc58e");     // outfit "Apply Visuals" input binding
    constexpr int32_t kFunctionInbox = 20;  // EMenuInputFunction FUNCTION_INBOX: Triangle, F on keyboard
    // "CloseEditModsIfNotEditing": closes the edit-coils view when the pointer leaves its slot row (or
    // list) before a slot was picked, since hovering the coils also opens it.
    constexpr GGUUID kPointerLeftEditView = ParseGuid("efb032d2-ef7c-b547-8d05-b1402d5fa566");
    // "Highlight item": first action when the inventory's item grid gets focus, which it gets back
    // whenever the edit-coils view closes (cancel, or kPointerLeftEditView).
    constexpr GGUUID kItemGridFocused = ParseGuid("e27a995c-4689-4d41-bc09-9080c9b983dc");

    // Texts of the edit-coils view that read differently while editing ammo.
    struct TextOverride
    {
        GGUUID id;
        const char* text;
    };
    constexpr TextOverride kAmmoTexts[] = {
        {ParseGuid("edc77f8f-bf5b-c245-9ad9-81c10a087647"), "Choose an ammo slot to change."},  // Choose a coil slot to modify.
        {ParseGuid("a3e51773-7119-4442-ba36-b6deee30c7f2"), "Ammo Types"},                      // Coils Inventory
        {ParseGuid("47455b55-b623-8a42-a595-8964239646d1"), "No other ammo available!"},        // No coils available!
        {ParseGuid("bd778cec-236f-6945-8e49-4fb90ad081d3"), "Ammo already equipped!"},          // Coil already in use!
        {ParseGuid("919987f4-246b-174b-8606-de87a993f28d"), "Remove Ammo"},                     // Unequip Coil
        {ParseGuid("59c097d7-fb40-1e43-a1af-7e5bc4c2df5b"), "Empty Ammo Slot"},                 // Empty Coil Slot
        {ParseGuid("20880885-5c94-2643-93de-dee0a7354db3"), "Empty Ammo Slot"},                 // Empty Coil Slot
        {ParseGuid("6783318c-3a82-cf4e-89a3-6636bad54919"), "Put an ammo type in this slot to use it with this weapon."},  // Equip a coil in this slot to improve your weapon.
        {ParseGuid("666b627d-2d0b-7a47-8bf7-105bb8be1eb5"), "Ammo"},                            // Coil (item type label)
        {ParseGuid("131795de-3565-2346-b352-6e1ef388c3e2"), "Ammo Slots"},                      // Coils (weapon panel header)
    };

    const char* FindOverride(const GGUUID& id)
    {
        for (const TextOverride& o : kAmmoTexts)
        {
            if (o.id == id)
                return o.text;
        }
        return nullptr;
    }

    constexpr const char* kEditAmmoPrompt = "Edit Ammo";
    constexpr const char* kEquipAmmoPrompt = "Equip Ammo";  // replaces current/equip_mods_text ("Equip Coil")
    constexpr const char* kRestoreDefaultsPrompt = "Restore Default Ammo";
    constexpr int kMaxSlots = 5;  // the edit view lays out up to five sockets
    constexpr uint8_t kRarityCommon = 2;  // EInventoryItemRarity
    constexpr int kCategoryWeapons = 0;

    // ---- state (menu thread only) ------------------------------------------------------------------
    struct State
    {
        uint8_t* controller = nullptr;     // InventoryMenuController that last refreshed its detail panel
        bool swappableSelected = false;    // its selected item is a weapon with several ammo types
        bool entering = false;             // running the Edit Coils actions on behalf of "Edit Ammo"
        bool ammoMode = false;             // edit-coils view shows ammo
        bool vanillaEdit = false;          // edit-coils view shows coils (or weaves)
        void* weapon = nullptr;            // weapon being edited
        int selectedSlot = 0;
        GGUUID previewAmmo{};              // ammo highlighted in the list
    };
    State g;

    // Stand-in ModificationSocketResources identifying ammo slots, owned by the mod for good.
    void* g_sockets[kMaxSlots];

    // Stand-in InventoryItems for list entries of ammo the player has no stack of (never crafted or
    // used up): the list's "value" goes to PreviewMod, which the menu declares with an InventoryItem
    // parameter. Inert objects (no resource, owner or entity), owned by the mod for good.
    std::vector<std::pair<GGUUID, void*>> g_ammoTokens;

    // ---- menu data tree helpers --------------------------------------------------------------------
    class GameString
    {
    public:
        explicit GameString(const char* text) : m_value{g_emptyString} { StringCtor(&m_value, text, int(strlen(text))); }
        ~GameString() { StringRelease(&m_value); }
        GameString(const GameString&) = delete;
        GameString& operator=(const GameString&) = delete;
        const String* get() const { return &m_value; }

    private:
        String m_value;
    };

    class Tree
    {
    public:
        explicit Tree(uint8_t* controller) : m_controller(controller) {}

        void* tree() const { return m_controller + InvMenu::Tree; }
        void* root() const { return m_controller + InvMenu::Tree + 8; }

        void* Child(void* parent, const char* name, int index = 0) const
        {
            GameString n(name);
            return NodeChild(tree(), parent, n.get(), index);
        }

        void Bool(void* parent, const char* name, bool value) const
        {
            void* node = Child(parent, name);
            NodeSetBool(node, value);
            Dirty(node);
        }

        void Int(void* parent, const char* name, int value) const
        {
            void* node = Child(parent, name);
            NodeSetInt(node, value);
            Dirty(node);
        }

        void Object(void* parent, const char* name, void* value) const
        {
            void* node = Child(parent, name);
            NodeSetObject(node, value);
            Dirty(node);
        }

        void Str(void* parent, const char* name, const char* value) const
        {
            GameString s(value);
            NodeSetString(tree(), Child(parent, name), s.get());
        }

        void Text(void* parent, const char* name, void* text) const { NodeSetText(tree(), Child(parent, name), text); }

        // Removes children `name` with index >= keep.
        void Trim(void* parent, const char* name, int keep) const
        {
            GameString n(name);
            for (int i = NodeCount(parent, n.get()) - 1; i >= keep; i--)
                NodeRemove(tree(), parent, n.get(), i);
        }

    private:
        void Dirty(void* node) const
        {
            NodeMarkDirty(m_controller + InvMenu::DirtySet, uint32_t(_mm_crc32_u64(0, reinterpret_cast<uint64_t>(node))), &node);
        }

        uint8_t* m_controller;
    };

    void* OwnerInventoryPtr(uint8_t* controller)
    {
        auto* p = At<uint8_t*>(controller, InvMenu::Inventory);
        return p ? p - 32 : nullptr;
    }

    void* Texts(void* itemResource, size_t offset) { return itemResource ? At<void*>(itemResource, offset) : nullptr; }

    // Stand-in socket objects: the edit-coils view passes slot identities around as
    // ModificationSocketResource objects (the type SelectMod's menu binding is declared with).
    bool EnsureSockets()
    {
        if (g_sockets[0])
            return true;
        const uint32_t size = At<uint32_t>(g_socketRtti, RTTI::Size);
        auto construct = At<void (*)(void*, void*)>(g_socketRtti, RTTI::Constructor);
        for (int i = 0; i < kMaxSlots; i++)
        {
            void* socket = Alloc(size);
            if (!socket)
                return false;
            memset(socket, 0, size);
            construct(g_socketRtti, socket);
            ObjectUUID(socket) = GGUUID{kModUuidLo, uint64_t(i + 1)};  // "AmmoSwap" + slot
            At<int32_t>(socket, RefObject::RefCount) = 1;               // owned by the mod for good
            g_sockets[i] = socket;
        }
        return true;
    }

    int SlotOfSocket(void* socket)
    {
        for (int i = 0; i < kMaxSlots; i++)
        {
            if (g_sockets[i] == socket)
                return i;
        }
        return -1;
    }

    void* AmmoToken(const GGUUID& ammo)
    {
        for (const auto& [id, token] : g_ammoTokens)
        {
            if (id == ammo)
                return token;
        }
        const uint32_t size = At<uint32_t>(g_inventoryItemRtti, RTTI::Size);
        void* token = Alloc(size);
        if (!token)
            return nullptr;
        memset(token, 0, size);
        At<void (*)(void*, void*)>(g_inventoryItemRtti, RTTI::Constructor)(g_inventoryItemRtti, token);
        At<int32_t>(token, RefObject::RefCount) = 1;
        g_ammoTokens.emplace_back(ammo, token);
        return token;
    }

    // Ammo a list entry's "value" (a stack or a stand-in) stands for.
    GGUUID AmmoOfListValue(void* value)
    {
        for (const auto& [id, token] : g_ammoTokens)
        {
            if (token == value)
                return id;
        }
        return ammo::AmmoOfItemResource(ammo::ItemResource(value));
    }

    bool InAmmoMode(void* controller) { return g.ammoMode && controller == g.controller && g.weapon; }

    // ---- node writers ------------------------------------------------------------------------------

    // One ammo slot, written like the game writes a coil socket (InventoryMenuController slot rows and
    // "selected_mod"). `ammo` is null for an empty slot.
    void WriteSlot(const Tree& t, void* node, int slot, const GGUUID* ammo, bool selected)
    {
        void* resource = ammo ? ammo::SupportedAmmoItem(g.weapon, *ammo) : nullptr;
        t.Object(node, "value", g_sockets[slot]);
        if (resource)
        {
            t.Text(node, "name", Texts(resource, InventoryItemResource::ItemName));
            t.Text(node, "description", Texts(resource, InventoryItemResourceUI::ItemDescription));
        }
        else
        {
            t.Str(node, "name", "");
            t.Str(node, "description", "");
        }
        // The socket's colour and rarity line come from these (they'd otherwise keep the coil's).
        t.Object(node, "mod_entity", resource);
        NodeItemRarity(t.tree(), node, resource ? At<uint8_t>(resource, InventoryItemResourceUI::Rarity) : kRarityCommon);
        t.Bool(node, "is_locked", false);
        t.Bool(node, "is_previewed", false);
        t.Bool(node, "is_celebration", false);
        t.Int(node, "socket_level", 0);
        t.Int(node, "socket_display_level", 1);
        t.Bool(node, "is_even", slot % 2 == 1);
        t.Bool(node, "is_empty", !resource);
        t.Bool(node, "is_mod_slot_selected", selected);
        t.Object(node, "mod_icon", resource ? ItemIcon(resource) : nullptr);
        t.Int(node, "upgrade_state", 3);
        t.Bool(node, "is_coil", true);
        t.Bool(node, "is_modification", true);
        t.Str(node, "mod_value", "");
    }

    // The slot row under "current" (what "custom_mod" shows for coil sockets): the weapon's slots at its
    // upgrade level, filled ones first.
    void WriteSlots(uint8_t* controller)
    {
        const Tree t(controller);
        void* current = t.Child(t.root(), "current");
        const std::vector<GGUUID> assigned = ammo::GetAssigned(g.weapon, nullptr);
        const int count = std::min(ammo::SlotCapacity(g.weapon), kMaxSlots);
        for (int i = 0; i < count; i++)
            WriteSlot(t, t.Child(current, "custom_mod", i), i, i < int(assigned.size()) ? &assigned[i] : nullptr, i == g.selectedSlot);
        t.Trim(current, "custom_mod", count);
        t.Int(current, "mod_slots_size", count);
        t.Bool(current, "has_one_mod", count == 1);
        t.Bool(current, "has_two_mod", count == 2);
        t.Bool(current, "has_three_mod", count == 3);
        t.Bool(current, "has_four_mod", count == 4);
        t.Bool(current, "has_five_mod", count == 5);
        t.Bool(current, "has_custom_mod_slots", count > 0);
    }

    // Fields every list entry ("edit_tool_mods/mod") gets, like PopulateModsForEditTool writes them.
    void WriteListEntry(const Tree& t, uint8_t* controller, void* node, void* value, bool equipped)
    {
        void* controllerResource = At<void*>(controller, InvMenu::Resource);
        t.Bool(node, "is_equipped", equipped);
        t.Bool(node, "is_equipped_by_selected_item", equipped);
        t.Bool(node, "is_top_right_visible", equipped);
        if (controllerResource)
        {
            // InventoryMenuControllerResource: Icons (+216) -> equipped icon (+56); EquippedWeaponModIcon (+152)
            void* icons = At<void*>(controllerResource, 216);
            t.Object(node, "top_right_icon", equipped && icons ? At<void*>(icons, 56) : At<void*>(controllerResource, 152));
        }
        t.Bool(node, "is_compatible", true);
        t.Bool(node, "is_available", !equipped);
        t.Str(node, "mod_value", "");
        // The preview graph takes the entry's inventory item; stand-ins go there too.
        t.Object(node, "inventoryItem", value);
        t.Object(node, "value", value);
        t.Bool(node, "is_enabled", true);
        t.Bool(node, "is_visible", true);
        t.Bool(node, "is_marked", false);
        t.Bool(node, "is_tracked", false);
        t.Bool(node, "is_passive", false);
        t.Bool(node, "has_upgrade_text", false);
        t.Bool(node, "has_normal_level_text", false);
        t.Bool(node, "is_low_opacity", equipped);
        t.Int(node, "guidance_type", 0);
    }

    // The list ("edit_tool_mods") of ammo the weapon can take, like PopulateModsForEditTool writes coils.
    void WriteAmmoList(uint8_t* controller)
    {
        const Tree t(controller);
        void* list = t.Child(t.root(), "edit_tool_mods");
        void* inventory = OwnerInventoryPtr(controller);
        const std::vector<GGUUID> assigned = ammo::GetAssigned(g.weapon, nullptr);

        int count = 0;

        for (const GGUUID& id : ammo::Candidates(g.weapon))
        {
            void* stack = ammo::AmmoStack(g.weapon, id);
            void* resource = ammo::SupportedAmmoItem(g.weapon, id);
            void* value = stack ? stack : AmmoToken(id);
            if (!resource || !value)
                continue;
            const bool equipped = std::find(assigned.begin(), assigned.end(), id) != assigned.end();

            void* node = t.Child(list, "mod", count++);
            NodeItemInfo(t.tree(), node, resource, false);
            NodeItemRarity(t.tree(), node, At<uint8_t>(resource, InventoryItemResourceUI::Rarity));
            if (inventory)
                NodeItemOwned(t.tree(), node, inventory, resource, 0, -1);
            t.Object(node, "item", resource);
            WriteListEntry(t, controller, node, value, equipped);
        }
        t.Trim(list, "mod", count);
        t.Bool(list, "has_mods", count > 0);
    }

    // Item description followed by the damage the edited weapon deals with `ammo` (defined below).
    void WriteAmmoDescription(uint8_t* controller, void* node, void* ammoItemResource, const GGUUID& ammo);

    void WriteSelectedSlot(uint8_t* controller)
    {
        const std::vector<GGUUID> assigned = ammo::GetAssigned(g.weapon, nullptr);
        const Tree t(controller);
        void* node = t.Child(t.root(), "selected_mod");
        const GGUUID* ammo = g.selectedSlot < int(assigned.size()) ? &assigned[g.selectedSlot] : nullptr;
        WriteSlot(t, node, g.selectedSlot, ammo, true);
        if (void* resource = ammo ? ammo::SupportedAmmoItem(g.weapon, *ammo) : nullptr)
            WriteAmmoDescription(controller, node, resource, *ammo);
    }

    void WritePreview(uint8_t* controller, const GGUUID& id)
    {
        const Tree t(controller);
        void* node = t.Child(t.root(), "previewed_mod");
        void* resource = ammo::SupportedAmmoItem(g.weapon, id);
        const std::vector<GGUUID> assigned = ammo::GetAssigned(g.weapon, nullptr);
        t.Text(node, "name", Texts(resource, InventoryItemResource::ItemName));
        WriteAmmoDescription(controller, node, resource, id);
        t.Object(node, "mod_icon", resource ? ItemIcon(resource) : nullptr);
        t.Object(node, "mod_entity", resource);
        if (resource)
            NodeItemRarity(t.tree(), node, At<uint8_t>(resource, InventoryItemResourceUI::Rarity));
        t.Bool(node, "is_equipped_by_selected_item", std::find(assigned.begin(), assigned.end(), id) != assigned.end());
        t.Bool(node, "is_coil", true);
        t.Bool(node, "is_modification", true);
        t.Bool(node, "is_empty", false);
        t.Bool(node, "is_compatible", true);
        t.Str(node, "equipped_by", "");
        t.Str(node, "mod_value", "");
    }

    // ---- hooks -------------------------------------------------------------------------------------
    using Matches_t = bool (*)(void* binding, void* event);
    using GraphExecute_t = void (*)(void* action, void* menu, void* sender, void* event);
    using GetString_t = String* (*)(void* text, String* out);
    using Controller_t = void (*)(void* controller);
    using ControllerObj_t = void (*)(void* controller, void* object);
    using Equip_t = void (*)(void* controller, uint8_t* result);
    using Destroy_t = void* (*)(void* controller, int flags);
    using BindText_t = void (*)(void* textTag, void* binding, const void* setter);

    Matches_t o_Matches;
    GraphExecute_t o_GraphExecute;
    GetString_t o_GetString;
    Controller_t o_Refresh;
    Controller_t o_PopulateModsExport;
    Controller_t o_PopulateMods;
    ControllerObj_t o_SelectMod;
    Controller_t o_SelectFirstSocket;
    ControllerObj_t o_PreviewMod;
    Equip_t o_Equip;
    Controller_t o_Unequip;
    Destroy_t o_Destroy;
    Controller_t o_PopulateWeaponsExport;
    BindText_t o_BindText;
    using TagOnEvent_t = void (*)(void* tag, void* event);
    TagOnEvent_t o_TagOnEvent;

    void RefreshAmmoView(uint8_t* controller);

    // ---- static texts ------------------------------------------------------------------------------
    // Text elements with a parameterless MenuLocalizedTextBinding get their text once, when the
    // inventory is built, so GetString never sees ammo mode for them. Track the ones showing a text
    // we override and evaluate them again whenever ammo mode starts or ends.
    struct MemberFn
    {
        void* fn;
        int64_t thisAdjust;
    };
    WeakPtr g_staticTexts[32];  // targets: TextTag + TextTag::WeakTarget

    bool IsOverriddenStaticText(void* binding)
    {
        if (!binding || At<uintptr_t>(binding, 0) != g_localizedTextBindingVtable)
            return false;
        void* text = At<void*>(binding, MenuLocalizedTextBinding::LocalizedText);
        return text && At<int>(binding, MenuLocalizedTextBinding::ParameterCount) == 0 && FindOverride(ObjectUUID(text));
    }

    void hk_BindText(void* textTag, void* binding, const void* setter)
    {
        o_BindText(textTag, binding, setter);
        if (!IsOverriddenStaticText(binding))
            return;
        void* target = static_cast<uint8_t*>(textTag) + TextTag::WeakTarget;
        WeakPtr* free = nullptr;
        for (WeakPtr& w : g_staticTexts)
        {
            if (w.target == target)
                return;
            if (!w.target && !free)
                free = &w;
        }
        if (!free)
        {
            LOG("More coil texts than can be tracked; some keep their coil wording");
            return;
        }
        *free = WeakPtr{target, nullptr, nullptr, free};
        WeakPtrLink(free);
    }

    // Texts the edit-coils view gets once (static text elements, the equip prompt node) and the
    // visibility of the mod's Restore Default Ammo prompt (see ShowRestorePrompt).
    void ApplyModeTexts(uint8_t* controller)
    {
        const MemberFn setter{g_textTagSetText, 0};
        for (WeakPtr& w : g_staticTexts)
        {
            if (!w.target)
                continue;
            auto* textTag = static_cast<uint8_t*>(w.target) - TextTag::WeakTarget;
            void* resource = At<void*>(textTag, TextTag::Resource);
            void* binding = resource ? At<void*>(resource, MenuTextResource::Text) : nullptr;
            if (IsOverriddenStaticText(binding))
                o_BindText(textTag, binding, &setter);
        }

        if (!controller)
            return;
        const Tree t(controller);
        void* current = t.Child(t.root(), "current");
        if (g.ammoMode)
            t.Str(current, "equip_mods_text", kEquipAmmoPrompt);
        else if (void* resource = At<void*>(controller, InvMenu::Resource))
            t.Text(current, "equip_mods_text", At<void*>(resource, InvMenuResource::EquipModText));
        t.Bool(current, "show_visuals_option", g.ammoMode);
    }

    void EndAmmoMode()
    {
        if (!g.ammoMode)
            return;
        g.ammoMode = false;
        g.weapon = nullptr;
        g.previewAmmo = {};
        if (g.controller)
            At<void*>(g.controller, InvMenu::PreviewedMod) = nullptr;
        ApplyModeTexts(g.controller);
        LOG("Edit Ammo closed");
    }

    // The mod's Restore Default Ammo binding in the edit view shows when this is set.
    void ShowRestorePrompt(uint8_t* controller, bool show)
    {
        const Tree t(controller);
        t.Bool(t.Child(t.root(), "current"), "show_visuals_option", show);
    }

    // Edit Ammo prompt for the selected weapon: the outfit visuals binding reads these nodes.
    void ShowEditAmmoPrompt(uint8_t* controller)
    {
        const Tree t(controller);
        void* current = t.Child(t.root(), "current");
        t.Bool(current, "show_visuals_option", true);
        t.Bool(current, "enable_visual_options", true);
        t.Str(t.root(), "outfit_visuals_text", kEditAmmoPrompt);
    }

    void hk_Refresh(void* controllerPtr)
    {
        auto* controller = static_cast<uint8_t*>(controllerPtr);
        o_Refresh(controller);

        void* item = At<void*>(controller, InvMenu::SelectedItem);
        if (g.ammoMode && (controller != g.controller || item != g.weapon))
            EndAmmoMode();

        g.controller = controller;
        g.swappableSelected = item && At<int8_t>(controller, InvMenu::Category) == kCategoryWeapons && ammo::HasSwappableAmmo(ammo::ItemResource(item));
        if (g.ammoMode)
        {
            WriteSlots(controller);
            ShowRestorePrompt(controller, true);
        }
        else if (g.vanillaEdit)
        {
            ShowRestorePrompt(controller, false);
        }
        else if (g.swappableSelected)
        {
            ShowEditAmmoPrompt(controller);
        }
    }

    void RestoreDefaultsFromInput()
    {
        static ULONGLONG s_last;
        const ULONGLONG now = GetTickCount64();
        if (now - s_last < 250)
            return;  // one press may be matched more than once
        s_last = now;
        const bool ok = ammo::RestoreDefaults(g.weapon);
        LOG("Edit Ammo: default ammo restored -> %s", ok ? "ok" : "failed");
        bonus::Refresh(g.weapon);  // also after a partial restore
        g.selectedSlot = std::min(g.selectedSlot, ammo::SlotCapacity(g.weapon) - 1);
        RefreshAmmoView(g.controller);
    }

    bool hk_Matches(void* binding, void* event)
    {
        if (binding == g_restoreBinding)
        {
            const bool matched = o_Matches(binding, event);
            if (matched && g.ammoMode && g.weapon && g.controller)
                RestoreDefaultsFromInput();
            return matched;
        }
        return o_Matches(binding, event);
    }

    // ---- "Restore Default Ammo" prompt ---------------------------------------------------------------
    // The slot row of the edit view only has Accept and Menu Options bindings. Each time its panel is
    // set up, the mod adds one more: Triangle (F on keyboard), no actions of its own (hk_Matches acts on
    // it), shown in the button legend like the game's prompts. It reuses the outfit visuals prompt's
    // visibility (current/show_visuals_option), which is only set while the view shows ammo.

    // Binding `id` of the inventory page panel as it is loaded now, or null.
    void* PagePanelBinding(const GGUUID& id)
    {
        auto* target = static_cast<uint8_t*>(g_pagePanel.target);
        void* resource = target ? At<void*>(target - Tag::WeakTarget, Tag::Resource) : nullptr;
        if (!resource)
            return nullptr;
        for (void* binding : At<Array<void*>>(resource, MenuElementResource::EventHandlers))
        {
            if (binding && ObjectUUID(binding) == id)
                return binding;
        }
        return nullptr;
    }

    void* VisualsVisible()
    {
        void* binding = PagePanelBinding(kVisualsBinding);
        return binding ? At<void*>(binding, MenuInputEventBinding::Visible) : nullptr;
    }

    void* NewObject(uint8_t* rtti)
    {
        const uint32_t size = At<uint32_t>(rtti, RTTI::Size);
        void* object = Alloc(size);
        if (!object)
            return nullptr;
        memset(object, 0, size);
        At<void (*)(void*, void*)>(rtti, RTTI::Constructor)(rtti, object);
        At<int32_t>(object, RefObject::RefCount) = 1;  // owned by the mod for good
        return object;
    }

    void* MakeRestoreBinding(void* slotPanel)
    {
        void* source = nullptr;  // the panel's "Select Slot" binding: same event type and legend
        for (void* binding : At<Array<void*>>(slotPanel, MenuElementResource::EventHandlers))
        {
            if (binding && ObjectUUID(binding) == kSlotViewAccept)
                source = binding;
        }
        if (!source)
            return nullptr;

        void* text = NewObject(g_textRtti);
        void* textBinding = NewObject(g_textBindingRtti);
        const uint32_t size = At<uint32_t>(g_bindingRtti, RTTI::Size);
        void* binding = Alloc(size);
        if (!text || !textBinding || !binding)
            return nullptr;
        ObjectUUID(text) = GGUUID{kModUuidLo, 0x7478655274736552ull};         // "AmmoSwap" "RestText"
        At<const char*>(text, LocalizedTextResource::Text) = kRestoreDefaultsPrompt;
        At<uint16_t>(text, LocalizedTextResource::Length) = uint16_t(strlen(kRestoreDefaultsPrompt));
        ObjectUUID(textBinding) = GGUUID{kModUuidLo, 0x646E694274736552ull};  // "AmmoSwap" "RestBind"
        At<void*>(textBinding, MenuLocalizedTextBinding::LocalizedText) = text;

        memcpy(binding, source, size);
        ObjectUUID(binding) = GGUUID{kModUuidLo, 0x7475706E74736552ull};      // "AmmoSwap" "RestInpt"
        int32_t& refCount = At<int32_t>(binding, RefObject::RefCount);
        refCount = (refCount & ~0x3FFFFF) | 1;
        At<Array<void*>>(binding, MenuInputEventBinding::Actions) = {};
        At<Array<void*>>(binding, MenuInputEventBinding::DisabledActions) = {};
        At<void*>(binding, MenuInputEventBinding::Enabled) = nullptr;
        At<int32_t>(binding, MenuInputEventBinding::Function) = kFunctionInbox;
        At<int32_t>(binding, MenuInputEventBinding::ButtonTrigger) = 0;
        At<void*>(binding, MenuInputEventBinding::Text) = textBinding;
        return binding;
    }

    void AddRestorePrompt(void* slotPanelTag, void* visible)
    {
        if (!g_restoreBinding)
            g_restoreBinding = MakeRestoreBinding(At<void*>(slotPanelTag, Tag::Resource));
        if (!g_restoreBinding)
            return;
        At<void*>(g_restoreBinding, MenuInputEventBinding::Visible) = visible;
        AddEventHandler(static_cast<uint8_t*>(slotPanelTag) + Tag::EventHandlers, &g_restoreBinding, slotPanelTag);
    }

    void hk_TagOnEvent(void* tag, void* event)
    {
        o_TagOnEvent(tag, event);
        if (At<int32_t>(event, TagEvent::Type) != 1)
            return;
        void* resource = At<void*>(tag, Tag::Resource);
        if (!resource)
            return;
        const GGUUID& id = ObjectUUID(resource);
        if (id == kPagePanel)
        {
            WeakPtrUnlink(&g_pagePanel);
            g_pagePanel = WeakPtr{static_cast<uint8_t*>(tag) + Tag::WeakTarget, nullptr, nullptr, &g_pagePanel};
            WeakPtrLink(&g_pagePanel);
            // The slot row may have been set up first.
            if (void* pending = g_pendingSlotPanel.target)
            {
                WeakPtrUnlink(&g_pendingSlotPanel);
                if (void* visible = VisualsVisible())
                    AddRestorePrompt(static_cast<uint8_t*>(pending) - Tag::WeakTarget, visible);
            }
        }
        else if (id == kSlotViewPanel)
        {
            if (void* visible = VisualsVisible())
            {
                AddRestorePrompt(tag, visible);
            }
            else
            {
                WeakPtrUnlink(&g_pendingSlotPanel);
                g_pendingSlotPanel = WeakPtr{static_cast<uint8_t*>(tag) + Tag::WeakTarget, nullptr, nullptr, &g_pendingSlotPanel};
                WeakPtrLink(&g_pendingSlotPanel);
            }
        }
    }

    // "Edit Ammo" pressed: run what "Edit Coils" runs, with ammo mode on.
    bool OpenEditAmmo(void* menu, void* sender, void* event)
    {
        void* binding = PagePanelBinding(kEditCoilsBinding);
        if (!binding)
        {
            LOG("Edit Ammo: Edit Coils binding not found");
            return false;
        }
        if (!EnsureSockets())
            return false;

        g.entering = true;
        for (void* action : At<Array<void*>>(binding, MenuEventBinding::Actions))
        {
            if (!action)
                continue;
            auto execute = reinterpret_cast<ActionExecute_t>((*static_cast<void***>(action))[MenuAction::ExecuteSlot]);
            execute(action, menu, sender, event);
        }
        g.entering = false;
        LOG("Edit Ammo opened (ammo mode %d)", g.ammoMode);
        return true;
    }

    void hk_GraphExecute(void* action, void* menu, void* sender, void* event)
    {
        const GGUUID id = ObjectUUID(action);
        if (id == kVisualsAction && (g.swappableSelected || g.ammoMode) && g.controller)
        {
            // The outfit visuals graph never runs for weapons: the prompt only exists there as Edit Ammo.
            if (!g.ammoMode)
                OpenEditAmmo(menu, sender, event);
            return;
        }

        // Edit Ammo is only ever opened on purpose, so its view stays open until cancelled.
        if (id == kPointerLeftEditView && g.ammoMode)
            return;

        if (id == kItemGridFocused && (g.ammoMode || g.vanillaEdit))
        {
            // The edit view closed. End the modes first so this action's own refresh puts back the
            // coil slots and the Edit Ammo prompt.
            EndAmmoMode();
            g.vanillaEdit = false;
        }

        o_GraphExecute(action, menu, sender, event);
    }

    String* hk_GetString(void* text, String* out)
    {
        if (g.ammoMode)
        {
            if (const char* replacement = FindOverride(ObjectUUID(text)))
            {
                String value{g_emptyString};
                StringCtor(&value, replacement, int(strlen(replacement)));
                out->chars = value.chars;  // ownership moves to the caller
                return out;
            }
        }
        return o_GetString(text, out);
    }

    // Menu action "PopulateModsForEditTool": entry into the edit-coils view, and with the mouse also
    // the pointer entering one of its slots.
    void hk_PopulateModsExport(void* controller)
    {
        if (g.entering)
        {
            auto* c = static_cast<uint8_t*>(controller);
            g.controller = c;
            g.weapon = At<void*>(c, InvMenu::SelectedItem);
            g.selectedSlot = 0;
            g.ammoMode = g.weapon != nullptr;
            if (g.ammoMode)
                ApplyModeTexts(c);
        }
        else if (!InAmmoMode(controller))  // in ammo mode hk_PopulateMods writes the ammo again
        {
            // Plain Edit Coils (or Edit Weaves): keep the mod's Restore Default Ammo prompt hidden.
            EndAmmoMode();  // left over from another controller
            g.vanillaEdit = true;
            ShowRestorePrompt(static_cast<uint8_t*>(controller), false);
        }
        o_PopulateModsExport(controller);
    }

    void hk_PopulateMods(void* controller)
    {
        if (!InAmmoMode(controller))
            return o_PopulateMods(controller);
        auto* c = static_cast<uint8_t*>(controller);
        WriteAmmoList(c);
        WriteSlots(c);
        // The view's opening actions may have selected the first coil socket before ammo mode was
        // on, leaving that coil in the slot card; select the first ammo slot instead.
        At<void*>(c, InvMenu::SelectedSocket) = g_sockets[g.selectedSlot];
        WriteSelectedSlot(c);
    }

    void hk_SelectMod(void* controller, void* socket)
    {
        if (!InAmmoMode(controller))
            return o_SelectMod(controller, socket);
        const int slot = SlotOfSocket(socket);
        g.selectedSlot = slot < 0 ? 0 : slot;
        At<void*>(controller, InvMenu::SelectedSocket) = g_sockets[g.selectedSlot];
        WriteSelectedSlot(static_cast<uint8_t*>(controller));
        WriteSlots(static_cast<uint8_t*>(controller));
    }

    void hk_SelectFirstSocket(void* controller)
    {
        if (!InAmmoMode(controller))
            return o_SelectFirstSocket(controller);
        hk_SelectMod(controller, g_sockets[0]);
    }

    void hk_PreviewMod(void* controller, void* value)
    {
        if (!InAmmoMode(controller))
            return o_PreviewMod(controller, value);
        if (!value)
            return;
        g.previewAmmo = AmmoOfListValue(value);
        // The controller's own field only ever gets a real stack (or null for stand-ins).
        At<void*>(controller, InvMenu::PreviewedMod) = ammo::AmmoStack(g.weapon, g.previewAmmo);
        WritePreview(static_cast<uint8_t*>(controller), g.previewAmmo);
    }

    void RefreshAmmoView(uint8_t* controller)
    {
        WriteAmmoList(controller);
        hk_SelectMod(controller, g_sockets[g.selectedSlot]);
        hk_Refresh(controller);
    }

    void hk_Equip(void* controller, uint8_t* result)
    {
        if (!InAmmoMode(controller))
            return o_Equip(controller, result);
        if (result)
            *result = 0;

        const GGUUID ammo = g.previewAmmo;
        if (!ammo.IsNull())
        {
            const int filled = int(ammo::GetAssigned(g.weapon, nullptr).size());
            if (g.selectedSlot < filled)
            {
                const bool ok = ammo::SetSlotAmmo(g.weapon, g.selectedSlot, ammo);
                LOG("Edit Ammo: slot %d -> %s", g.selectedSlot, ok ? "swapped" : "rejected");
                if (ok)
                    bonus::Refresh(g.weapon);
            }
            else
            {
                // Empty slots follow the filled ones, so the ammo lands in the first empty slot.
                const bool ok = ammo::AddSlot(g.weapon, ammo);
                LOG("Edit Ammo: slot %d -> %s", filled, ok ? "added" : "rejected");
                if (ok)
                {
                    g.selectedSlot = filled;
                    bonus::Refresh(g.weapon);
                }
            }
        }
        g.previewAmmo = {};
        At<void*>(controller, InvMenu::PreviewedMod) = nullptr;
        RefreshAmmoView(static_cast<uint8_t*>(controller));
    }

    // "Unequip" empties the slot; the weapon's later ammo moves up one slot.
    void hk_Unequip(void* controller)
    {
        if (!InAmmoMode(controller))
            return o_Unequip(controller);
        const bool ok = ammo::RemoveSlot(g.weapon, g.selectedSlot);
        LOG("Edit Ammo: slot %d removed -> %s", g.selectedSlot, ok ? "ok" : "rejected");
        if (ok)
            bonus::Refresh(g.weapon);
        RefreshAmmoView(static_cast<uint8_t*>(controller));
    }

    // Menu action "PopulateWeapons": the weapons tab is (re)built, so no edit view is open.
    void hk_PopulateWeaponsExport(void* controller)
    {
        EndAmmoMode();
        g.vanillaEdit = false;
        o_PopulateWeaponsExport(controller);
    }

    void* hk_Destroy(void* controller, int flags)
    {
        if (controller == g.controller)
            g = State{};
        return o_Destroy(controller, flags);
    }

    // ---- item details "ammo" list ------------------------------------------------------------------
    // The game lists a weapon's ammo types (with damage) from its default ammo: InitialAmmoConfig,
    // then upgrade unlocks (locked ones of later levels included), which is also the order of its ammo
    // slots. For a weapon with swappable ammo, entry N shows what slot N holds, and entries of emptied
    // slots are left out.
    using WriteAmmoPanel_t = void (*)(void* tree, void* node, void* itemResource, void* item, void* upgrades,
                                      uint64_t level, uint64_t previewLevel, uint64_t maxLevel, uint64_t, uint64_t, uint64_t);
    using WriteAmmoEntry_t = char (*)(void* tree, void* node, int* entryIndex, const GGUUID* ammo, void* settings,
                                      uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

    WriteAmmoPanel_t o_WriteAmmoPanel;
    WriteAmmoEntry_t o_WriteAmmoEntry;

    // The weapon whose ammo list is being written on this thread.
    struct PanelWeapon
    {
        std::vector<GGUUID> slots;
        int capacity = 0;   // slots at its upgrade level; later entries are locked unlocks
        int entry = 0;      // default-ammo entry the next WriteAmmoEntry call is for
    };
    thread_local PanelWeapon* t_panelWeapon;

    void hk_WriteAmmoPanel(void* tree, void* node, void* itemResource, void* item, void* upgrades,
                           uint64_t level, uint64_t previewLevel, uint64_t maxLevel, uint64_t a9, uint64_t a10, uint64_t a11)
    {
        PanelWeapon weapon;
        const bool swappable = item && ammo::HasSwappableAmmo(itemResource);
        if (swappable)
        {
            weapon.slots = ammo::GetAssigned(item, nullptr);
            weapon.capacity = ammo::SlotCapacity(item);
        }
        PanelWeapon* outer = t_panelWeapon;
        t_panelWeapon = swappable ? &weapon : nullptr;
        o_WriteAmmoPanel(tree, node, itemResource, item, upgrades, level, previewLevel, maxLevel, a9, a10, a11);
        t_panelWeapon = outer;
    }

    char hk_WriteAmmoEntry(void* tree, void* node, int* entryIndex, const GGUUID* ammo, void* settings,
                           uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10)
    {
        if (PanelWeapon* weapon = t_panelWeapon)
        {
            const int entry = weapon->entry++;
            if (entry < int(weapon->slots.size()))
            {
                if (!weapon->slots[entry].IsNull())
                    ammo = &weapon->slots[entry];
            }
            else if (entry < weapon->capacity)
            {
                return 0;  // emptied slot: skipped like an entry the weapon doesn't support
            }
        }
        return o_WriteAmmoEntry(tree, node, entryIndex, ammo, settings, a6, a7, a8, a9, a10);
    }

    // ---- ammo damage lines -------------------------------------------------------------------------
    std::string ResolveText(void* text)
    {
        if (!text)
            return {};
        String value{g_emptyString};
        o_GetString(text, &value);
        std::string result = value.chars ? value.chars : "";
        StringRelease(&value);
        return result;
    }

    // The damage type a stat icon of the item details ammo list stands for.
    void* StatDescriptionOfIcon(void* icon)
    {
        void* ui = icon && g_inventoryUiSettings ? *g_inventoryUiSettings : nullptr;
        void* display = ui ? At<void*>(ui, InventoryUiSettings::StatDisplay) : nullptr;
        if (!display)
            return nullptr;
        for (const DamageStatEntry& entry : At<Array<DamageStatEntry>>(display, StatDisplaySettings::Entries))
        {
            if (entry.description && At<void*>(entry.description, DamageTypeStatDescription::Icon) == icon)
                return entry.description;
        }
        return nullptr;
    }

    // One line per damage stat ("<icon> Impact 192"), with the numbers the item details ammo list
    // shows: the game writes a one-entry list for `ammo` on the edited weapon into a scratch node. The
    // numbers are those of the weapon holding `ammo`, damage bonuses for swapped-in ammo included.
    std::string AmmoStatsText(uint8_t* controller, const GGUUID& ammo)
    {
        const Tree t(controller);
        void* scratch = t.Child(t.root(), "ammoswap_stats");
        PanelWeapon weapon;
        weapon.slots = {ammo};
        weapon.capacity = INT_MAX;  // entry 0 shows `ammo`, the rest are left out
        PanelWeapon* outer = t_panelWeapon;
        t_panelWeapon = &weapon;
        {
            const bonus::PreviewAmmo preview(weapon.slots);
            // previewLevel -1: no upgrade preview; maxLevel -1: skips the upgrade unlock entries, which
            // are the only users of the upgrade settings argument.
            o_WriteAmmoPanel(t.tree(), scratch, ammo::ItemResource(g.weapon), g.weapon, nullptr,
                             uint64_t(int64_t(ammo::UpgradeLevel(g.weapon))), uint64_t(-1), uint64_t(-1), 0, 0, 0);
        }
        t_panelWeapon = outer;

        std::string text;
        void* slot = t.Child(t.Child(scratch, "ammo"), "ammo_slot");
        const GameString statName("ammo_dmg_stat");
        const int count = NodeCount(slot, statName.get());
        for (int i = 0; i < count; i++)
        {
            void* stat = t.Child(slot, "ammo_dmg_stat", i);
            void* icon = At<void*>(t.Child(stat, "ammo_dmg_icon"), MenuNode::Value);
            const int value = At<int>(t.Child(stat, "ammo_dmg_value"), MenuNode::Value);
            if (!text.empty())
                text += "\n";
            if (void* description = StatDescriptionOfIcon(icon))
            {
                const char* iconName = At<const char*>(description, DamageTypeStatDescription::ImageMapEntryID);
                if (iconName && *iconName)
                    text += std::string("<icon name=") + iconName + "> ";
                text += ResolveText(At<void*>(description, DamageTypeStatDescription::Name)) + " ";
            }
            text += std::to_string(value);
        }
        return text;
    }

    void WriteAmmoDescription(uint8_t* controller, void* node, void* ammoItemResource, const GGUUID& ammo)
    {
        std::string text = ResolveText(Texts(ammoItemResource, InventoryItemResourceUI::ItemDescription));
        const std::string stats = AmmoStatsText(controller, ammo);
        if (!stats.empty())
            text += "\n\n" + stats;
        Tree(controller).Str(node, "description", text.c_str());
    }
}

namespace menu
{
    bool CreateHooks(uintptr_t base)
    {
        auto resolve = [base](uintptr_t rva) { return reinterpret_cast<void*>(base + rva); };
        StringCtor = reinterpret_cast<StringCtor_t>(resolve(rva::StringCtor));
        StringRelease = reinterpret_cast<StringRelease_t>(resolve(rva::StringRelease));
        NodeChild = reinterpret_cast<NodeChild_t>(resolve(rva::NodeChild));
        NodeCount = reinterpret_cast<NodeCount_t>(resolve(rva::NodeCount));
        NodeRemove = reinterpret_cast<NodeRemove_t>(resolve(rva::NodeRemove));
        NodeSetBool = reinterpret_cast<NodeSetBool_t>(resolve(rva::NodeSetBool));
        NodeSetInt = reinterpret_cast<NodeSetInt_t>(resolve(rva::NodeSetInt));
        NodeSetObject = reinterpret_cast<NodeSetObject_t>(resolve(rva::NodeSetObject));
        NodeSetString = reinterpret_cast<NodeSetString_t>(resolve(rva::NodeSetString));
        NodeSetText = reinterpret_cast<NodeSetText_t>(resolve(rva::NodeSetText));
        NodeMarkDirty = reinterpret_cast<NodeMarkDirty_t>(resolve(rva::NodeMarkDirty));
        NodeItemInfo = reinterpret_cast<NodeItemInfo_t>(resolve(rva::NodeItemInfo));
        NodeItemRarity = reinterpret_cast<NodeItemRarity_t>(resolve(rva::NodeItemRarity));
        NodeItemOwned = reinterpret_cast<NodeItemOwned_t>(resolve(rva::NodeItemOwned));
        ItemIcon = reinterpret_cast<ItemIcon_t>(resolve(rva::ItemIcon));
        Alloc = reinterpret_cast<Alloc_t>(resolve(rva::Alloc));
        g_emptyString = static_cast<const char*>(resolve(rva::EmptyString));
        g_socketRtti = static_cast<uint8_t*>(resolve(rva::RTTI_ModificationSocketResource));
        g_inventoryItemRtti = static_cast<uint8_t*>(resolve(rva::RTTI_InventoryItem));
        WeakPtrLink = reinterpret_cast<WeakPtrOp_t>(resolve(rva::WeakPtrLink));
        WeakPtrUnlink = reinterpret_cast<WeakPtrOp_t>(resolve(rva::WeakPtrUnlink));
        AddEventHandler = reinterpret_cast<AddEventHandler_t>(resolve(rva::Tag_AddEventHandler));
        g_bindingRtti = static_cast<uint8_t*>(resolve(rva::RTTI_MenuInputEventBinding));
        g_textBindingRtti = static_cast<uint8_t*>(resolve(rva::RTTI_MenuLocalizedTextBinding));
        g_textRtti = static_cast<uint8_t*>(resolve(rva::RTTI_LocalizedTextResource));
        g_localizedTextBindingVtable = base + rva::VT_MenuLocalizedTextBinding;
        g_textTagSetText = resolve(rva::TextTag_SetText);
        g_inventoryUiSettings = static_cast<void**>(resolve(rva::InventoryUiSettingsPtr));

        bool ok = true;
        ok &= CreateHook(base, rva::MenuInputBinding_Matches, &hk_Matches, &o_Matches, "MenuInputEventBinding::Matches");
        ok &= CreateHook(base, rva::MenuGraphAction_Execute, &hk_GraphExecute, &o_GraphExecute, "MenuActionGraphProgram::Execute");
        ok &= CreateHook(base, rva::LocalizedText_GetString, &hk_GetString, &o_GetString, "LocalizedTextResource::GetString");
        ok &= CreateHook(base, rva::InvMenu_RefreshItemSelected, &hk_Refresh, &o_Refresh, "InventoryMenuController::RefreshItemSelected");
        ok &= CreateHook(base, rva::InvMenu_PopulateModsExport, &hk_PopulateModsExport, &o_PopulateModsExport, "PopulateModsForEditTool (export)");
        ok &= CreateHook(base, rva::InvMenu_PopulateMods, &hk_PopulateMods, &o_PopulateMods, "InventoryMenuController::PopulateModsForEditTool");
        ok &= CreateHook(base, rva::InvMenu_SelectMod, &hk_SelectMod, &o_SelectMod, "InventoryMenuController::SelectMod");
        ok &= CreateHook(base, rva::InvMenu_SelectFirstModSocket, &hk_SelectFirstSocket, &o_SelectFirstSocket, "InventoryMenuController::SelectFirstModSocket");
        ok &= CreateHook(base, rva::InvMenu_PreviewMod, &hk_PreviewMod, &o_PreviewMod, "InventoryMenuController::PreviewMod");
        ok &= CreateHook(base, rva::InvMenu_EquipSelectedMod, &hk_Equip, &o_Equip, "InventoryMenuController::EquipSelectedMod");
        ok &= CreateHook(base, rva::InvMenu_UnequipSelectedMod, &hk_Unequip, &o_Unequip, "InventoryMenuController::UnequipSelectedMod");
        ok &= CreateHook(base, rva::InvMenu_Destroy, &hk_Destroy, &o_Destroy, "InventoryMenuController::~InventoryMenuController");
        ok &= CreateHook(base, rva::InvMenu_PopulateWeaponsExport, &hk_PopulateWeaponsExport, &o_PopulateWeaponsExport, "PopulateWeapons (export)");
        ok &= CreateHook(base, rva::ItemPanel_WriteAmmo, &hk_WriteAmmoPanel, &o_WriteAmmoPanel, "item details ammo list");
        ok &= CreateHook(base, rva::ItemPanel_WriteAmmoEntry, &hk_WriteAmmoEntry, &o_WriteAmmoEntry, "item details ammo entry");
        ok &= CreateHook(base, rva::TextTag_BindText, &hk_BindText, &o_BindText, "TextTag text binding");
        ok &= CreateHook(base, rva::Tag_OnEvent, &hk_TagOnEvent, &o_TagOnEvent, "Tag::OnEvent");
        return ok;
    }
}
