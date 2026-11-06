#include "bonus.h"

#include "ammo.h"
#include "hookutil.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>
#include <optional>
#include <utility>

using namespace game;

// A weapon's upgrades raise the damage and build-up of the damage types of the ammo it comes with: the
// Sun Scourge's raise fire, frost and acid, not shock. Swapped-in ammo of a damage type the weapon has no
// bonus for would do only its base damage and build-up. For each such damage type the mod gives the
// weapon one more bonus per stack group, made of the weapon's bonuses for its own ammo: the highest of
// each amount (damage and build-up multipliers and additions) among them. Its base stats and its upgrade
// level each get theirs, like the bonuses they are made of, and they add up the same way.
//
// The bonuses go wherever the game uses the weapon's own: to the wielder's combat stats while the weapon is
// wielded (added and removed with the game's), and into the bonus lists the item details and the HUD work
// out ammo damage from. Damage types of the ammo the weapon comes with keep the game's numbers.

namespace
{
    using GetComponent_t = void* (*)(void* container, void* rtti);
    using GetSettings_t = void* (*)(void* itemResource);
    using FilterMatches_t = bool (*)(void* filter, void* damageType);
    using BonusOp_t = void (*)(void* combatStats, void* bonus, const GGUUID* key, uint16_t activation);
    using Alloc_t = void* (*)(size_t);
    using ComponentFn_t = uint64_t (*)(void* component);
    using PrepareForRemoval_t = uint64_t (*)(void* combatStats, void* message);
    using CollectItemBonuses_t = uint64_t (*)(void* collection, void* item, int level, void* addedCoil, void* removedCoil);

    GetComponent_t GetComponent;
    GetSettings_t GetUpgradableWeaponSettings;
    GetSettings_t GetAmmoSettings;
    FilterMatches_t FilterMatches;
    BonusOp_t AddBonus;
    BonusOp_t RemoveBonus;
    Alloc_t Alloc;

    void* g_combatStatsRtti;
    void* g_bonusRtti;
    void* g_definitionRtti;
    void* g_valueRtti;
    void* g_filterRtti;
    void* g_equipmentStatsRtti;

    ComponentFn_t o_EquipmentStatsApply;
    ComponentFn_t o_EquipmentStatsRemove;
    ComponentFn_t o_UpgradeApply;
    ComponentFn_t o_UpgradeRemove;
    PrepareForRemoval_t o_PrepareForRemoval;
    CollectItemBonuses_t o_CollectItemBonuses;

    thread_local const std::vector<GGUUID>* t_previewAmmo;

    constexpr int kAmounts = CombatStatsDamageValue::AmountCount;

    // ---- the weapon's bonuses -------------------------------------------------------------------------
    // Filter of a bonus to the damage dealt with the damage types an include list names, without further
    // conditions (what weapons have for their ammo); null for any other bonus.
    void* PlainFilter(void* bonus)
    {
        if (!bonus || RttiOf(bonus) != g_bonusRtti)
            return nullptr;
        void* definition = At<void*>(bonus, CombatStatsBonus::Definition);
        void* value = At<void*>(bonus, CombatStatsBonus::Value);
        if (!definition || !value || RttiOf(definition) != g_definitionRtti || RttiOf(value) != g_valueRtti)
            return nullptr;
        if (At<void*>(definition, CombatStatsDefinition::ConditionOnAffectedEntity) ||
            At<void*>(definition, CombatStatsDefinition::ConditionOnOwner) ||
            At<void*>(definition, CombatStatsDefinition::CombatEventConditionFilter))
            return nullptr;
        void* filter = At<void*>(definition, CombatStatsDefinition::DamageTypeFilter);
        return filter && At<uint8_t>(filter, DamageTypeFilter::FilterType) == DamageTypeFilter::Include ? filter : nullptr;
    }

