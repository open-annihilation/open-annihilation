# Brains

A computer seat played by a sandboxed Lua brain. The app reads the package
and hands its bytes here. The seat copies them, checks every network, and
calls `fits` and `start` before the match's first tick. A seat with no
brain plays as the built-in computer player.

## Entry points

- `seat_brain` loads one brain for one computer seat. On success the seat's
  hooks are installed and its play delivers that turn and skips the built-in
  order tick. On any failure the sandbox is closed and no hooks are installed.
- `brain_seated` tells whether that seat's hooks are a seated brain's.
- `seat_sandbox` returns the sandbox, for tests and tools.
- `set_match_game_facts` stores the facts every brain seated afterwards
  reads. A seat made before that reads a default game.
- `check_brain` loads a brain and runs `fits` with no match. `start` is not
  called, and the sandbox is closed before it returns.
- The brain profile is named `brain`. `install_libraries` is the one place
  a later change adds a library: one call, after the ones already there.
  It opens `f32` and `geometry`, adds `geometry.sine_scaled` and
  `geometry.cosine_scaled`, and creates the global `brain`, `game` and `me`
  tables. `game` is then filled from the seat's facts, and unit handles,
  the read-only `me` surface, and the definition collections are installed.
  It has to work when the seat has no match: `game.in_match` is false, `me`
  still carries the player, level, difficulty and options copied from the
  spec, and `units`, `weapons` and `features` exist and are empty.

## State

The seats live in the match's `seat_state`, one slot per player, and end
with the match. A seat keeps the package's bytes in memory. This module
reads no files.

## Invariants

No heap container, no `throw` and no `virtual` outside `tests/`. No Lua
runs before every network's digest has been checked. `fits` and `start`
run under the phase allowance, the same allowance a mutator's phase gets.
Only a computer seat of this machine is seated. No brain seated changes
nothing about the built-in computer player.

Name the hooks table something other than `brain`. A file that starts
`local brain = {}` shadows the global `brain` table, so a hook in that
file that calls `brain.random` finds the hooks table instead. Use
`local hooks = {}`.

## The game's facts

`game` is one read-only table for the life of the seat. `fits` and `start`
receive that same table. A write to it, or to any table inside it, raises
`game is read-only`.

`game.base` is the content bundle. `game.expansions` is the expansions that
are on, in the built-in order, and `game:has_expansion(id)` reports one of
them. The base game is not an expansion. `game.engine` is the engine's
version. `game.mode` is `skirmish`, `campaign` or `network`.
`game.difficulty` is `easy`, `medium` or `hard`. `game.in_match` is true
for a seated brain and false for `check_brain`.

`game.mod` holds `id`, `name`, `version`, `sides`, `hacks` and `limits`.
Each side is `name`, `prefix` and `commander`. Each simulation hack is
`on` plus its parameters, listed whether the hack is on or off. A view
hack is left out. Each limit is its parameters, with no `on`.
`game:hack(id)` is that hack's table, or nil. `game.mutators` is the
loadout in apply order: `id`, `version` and `options`.

`has_expansion` and `hack` live in the hidden table, so a write to either
name raises and `pairs(game)` yields both. `pairs(game.mod.hacks)` does
not. Nested tables are built once, so `game.mod == game.mod`. `pairs`
walks the hidden table in the host's fixed order.

An installer in `install_libraries` must work when the seat's match is
null. This ticket's own tables do: they read the facts pointer and the
texts copied onto the seat, not the match.

## Tests

`sim-brains-seat` (`tests/seat_test.cpp`) seats the made-up brain
`steady-commander` on the computer-build fixture: `me`, `game`, the
libraries, `require`, a reason from `fits`, load failures, a start that
runs past its allowance, seats that are refused, a seated brain that builds
nothing, and two brains destroyed with the match.

`sim-brains-game` (`tests/game_test.cpp`) seats the made-up brain
`fact-reader` on the same fixture. It reads a rich game back, refuses
writes, checks that `fits` and `start` share one table, and checks the
order of `pairs(game.mod.hacks)` against the simulation hack ids in byte
order. A seat with no facts stored reads the base game. `check_brain`
sees the same facts with `in_match` false, a reason from `fits`, a `fits`
that runs past its allowance, a network refused before Lua, and the
libraries a check can call when no match is open.

