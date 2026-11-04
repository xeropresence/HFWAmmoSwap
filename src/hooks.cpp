#include "hooks.h"

#include "ammo.h"
#include "game.h"
#include "hookutil.h"
#include "log.h"
#include "menu.h"

#include <MinHook.h>

using namespace game;

namespace
{
    // Second argument is a completion callback functor {fn, context} the engine passes through.
    using LoadDependencies_t = void (*)(void* item, void* onLoaded);
    using CollectRefs_t = void (*)(void* settings, Array<StreamingHandle*>* out);

    LoadDependencies_t o_LoadDependencies;
    CollectRefs_t o_CollectRefs;

    void hk_LoadDependencies(void* item, void* onLoaded)
    {
        ammo::OnLoadItemDependenciesBegin(item);
        o_LoadDependencies(item, onLoaded);
        ammo::OnLoadItemDependenciesEnd();
    }

    void hk_CollectRefs(void* settings, Array<StreamingHandle*>* out)
    {
        o_CollectRefs(settings, out);
        ammo::OnCollectAmmoRefs(settings, out);
    }
}

namespace hooks
{
    bool Install(uintptr_t base)
    {
        if (MH_Initialize() != MH_OK)
            return false;

        bool ok = true;
        ok &= CreateHook(base, rva::InventoryItem_LoadDependencies, &hk_LoadDependencies, &o_LoadDependencies, "InventoryItem::LoadDependencies");
        ok &= CreateHook(base, rva::AmmoSlotsSettings_CollectRefs, &hk_CollectRefs, &o_CollectRefs, "AmmoSlotsSettings::CollectRefs");
        ok &= menu::CreateHooks(base);
        if (!ok)
            return false;
        return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    }
}