    // Bonuses of the weapon's base stats.
    const Array<void*>* BaseBonuses(void* resource)
    {
        for (void* settings : At<Array<void*>>(resource, InventoryItemResource::Settings))
        {
            if (settings && RttiOf(settings) == g_equipmentStatsRtti)
                return &At<Array<void*>>(settings, EquipmentStatsSettings::Bonuses);
        }
        return nullptr;
    }

    // Bonuses the weapon has at an upgrade level (they replace those of lower levels).
    const Array<void*>* UpgradeBonuses(void* resource, int level)
    {
        void* upgrades = level >= 0 ? GetUpgradableWeaponSettings(resource) : nullptr;
        if (!upgrades)
            return nullptr;
        const auto& path = At<Array<void*>>(upgrades, UpgradableWeaponSettings::UpgradePath);
        if (level >= path.count || !path.data[level])
            return nullptr;
        return &At<Array<void*>>(path.data[level], WeaponUpgradeDefinition::NewBonusResources);
    }

    bool Raises(const Array<void*>* bonuses, void* type)
    {
        if (!bonuses)
            return false;
        return std::any_of(bonuses->begin(), bonuses->end(), [type](void* bonus) {
            void* filter = PlainFilter(bonus);
            return filter && FilterMatches(filter, type);
        });
    }

    // Damage types an ammo's stats list (one per damage line).
    void AddDamageTypes(void* weapon, const GGUUID& ammo, std::vector<void*>& types)
    {
        void* item = ammo.IsNull() ? nullptr : ammo::SupportedAmmoItem(weapon, ammo);
        void* settings = item ? GetAmmoSettings(item) : nullptr;
        if (!settings)
            return;
        for (void* display : At<Array<void*>>(settings, AmmoSettings::DisplayStats))
        {
            void* damage = display ? At<void*>(display, CombatStatsDamageDisplayResource::BaseDamageSettings) : nullptr;
            void* type = damage ? At<void*>(damage, DamageSettings::DamageType) : nullptr;
            if (type && std::find(types.begin(), types.end(), type) == types.end())
                types.push_back(type);
        }
    }

    // ---- made bonuses ---------------------------------------------------------------------------------
    // The highest amounts of a weapon's bonuses of one stack group.
    struct Pool
    {
        void* definition;  // of the pool's first bonus: who it affects, when it applies
        void* stackGroup;
        void* stackGroupModifier;
        float amounts[kAmounts];
    };

    // Made objects are never freed: combat stats and attacks in flight may still point at them. Equal
    // inputs give the same objects, so there are only as many as damage type and amount combinations used.
    struct MadeDefinition
    {
        void* source;
        void* type;
        void* definition;
    };
    struct MadeBonus
    {
        void* definition;
        void* stackGroup;
        void* stackGroupModifier;
        float amounts[kAmounts];
        void* bonus;
    };

    constexpr uint64_t kBonusUuidHi = 0x736E6F4200000000ull;  // "Bons" + count: second half of made objects' UUIDs

    std::mutex g_madeLock;
    std::vector<std::pair<void*, void*>> g_madeFilters;  // damage type -> filter for it
    std::vector<MadeDefinition> g_madeDefinitions;
    std::vector<MadeBonus> g_madeBonuses;
    uint32_t g_madeCount;

    void* NewObject(void* rtti)
    {
        const uint32_t size = At<uint32_t>(rtti, RTTI::Size);
        void* object = Alloc(size);
        if (!object)
            return nullptr;
        memset(object, 0, size);
        At<void (*)(void*, void*)>(rtti, RTTI::Constructor)(rtti, object);
        ObjectUUID(object) = GGUUID{kModUuidLo, kBonusUuidHi | ++g_madeCount};
        At<int32_t>(object, RefObject::RefCount) = 1;  // owned by the mod for good
        return object;
    }

