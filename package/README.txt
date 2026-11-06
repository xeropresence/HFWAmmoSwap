HFW AMMO SWAP
=============

Swap ammo types between weapons of the same type, the same way you swap coils.

In the inventory, select a ranged weapon that can use more than one ammo type (bows, slings,
tripcasters, spike throwers, boltblasters and so on). Next to "Edit Coils" there is now an
"Edit Ammo" prompt. It opens the coil editing screen for the weapon's ammo:

- The slot row shows the weapon's ammo slots (as many as it has at its upgrade level).
- The list shows every ammo type that weapon can use and that one of your weapons has or comes
  with. Using an ammo type copies it from that weapon; the other weapon keeps it.
- Ammo already on the weapon is greyed out. Each ammo type can only be in one slot of a weapon.
- Pick a slot, then pick an ammo type to put it in that slot.
- "Remove Ammo" empties a slot, so the weapon cycles through fewer ammo types. The other ammo
  moves up; empty slots are always the last ones. A weapon keeps at least one ammo type.
- "Restore Default Ammo" (Triangle, F on keyboard) on the slot row gives the weapon back the ammo it
  comes with.
- Ammo descriptions list the damage that ammo does with the weapon you are editing (the same
  numbers as its Ammo panel: the ammo's base damage with this weapon's upgrades and coils).
- A weapon's upgrades only raise the damage types of the ammo it comes with. Ammo of another damage
  type gets the weapon's highest damage and build-up bonuses for its own ammo instead, in combat and
  in the Ammo panel. On the Sun Scourge, for example, Advanced Shock Hunter Arrows build up shock
  like its acid arrows build up acid, instead of with their bare base value.

The changes are part of your save.


REQUIREMENTS
------------

- Horizon Forbidden West Complete Edition (made on 1.5.80.0, Steam).
- Native Loader: https://www.nexusmods.com/horizonforbiddenwest/mods/223


INSTALL
-------

1. Install Native Loader (its winmm.dll goes next to HorizonForbiddenWest.exe).
2. Copy the HFWAmmoSwap folder into <game folder>\nativemods\, so the DLL ends up at
   <game folder>\nativemods\HFWAmmoSwap\HFWAmmoSwap.dll


UNINSTALL
---------

Before removing the mod, use "Restore Default Ammo" on every weapon you changed and save. A save
with swapped ammo relies on the mod to load that ammo for the weapon.

Then delete the nativemods\HFWAmmoSwap folder.


TROUBLESHOOTING
---------------

The mod writes HFWAmmoSwap.log next to its DLL. At startup it looks for the game code it uses; on
a game version where it can't find all of it, it disables itself and says so in the log.
