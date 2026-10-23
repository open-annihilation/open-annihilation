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
  tables. `game` is then filled from the seat's facts, and unit handles and
  the read-only `me` surface are installed. It then installs `brain.random`,
  `brain.after`, `brain.every` and `brain.cancel`. `brain.shared_random` is
  installed only when the seat is Classic and it has a match. It has to work
  when the seat has no match: `game.in_match` is false, and `me` still
  carries the player, level, difficulty and options copied from the spec.

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

`sim-brains-random` (`tests/random_test.cpp`) checks the brain's own stream
against `tests/random_reference.py`, that replacing `math.random` leaves
`brain.random`, that those draws do not move the match's shared stream, and
that Classic is the fingerprint of the files. The sample package is
`tests/classic-sample`.

`sim-brains-timers` (`tests/timers_test.cpp`) checks that `brain.after` and
`brain.every` run after the queued events and before `onTick`, in due-tick
then sequence order, including a cancel from an earlier timer of the same
turn, the waiting limit and the argument errors, and that an error inside a
timer stops the seat and names the timer's chunk and line.

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
defeat for each player who was participating and no longer is, then the
timers whose due tick has come, then `onTick`. The timers run even when the
hooks table is missing. The first turn stores the alliances and who is
participating and reports none of those changes. A failed call marks the
seat stopped; later turns return without a call. Orders a hook gives take
effect in the next tick's unit sweep.

## Timers

`brain.after(ticks, fn)` runs `fn` once, that many turns after the turn that
created it. `start` sees the seating tick, so a timer it creates is due that
many turns later. `brain.every(ticks, fn)` runs then, and again every
`ticks` turns after the due tick it just met. `ticks` is an integer from 1
to 2147483647. Anything else raises `brain.after: ticks must be an integer
from 1 to 2147483647`, or the same sentence for `brain.every`. Each call
returns an id. Ids are sequence numbers, starting at 1, and they only grow.
`brain.cancel(id)` returns true when it removed a waiting timer and false
otherwise. A brain may keep 1024 timers waiting; one more raises
`brain.after: a brain may keep at most 1024 timers waiting`, or the same
sentence for `brain.every`.

Due timers run in order of due tick, then sequence number. One cancelled by
an earlier timer of the same turn does not run. One created during the turn
is due on a later turn. An `every` timer's next due tick is the due tick it
just met plus its period. Each run charges one element of work. An error
inside a timer stops the seat the way a hook error does, and the failure
names the timer's chunk and line.

With no match the three functions still check their arguments, and
`brain.after`, `brain.every` and `brain.cancel` can store and cancel timers.
No turn runs them.

## Random

`brain.random` is the `math.random` function taken when the profile
installs, so both draw from the brain's own xoshiro256** stream. A script
that replaces `math.random` does not change `brain.random`. `brain.random(n)`
returns an integer from 1 to n, as `math.random(n)` does.

The stream is seeded from the first eight bytes, little-endian, of the
SHA-256 of the match seed written as eight little-endian bytes (the 32-bit
seed zero-extended), then the player index as one byte, then the brain's id,
a zero byte, and the version. The same four values always give the same
draws. A load check has no match seed, so it uses 0. Draws from this stream
do not advance the match's shared stream.

`brain.shared_random(bound)` draws from that shared stream, the one unit
scripts and the simulation draw from. `bound` is an integer from 0 to
4294967295; anything else raises `brain.shared_random: argument 1 must be an
integer from 0 to 4294967295`. The result is the match's own bounded draw: a
value below `bound`, or 0 without advancing the stream when `bound`, read as
a signed 32-bit value, is below 2. For any other brain, and for every brain
with no match, `brain.shared_random` is nil.

## Classic

A seat is Classic only when its files are the files of this build's
`brains/classic/` folder: the same paths and the same bytes. The id, the
name, the manifest and the install path prove nothing. The test compares
fingerprints. One line per file, in byte order of the path, is the SHA-256
of that file in lower-case hex, a space, the path and a newline. The
installer's record `.oa-origin.yaml` is left out. The fingerprint is the
SHA-256 of that text, written the same way. The build computes it from
`brains/classic/` and writes a generated header. A missing folder, or one
with no other file, has an empty fingerprint, and then no package is Classic.

`seat_is_classic` is the one test for every Classic privilege. The
privileges are the shared stream (`brain.shared_random`, and later a draw
from a copy of it) and the direct write of the standing bits. Full awareness
is the manifest's own flag, kept on the seat. `seat_is_classic` does not
decide it.

Tests may set `classic_fingerprint_for_tests` on the spec. When it is not
empty it replaces the fingerprint baked into the build, for that load. The
app never sets it.

## Limitations

Radar contacts wait on per-player radar (A05). Chat waits on A08. Handing a
stopped seat back to the built-in player, with the notice and the log, is
B04. Shots are not delivered. A saved game does not keep timers or the
brain's stream: the brain starts again from `start`.