    void* FilterFor(void* type)
    {
        for (const auto& [madeType, filter] : g_madeFilters)
        {
            if (madeType == type)
                return filter;
        }
        void* filter = NewObject(g_filterRtti);
        auto* types = filter ? static_cast<void**>(Alloc(sizeof(void*))) : nullptr;
        if (!types)
            return nullptr;
        types[0] = type;
        At<Array<void*>>(filter, DamageTypeFilter::DamageTypes) = {1, 1, types};  // filter type: include
        g_madeFilters.emplace_back(type, filter);
        return filter;
    }

    // Fields of a damage dealt definition past the RTTIRefObject header, without its filter and texts.
    constexpr size_t kFields = 32;
    constexpr size_t kTexts = 72;     // DamageStatDescription, SeverityStatDescription, SameValueStatDescription
    constexpr size_t kTextsEnd = 96;

    bool SameFields(void* made, void* source, size_t size)
    {
        const auto* a = static_cast<const uint8_t*>(made);
        const auto* b = static_cast<const uint8_t*>(source);
        return memcmp(a + kFields, b + kFields, CombatStatsDefinition::DamageTypeFilter - kFields) == 0 &&
               memcmp(a + kTextsEnd, b + kTextsEnd, size - kTextsEnd) == 0;
    }

    // `source` (a definition of the weapon's) for `type` alone. The texts naming the damage type are left out.
    void* DefinitionFor(void* source, void* type)
    {
        const uint32_t size = At<uint32_t>(g_definitionRtti, RTTI::Size);
        for (const MadeDefinition& made : g_madeDefinitions)
        {
            if (made.source == source && made.type == type && SameFields(made.definition, source, size))
                return made.definition;
        }
        void* filter = FilterFor(type);
        void* definition = filter ? NewObject(g_definitionRtti) : nullptr;
        if (!definition)
            return nullptr;
        auto* fields = static_cast<uint8_t*>(definition);
        memcpy(fields + kFields, static_cast<uint8_t*>(source) + kFields, size - kFields);
        memset(fields + kTexts, 0, kTextsEnd - kTexts);
        At<void*>(definition, CombatStatsDefinition::DamageTypeFilter) = filter;
        g_madeDefinitions.push_back({source, type, definition});
        return definition;
    }

    void* BonusFor(const Pool& pool, void* type)
    {
        std::lock_guard lock(g_madeLock);
        void* definition = DefinitionFor(pool.definition, type);
        if (!definition)
            return nullptr;
        for (const MadeBonus& made : g_madeBonuses)
        {
            if (made.definition == definition && made.stackGroup == pool.stackGroup && made.stackGroupModifier == pool.stackGroupModifier &&
                memcmp(made.amounts, pool.amounts, sizeof(pool.amounts)) == 0)
                return made.bonus;
        }
        void* value = NewObject(g_valueRtti);
        void* bonus = value ? NewObject(g_bonusRtti) : nullptr;
        if (!bonus)
        {
            LOG("Out of memory making a damage bonus");
            return nullptr;
        }
        At<void*>(value, CombatStatsDamageValue::StackGroup) = pool.stackGroup;
        At<void*>(value, CombatStatsDamageValue::StackGroupModifier) = pool.stackGroupModifier;
        memcpy(&At<float>(value, CombatStatsDamageValue::Amounts), pool.amounts, sizeof(pool.amounts));
        At<void*>(bonus, CombatStatsBonus::Definition) = definition;
        At<void*>(bonus, CombatStatsBonus::Value) = value;
        MadeBonus made{definition, pool.stackGroup, pool.stackGroupModifier, {}, bonus};
        std::copy(std::begin(pool.amounts), std::end(pool.amounts), made.amounts);
        g_madeBonuses.push_back(made);
        return bonus;
    }