`match-seat-hooks` covers the controller switch with no brain loaded.

`sim-brains-me` (`tests/me_test.cpp`) seats the made-up brain `side-reader`
on the same fixture and reads that seat's units, squads, economy and
knowledge.

`sim-brains-events` (`tests/events_test.cpp`) seats the made-up brain
`steady-commander` on the same fixture. Its hooks record every call. The
test checks creation, damage, death and a wreck, the order of one turn, a
gift, a cancelled frame, carry, sight, a blocked path and a completed move,
alliance and defeat, a missing hook, the bank, a full queue, and that two
seated brains and two matches with no brain keep the same random stream and
state hash.

## Handles and my side

A unit handle is one Lua object for one slot and one generation. The
generation is `SlotRuntime::instance_generation`, the count the match already
keeps for that slot. The seat caches the object by slot and replaces it when
the generation changes, so the same unit is the same table key. A slot reused
after a death is not the old unit: `id` and `slot` still read, and `alive` is
false when the slot is one of the seat's. Every other field of that handle is
nil.

`id` and `slot` read on any handle. A seen field reads the seat's own live
unit. Another player's unit reads nil for those fields until A05 replaces
`seat_may_see`, which this seat leaves false. An own field or method reads
only the seat's own live unit. A05 adds seen fields through
`add_unit_getter`. A07 adds own methods, including orders, through
`add_unit_method`.

`me` stays the table B01 created. `player`, `level`, `difficulty` and
`options` move into a hidden table, and assigning `me` raises `me is
read-only`. The seat then reads its name, side, team and color, its resources
as `f32`, and its surpluses as Lua numbers. `tick` is the match tick.
`handed_over` is recorded when the seat is made: false before the built-in
player's set-up has run, true when a seat is made after that. `me.knowledge`
is that built-in player's knowledge record for the seat (`tick`, `builders`,
`base`, and `knowledge:owned`), or nil when the record is not there.

`me:units` and `me:each_unit` walk the seat's occupied slots in slot order.
`me:each_unit` takes that list before the first call. `me:squad`,
`me:squad_size` and `me:squad_member` follow the engine's member order.
Squad 0 skips a member that is not live or whose `Unit.squad` is not 0, so a
restored unit can sit in squad 0's list after it has joined another squad.
Squads 1 to 9 are the lists the engine holds. `me:assign` calls
`set_unit_squad` at once. A unit the seat does not own, or a dead handle,
returns false and counts one refusal.

`brain.distance` and `brain.direction` measure between two units or two
positions with the engine's own maths. A unit whose position the seat may not
read contributes nil.

With no match, the four fields from B01 remain, `handed_over` is false, and
the other getters are nil. `me:units` and `me:squad` are empty,
`me:unit_count` and `me:squad_size` are 0, and `me:assign` returns false.
`brain.distance` and `brain.direction` still work on positions.

## Events

The first seated brain registers one listener on the match's event fan-out.
The listener is not removed; it and the brains end with the match. When the
match cannot take another listener, the seat still runs `onTick`.

Each seat keeps two plain buffers of event records, grown by doubling up to
65,536 records for one turn. Past that, or when a buffer cannot be grown,
the event is dropped and `dropped_events` counts it. At the start of the
turn the incoming buffer becomes the one being delivered, so an event that
fires while the hooks run waits for the next turn.

An event about the seat's own unit is queued for that seat. A death and a
wreck are queued for every seated brain. A gift is queued for the old owner
and the new owner. A carry is queued for the owner of the unit and the owner
of the carrier. Sight gained and lost is queued for that player. A path
failure is queued for the unit's owner. The unit's generation is the one it
had when the event fired.

At delivery an enemy death is passed on only when that slot is on the seat's
seen list, or the seat has full awareness. An ally's death and the seat's
own are always passed on. A death of kind captured is not `onUnitDestroyed`.
A finished event is dropped when a cancelled death of the same unit follows
it in the queue. A wreck in the queue becomes that death's third argument,
`{name, x, z}`, with `x` and `z` fixed numbers; a death with no wreck passes
nil. An attacker or killer is passed only when it is the seat's own unit, an
ally's, on the seen list, or the seat has full awareness, and is nil
otherwise.

