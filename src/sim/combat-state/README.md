# Combat state

Weapon definitions, a unit's three weapon slots, automatic targeting, attack
orders and the strength estimates computer players build from unit types.

`WeaponRegistry` holds one weapon record per slot, from 0 to the table's
last, indexed by each `Weapons/*.tdf` section's `ID`. A new registry holds
256. The weapon files are read once, by
`src/data/defs` (`load_weapon_defs`), into the `WeaponDef` table the game state
holds; `install_weapon_table` then installs each loaded slot, keeping its
`WeaponDef` record for `Game.weapon_defs` and the explosion and sound names its
section gave. `install_weapon_files` and `install_weapon_text` do both steps,
from the game's files or from TDF text. Unknown or empty weapon names fall back
to record zero, as the game does, while `bind_unit_weapons` still reports
whether any name resolved to a real weapon; that result sets
`OA_UNIT_DEF_FLAG_HAS_WEAPONS` in `UnitDef.flags`.

Call `initialize_spawn_combat` for a new unit with FBI weapon1/2/3 and a
`SpawnGeometryHost` that runs QueryWeapon/AimFrom and converts the pieces to
world coordinates. It arms the three slots (stockpile cleared, slot index and
enabled bit set, muzzle offset from the two pieces) and passes the slowest
reload, in milliseconds, to the script's SetMaxReloadTime.
`initialize_weapon_slots` does the same from positions already known. The
reload countdown is kept only in the canonical `UnitWeapon.reload`, which the
caller clears when it arms the unit.

`select_automatic_target` draws at most 50 random candidates from the units the
owner knows of nearby, filters them by targetability, owner and type flags,
weapon reach and the category masks, and keeps the game's two-tier choice: the
nearest candidate outside the weapon's preferred-category mask, else the nearest
inside it. `gather_sightings` answers the candidate query from a player's seen
and radar lists (kept by `src/sim/detection`), searching the radar list only
while nothing has been found. `issue_attack_order` validates an attack and
commits its one or two orders through `AttackHost` all at once; a failed commit
is reported as failure.

`strategic_refresh` rebuilds the per-type class and three strength bytes a
computer player uses to pick what to build. It keeps the game's wrapped byte
outputs, owned-count multipliers, map gates and resource clamps. `classify_type`,
`threat_score` and `energy_rate` are its building blocks.

`accelerate_projectile`, `turn_toward_angle` and `accuracy_spread` are the small
per-tick rules of self-propelled shots, turret aim and shot spread. The library
also builds the ballistic helpers of `src/sim/ballistics` (launch pitch, weapon reach
and turret aim).

Build independently with:

```
cmake -S src/sim/combat-state -B local/build-combat-state
cmake --build local/build-combat-state
ctest --test-dir local/build-combat-state --output-on-failure
```