    // Bonuses the mod adds to one of the weapon's bonus lists (its base stats, or its upgrade level
    // `level`) for the damage types of `held` ammo that neither its own ammo has nor any of its bonuses raises.
    std::vector<void*> ExtraBonuses(void* weapon, int level, bool upgrade, const std::vector<GGUUID>& held)
    {
        std::vector<void*> result;
        void* resource = ammo::ItemResource(weapon);
        if (!resource || !ammo::HasSwappableAmmo(resource))
            return result;
        const Array<void*>* base = BaseBonuses(resource);
        const Array<void*>* levelBonuses = UpgradeBonuses(resource, level);
        const Array<void*>* list = upgrade ? levelBonuses : base;
        if (!list || list->count == 0)
            return result;

        std::vector<void*> own;
        for (const GGUUID& id : ammo::DefaultAmmo(weapon))
            AddDamageTypes(weapon, id, own);
        std::vector<void*> types;
        for (const GGUUID& id : held)
            AddDamageTypes(weapon, id, types);
        std::erase_if(types, [&](void* type) {
            return std::find(own.begin(), own.end(), type) != own.end() || Raises(base, type) || Raises(levelBonuses, type);
        });
        if (types.empty())
            return result;

        std::vector<Pool> pools;
        for (void* bonus : *list)
        {
            void* filter = PlainFilter(bonus);
            if (!filter || std::none_of(own.begin(), own.end(), [filter](void* type) { return FilterMatches(filter, type); }))
                continue;
            void* value = At<void*>(bonus, CombatStatsBonus::Value);
            void* group = At<void*>(value, CombatStatsDamageValue::StackGroup);
            void* modifier = At<void*>(value, CombatStatsDamageValue::StackGroupModifier);
            const float* amounts = &At<float>(value, CombatStatsDamageValue::Amounts);
            auto pool = std::find_if(pools.begin(), pools.end(), [&](const Pool& p) { return p.stackGroup == group && p.stackGroupModifier == modifier; });
            if (pool == pools.end())
            {
                Pool added{At<void*>(bonus, CombatStatsBonus::Definition), group, modifier, {}};
                std::copy(amounts, amounts + kAmounts, added.amounts);
                pools.push_back(added);
                continue;
            }
            for (int i = 0; i < kAmounts; i++)
                pool->amounts[i] = std::max(pool->amounts[i], amounts[i]);
        }

        for (void* type : types)
        {
            for (const Pool& pool : pools)
            {
                if (void* bonus = BonusFor(pool, type))
                    result.push_back(bonus);
            }
        }
        return result;
    }

    // ---- the wielder's combat stats -------------------------------------------------------------------
    // Bonuses the mod added next to those a weapon component added to the wielder's combat stats. Every
    // component of a swappable weapon whose bonuses are on has one, with or without bonuses, so Refresh
    // can add some after a swap.
    struct Applied
    {
        void* component;  // EquipmentStatsComponent (base stats) or ItemUpgradeComponent (upgrade level)
        GGUUID componentId;
        void* weapon;
        void* combatStats;
        bool upgrade;
        int level;
        uint16_t activation;
        std::vector<std::pair<void*, GGUUID>> bonuses;  // with the keys they were added under
    };

    // Entries live from a component's apply to its remove. The combat stats they point at outlive them:
    // entries are dropped when those are removed.
    std::mutex g_appliedLock;
    std::vector<Applied> g_applied;

    void AddExtras(Applied& applied)
    {
        for (void* bonus : ExtraBonuses(applied.weapon, applied.level, applied.upgrade, ammo::GetAssigned(applied.weapon, nullptr)))
        {
            // Unique per component and bonus, like the keys the game adds a component's bonuses under.
            const GGUUID& id = ObjectUUID(bonus);
            const GGUUID key{applied.componentId.lo ^ id.lo, applied.componentId.hi ^ id.hi};
            AddBonus(applied.combatStats, bonus, &key, applied.activation);
            applied.bonuses.emplace_back(bonus, key);
        }
    }

    void RemoveExtras(Applied& applied)
    {
        for (const auto& [bonus, key] : applied.bonuses)
            RemoveBonus(applied.combatStats, bonus, &key, applied.activation);
        applied.bonuses.clear();
    }