The hooks are `onUnitCreated(unit, builder)`, `onUnitFinished(unit, builder)`
(builder nil when there is none or it is the unit),
`onUnitDamaged(unit, attacker, amount, kind)` with kind `weapon`,
`paralyze`, `heal` or `environment`, `onUnitDestroyed(unit, killer, wreck)`,
`onUnitGiven(old, new, from, to)`, `onUnitIdle(unit)`,
`onCommandDone(unit, order, result)` with result `done` or `failed`,
`onUnitLoaded(unit, carrier)`, `onUnitUnloaded(unit, carrier)`,
`onUnitSeen(unit)`, `onUnitLost(unit)`, `onPathFailed(unit)`,
`onAllianceChanged(player, allied)`, `onPlayerDefeated(player)` and
`onTick(tick)`. `onTick` receives `Game.tick`. A missing hook is skipped. A
missing `onTick` is fine.

One turn is one banked call. It delivers the queued events in the order they
fired, then alliance changes for the other players in index order, then a
defeat for each player who was participating and no longer is, then
`onTick`. The first turn stores the alliances and who is participating and
reports none of those changes. A failed call marks the seat stopped; later
turns return without a call. Orders a hook gives take effect in the next
tick's unit sweep.

## Limitations

Radar contacts wait on per-player radar (A05). Chat waits on A08. Timers
run inside this call later (B03). Handing a stopped seat back to the
built-in player, with the notice and the log, is B04. Shots are not
delivered. Placed map features are not listed; `features` is the
feature-type table.

## Definitions

`units`, `weapons` and `features` are the match's loaded records, after the
mod and the mutators. They are read-only. Assigning a field raises
`a collection cannot be assigned` or `a record cannot be assigned`. Replacing
the global `units` itself is ordinary Lua.

A record answers `rec.key` and `rec:get(key)`. `rec:fixed(key)` is that value
as a fixed count: a stored 16.16 count, an integer or a tick count times
65536, or a float times 65536 truncated toward zero. A list or a text is nil.
`rec:keys()` is the kept fields in schema order. `rec:index()` is the type
index, the weapon id, or the feature's index from 0.

`units[i]` is type index `i`, from 1. Index 0 is the reserved empty type and
is not a record. `#units` is that count. `weapons[id]` is the weapon id from
0 to 255, or nil when that slot was not loaded. `#weapons` is how many slots
are loaded, so it matches `each` and `pairs`; the id is still the key, and a
gap stays a gap. `features[i]` is the feature type at index `i - 1`.

Names are matched without regard to ASCII case. The first record in that
order wins when two share a name. `each` is the collection's method, so a
record named `each` is reached by index, not by that name. `pairs` and `each`
walk the same order: type index, then weapon id, then feature index.

An integer is a Lua integer. A 16.16 count is a `fixed` value, and a stored
float is an `f32`, so the bits the record holds are the bits the script sees.
A flag is the record's bit, read when the field is read. A tick field is the
stored tick count, thirty to a second. `category` is the type's categories in
registry order.

These unit keys are not on the record, so they read nil: `copyright`,
`version`, `yardmap`, `corpse`, `badslope`, `badwaterslope`,
`wpri_badtargetcategory`, `wsec_badtargetcategory`, `wspe_badtargetcategory`
and `nochasecategory`. A weapon's sound names, explosion art and model name
are not on the weapon record either.

`weapon:damage()` is the weapon's default damage. `weapon:damage(unit)` uses
a unit record, a unit name or a type index, and reads that unit's entry.
`resolve_unit_type` is the one way a later change turns those three into a
type index.

A unit handle's `type` is that unit's record. It is a seen field: another
player's unit reads nil while the seat may not see it.

With no match the three collections are empty, `#units` is 0, and every
lookup is nil.

`sim-brains-defs` seats the made-up brain `definition-reader` on the
computer-build fixture and also runs that brain with no match.
`sim-brains-defs-data` compares every installed unit's kept fields with the
value the match loaded.
