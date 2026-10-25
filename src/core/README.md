# Core game-state records

Canonical byte-exact layouts of the 3.1c game records, shared by every
system. Headers compile as C11 and C++20 (namespace `oa` in C++); target
`oa-core-types` is header-only.

| Header | Owns |
|---|---|
| `types.h` | `oa_fixed` (16.16), `oa_angle` (65536/rev), `oa_ref32`, `FixedVec3`, `Rect32`, assert macros |
| `unit.h` | `Unit` (0x118), `UnitWeapon` (0x1c x3), `UnitEconomy`/`ResourceAccumulator`, unit flag bits |
| `player.h` | `Player` (0x14b), status values |
| `player_setup.h` | `PlayerSetupInfo` (0xb9, the per-player game-setup block `Player.info` refers to), its option and status bits |
| `unit_def.h` | `UnitDef` (0x249, FBI), unit-def flag and ability bits |
| `weapon_def.h` | `WeaponDef` (0x115, weapon TDF), weapon flag bits, the id's high byte |
| `feature_def.h`, `side.h`, `move_class.h` | `FeatureDef` (0x100), `FeatureDefCursor` (0xc x2), `Side` (0x232), `MoveClass` (0x20) |
| `projectile.h` | `Projectile` (0x6b), pool capacity |
| `map_plot.h` | `MapPlot` (0xd terrain cell), plot flags |
| `cob.h` | `CobScriptHeader`, `CobThread` (0xa4), `CobMachine` (0x544) |
| `game_state.h` | `Game` (0x3924d): players, weapon defs, sides, score table inline; tables by reference |
| `world.h` | `World`: the live `Game` plus its tables, including weapon slots past the game state's table; lifetime (`world_create`, `world_alloc_tables`, ...) and reference helpers |

Conventions:

- Records are `#pragma pack(1)` byte-exact layouts of the 3.1c records. Every
  named field has an `offsetof` assert and every record a `sizeof` assert.
- Every field is named by what it holds: by the code that uses it, by marked
  evidence (`?` at the start of its comment marks a tentative name), or, for
  bytes the engine only carries, by their role and position
  (`block_after_tile_map`). A byte array that code reads by part is split
  into typed, pinned fields at the same offsets.
- `World` owns live state; systems are free functions over `World*`. It holds
  host pointers and has no 3.1c layout. `world_alloc_tables` sizes the
  unit (`units_per_player * 10 + 1`, slot 0 reserved), type (index 0 reserved),
  feature and projectile tables, and weapon slots past the game state's table
  when the capacity asks for them; the map loader owns `plots`, `sight_grid`
  and `placed_features`. Tables stay at stable addresses while referenced.
- An `oa_ref32` that refers to an element of a World table stores the element
  index + 1; 0 is null. Use the `world_*` helpers (`world_unit`,
  `world_unit_ref`, `world_unit_def_of`, `world_unit_owner`,
  `world_player_units`, ...) rather than decoding by hand. Unit links convert
  to slot indices with `oa_unit_slot_from_ref` / `oa_unit_ref_from_slot` (0
  stays "none").
- Objects with no 3.1c layout yet (order queues, movement and script
  objects, model instances, path jobs) live in side tables indexed by unit slot,
  declared by the module that owns them, never in core. The matching `Unit`
  field (`movement`, `script`) is only a nonzero presence flag.
- Never add per-module copies of unit, player or world records. Transitional
  views of the removed copies live in
  `src/sim/unit-spawn/include/oa/sim/unit_spawn/legacy_views.hpp`.
- Flag words get one named constant per bit (`OA_UNIT_FLAG_*`,
  `OA_UNIT_DEF_FLAG_*`, `OA_UNIT_DEF_ABILITY_*`, `OA_WEAPON_FLAG_*`,
  `OA_FEATURE_FLAG_*`), each named by what the bit does; names never carry a
  record offset or an address.
- `core-types-*` and `core-world-*` compile the headers in both languages.