    std::optional<Applied> Take(void* component)
    {
        std::lock_guard lock(g_appliedLock);
        const auto it = std::find_if(g_applied.begin(), g_applied.end(), [component](const Applied& a) { return a.component == component; });
        if (it == g_applied.end())
            return std::nullopt;
        Applied applied = std::move(*it);
        g_applied.erase(it);
        return applied;
    }

    void OnApply(void* component, bool upgrade, uint16_t activation)
    {
        if (std::optional<Applied> previous = Take(component))
            RemoveExtras(*previous);  // applied again without being removed

        void* weapon = ammo::ItemOfComponent(component);
        if (!weapon || !ammo::HasSwappableAmmo(ammo::ItemResource(weapon)))
            return;
        void* entity = At<void*>(component, EntityComponent::Entity);
        void* wielder = At<void*>(entity, Entity::Owner);
        void* combatStats = wielder ? GetComponent(static_cast<uint8_t*>(wielder) + Entity::Components, g_combatStatsRtti) : nullptr;
        if (!combatStats)
            return;

        const int level = upgrade ? At<int>(component, ItemUpgradeComponent::Level) : ammo::UpgradeLevel(weapon);
        Applied applied{component, ObjectUUID(component), weapon, combatStats, upgrade, level, activation, {}};
        AddExtras(applied);
        std::lock_guard lock(g_appliedLock);
        g_applied.push_back(std::move(applied));
    }

    void OnRemove(void* component)
    {
        if (std::optional<Applied> applied = Take(component))
            RemoveExtras(*applied);
    }

    uint64_t hk_EquipmentStatsApply(void* component)
    {
        const uint64_t result = o_EquipmentStatsApply(component);
        OnApply(component, false, At<uint16_t>(component, EquipmentStatsComponent::Activation));
        return result;
    }

    uint64_t hk_EquipmentStatsRemove(void* component)
    {
        OnRemove(component);
        return o_EquipmentStatsRemove(component);
    }

    uint64_t hk_UpgradeApply(void* component)
    {
        const uint64_t result = o_UpgradeApply(component);
        OnApply(component, true, At<uint16_t>(component, ItemUpgradeComponent::Activation));
        return result;
    }

    uint64_t hk_UpgradeRemove(void* component)
    {
        OnRemove(component);
        return o_UpgradeRemove(component);
    }

    uint64_t hk_PrepareForRemoval(void* combatStats, void* message)
    {
        std::vector<Applied> gone;
        {
            std::lock_guard lock(g_appliedLock);
            const auto first = std::stable_partition(g_applied.begin(), g_applied.end(), [combatStats](const Applied& a) { return a.combatStats != combatStats; });
            std::move(first, g_applied.end(), std::back_inserter(gone));
            g_applied.erase(first, g_applied.end());
        }
        for (Applied& applied : gone)
            RemoveExtras(applied);
        return o_PrepareForRemoval(combatStats, message);
    }

    // ---- item details and HUD -------------------------------------------------------------------------
    uint64_t hk_CollectItemBonuses(void* collection, void* item, int level, void* addedCoil, void* removedCoil)
    {
        const uint64_t result = o_CollectItemBonuses(collection, item, level, addedCoil, removedCoil);
        if (!item || !ammo::HasSwappableAmmo(ammo::ItemResource(item)))
            return result;
        const std::vector<GGUUID> held = t_previewAmmo ? *t_previewAmmo : ammo::GetAssigned(item, nullptr);
        using AddToCollection_t = void (*)(void* bonus, void* collection);
        for (const bool upgrade : {false, true})
        {
            for (void* bonus : ExtraBonuses(item, level, upgrade, held))
                (*static_cast<AddToCollection_t**>(bonus))[CombatStatsBonus::AddToCollection](bonus, collection);
        }
        return result;
    }
}

