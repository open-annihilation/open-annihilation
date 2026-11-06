# Unit creation and commander placement

`oa-sim-unit-spawn` covers unit pool selection, numeric initialization, the order in
which models and COB scripts are attached, and single-player commander
placement. It works on the canonical `oa::World` (`src/core`) plus native
`Tables`: runtime `Type`s (indexed like `World.unit_defs`, copied in with
`load_unit_def`), per-slot `SlotAssets` and per-player `PlayerSetupState`. It
does not create a substitute RTS engine or pretend unresolved model, script or
terrain callbacks have run.

`legacy_views.hpp` keeps the removed `Slot`, `PlayerRange`, `World` and
simulation-state `Unit`/`Player`/`World` names as zero-state reference views over
the World for code that has not migrated; `LegacyWorld` builds one for tests.

Entry points:

* `start_position`: start-marker lookup; kind 1 and matching index, first match
  wins, signed integer x/z converted to 16.16, y zero.
* `spawn_player_commander`: player setup, start storage floor, commander lookup,
  spawn and local camera positioning.
* `create`: slot choice, `initialize_numeric`, `attach_model_script`, weapons,
  extraction rate, the movement object and the creation notifications.
* `record_slot_death`: the tick from which a dead unit's place may be taken
  again, when a mod's rules delay slot reuse.

`Tables::rules` (`SpawnRules`) carries the unit rules a mod's profile sets, all
3.1c's by default: the slots' reuse ticks (units.id-reuse-delay) and the sea
occupy code of a unit created under the sea (units.water-state-rules). A
`Request` may name a building's facing (units.build-rotation), which swaps its
footprint east and west and joins its heading.
* `attached_child_count`: count of the children on the unit's
  `attach_first_child`/`attach_next` chain whose `attach_parent` is the
  queried unit.
* `average_plot_height`: signed high words of a 16.16 position are divided by 16
  toward zero to select a plot. The result is the arithmetic mean of that plot's
  low and high height bytes, or -1 when the cell has no plot. Y is unread.
* `player_pay_energy`/`player_pay_metal`/`player_pay_resources`: debits of the
  `Player.energy` and `Player.metal` stores; for both, each store must cover
  its cost before either is debited, and the unit's requested words are
  written by the caller.

Populate world type index zero as reserved. Load runtime types from FBI fields,
then supply enabled/limit state and cache-owned model and COB handles. Player
ranges select slices of the global slot array; slot zero is permitted by the
range, but requested-slot zero means search. Assign each bound unit's owner
before creating it. Retain type, unit and slot storage addresses while using
callbacks. Constructor callbacks receive real state, and all unresolved work is
mandatory through `Host`.

For a scripted model the attachment order is VM allocation/init, COB load,
model+COB instance creation, bind, script `Create`, and a reset of the model
instance's cached image word (`cached_image_word`).
A nonscripted model gets a plain instance and owner linkage. The component
stores handles but does not claim the model-instance or script-VM work is
implemented by those interface calls.

Commander placement is one part of starting a skirmish, which also covers
player synchronization, scenario loading, side loading, team and start
randomization, mission unit placement and interface setup; only the individual
player creation is here. Map marker parsing and side commander-name resolution
remain integration inputs.

`unit_pool_size(limit)` and `init_unit_pool(world, limit)` lay out the
single-player unit pool before `spawn_player_commander`: reserve slot 0, allocate
ten equal owner ranges, initialize indices and type-zero linkage, and set each
player's first/last unit references. Tables must have exactly the returned size;
a limit whose pool would pass 65535 slots sizes none, and `init_unit_pool`
returns false for it.
Multiplayer ordering and the auxiliary hot-unit and radar caches stay outside
this helper.

Enhanced Mode uses the overloads with a participating-player mask. Only those
players receive ranges, in ascending player-slot order; absent players and
observers have empty ranges. Player ownership keeps the original slot index.
Two players can each own 10,000 units in a pool of 20,001 records including
reserved slot zero, without changing packed records. The pool refuses ids
above 32767, which the existing weapon targets and carrier messages cannot
represent. `unit-capacity` creates all 20,000 units, checks sparse player slots,
range boundaries and overflow, and pins the classic pool bounds.

## Parsed asset adapter

Target `oa-sim-unit-spawn-runtime`, header `oa/sim/unit_spawn/spawn_runtime.hpp`, connects a unit type's
fields to the spawn type. `load_runtime_type` loads the actual 3DO and optional
COB, stores shared ownership of both parsed assets, and puts their stable native
addresses in the model/COB handles. Preserve the returned `LoadedType` while
using its copied `Type` in the world. A type that does not load holds a
`load_error` saying why, and no model, script or type fields. These handles are parsed assets, not
fabricated model instances or running VMs; constructor Hosts still perform that
work.

A unit whose script file is absent has no script: it is built and drawn from
its plain model, and runs no script. A script file the COB parser cannot
read, such as one whose header points past its end, is treated the same way:
the type still loads, with no script, and its `script_error` says why. The
game writes that once to its log as it loads the unit types, and the other
types load as before.

The caller supplies availability, per-player limit, resolved default-mission
index and movement-class footprint, which it takes from the movement classes
the game data loaded (the unit's runtime metadata); a named class that is
absent leaves the FBI dimensions. GUI pages are counted from the nonempty GUI files present, as
3.1c counts them; the count overwrites the FBI `makesmetal` byte that
`UnitDef.gui_page_count` first holds.

The required `AssetReader` returns null only for an absent resource; decoding,
archive corruption and I/O failures must propagate. The adapter keeps model names
apart from unit script names and uses the game's base paths. Mod-prefix
resolution is the reader's responsibility. `find_type_index` looks names up
case-insensitively, as the game does, without requiring caller storage to be
sorted.