namespace bonus
{
    bool CreateHooks(uintptr_t base)
    {
        auto resolve = [base](uintptr_t rva) { return reinterpret_cast<void*>(base + rva); };
        GetComponent = reinterpret_cast<GetComponent_t>(resolve(rva::GetComponent));
        GetUpgradableWeaponSettings = reinterpret_cast<GetSettings_t>(resolve(rva::GetUpgradableWeaponSettings));
        GetAmmoSettings = reinterpret_cast<GetSettings_t>(resolve(rva::GetAmmoSettings));
        FilterMatches = reinterpret_cast<FilterMatches_t>(resolve(rva::DamageTypeFilter_Matches));
        AddBonus = reinterpret_cast<BonusOp_t>(resolve(rva::CombatStats_AddBonus));
        RemoveBonus = reinterpret_cast<BonusOp_t>(resolve(rva::CombatStats_RemoveBonus));
        Alloc = reinterpret_cast<Alloc_t>(resolve(rva::Alloc));
        g_combatStatsRtti = resolve(rva::RTTI_CombatStatsComponent);
        g_bonusRtti = resolve(rva::RTTI_CombatStatsDamageDealtBonusResource);
        g_definitionRtti = resolve(rva::RTTI_CombatStatsDamageDealtDefinition);
        g_valueRtti = resolve(rva::RTTI_CombatStatsDamageValue);
        g_filterRtti = resolve(rva::RTTI_DamageTypeFilter);
        g_equipmentStatsRtti = resolve(rva::RTTI_EquipmentStatsSettings);

        bool ok = true;
        ok &= CreateHook(base, rva::EquipmentStats_Apply, &hk_EquipmentStatsApply, &o_EquipmentStatsApply, "EquipmentStatsComponent::ApplyBonuses");
        ok &= CreateHook(base, rva::EquipmentStats_Remove, &hk_EquipmentStatsRemove, &o_EquipmentStatsRemove, "EquipmentStatsComponent::RemoveBonuses");
        ok &= CreateHook(base, rva::ItemUpgrade_Apply, &hk_UpgradeApply, &o_UpgradeApply, "ItemUpgradeComponent::ApplyBonuses");
        ok &= CreateHook(base, rva::ItemUpgrade_Remove, &hk_UpgradeRemove, &o_UpgradeRemove, "ItemUpgradeComponent::RemoveBonuses");
        ok &= CreateHook(base, rva::CombatStats_PrepareForRemoval, &hk_PrepareForRemoval, &o_PrepareForRemoval, "CombatStatsComponent::MsgPrepareForRemoval");
        ok &= CreateHook(base, rva::CollectItemBonuses, &hk_CollectItemBonuses, &o_CollectItemBonuses, "CollectItemBonuses");
        return ok;
    }

    void Refresh(void* weapon)
    {
        std::vector<Applied> entries;
        {
            std::lock_guard lock(g_appliedLock);
            const auto first = std::stable_partition(g_applied.begin(), g_applied.end(), [weapon](const Applied& a) { return a.weapon != weapon; });
            std::move(first, g_applied.end(), std::back_inserter(entries));
            g_applied.erase(first, g_applied.end());
        }
        size_t count = 0;
        for (Applied& applied : entries)
        {
            RemoveExtras(applied);
            AddExtras(applied);
            count += applied.bonuses.size();
        }
        if (!entries.empty())
            LOG("Damage bonuses for ammo the weapon doesn't come with: %zu", count);
        std::lock_guard lock(g_appliedLock);
        std::move(entries.begin(), entries.end(), std::back_inserter(g_applied));
    }

    PreviewAmmo::PreviewAmmo(const std::vector<GGUUID>& ammo) : m_outer(t_previewAmmo) { t_previewAmmo = &ammo; }

    PreviewAmmo::~PreviewAmmo() { t_previewAmmo = m_outer; }
}
