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
- `set_report_hooks` stores where a seat reports printed lines and a stop.
  The simulation writes no file and posts no message. Empty functions
  report nothing. The hooks are kept before any brain is seated.
- The brain profile is named `brain`. `install_libraries` is the one place
  a later change adds a library: one call, after the ones already there.
  It opens `f32` and `geometry`, adds `geometry.sine_scaled` and
  `geometry.cosine_scaled`, and creates the global `brain`, `game` and `me`
  tables. `game` is then filled from the seat's facts, and unit handles,
  the read-only `me` surface, the definition collections, the AI profile
  fields, `game.map` and what the seat may see are installed, then
  `brain.say`, `brain.random`, `brain.after`, `brain.every` and
  `brain.cancel`. `brain.shared_random` is installed when the seat is
  Classic or a shadow, and it has a match. `brain.section` is installed for
  every brain. `brain.setup_random` is installed for a Classic seat and for
  a shadow, and with no set-up record it raises
  `brain.setup_random: the seat is not set up yet`. The unit order methods,
  `brain.order_counts`,
  `brain.site` and `brain.place` follow. It has to work when the seat has no
  match: `game.in_match` is false, `game.map` is nil, `me` still carries the
  player, level, difficulty and options copied from the spec, `units`,
  `weapons` and `features` exist and are empty, `brain.say` answers that
  chat is not connected, `brain.site` and `brain.place` return nil, and
  `brain.order_counts` is all zeros. With no match, `me.seen` and `me.radar`
  are empty, `me:sees` and `me:sees_unit` are false, and `me.sight` and
  `me.explored` are nil. Networks and grids are installed last, after the
  orders. `brain.network` and `brain.tensor` still work when the seat has no
  match. `brain.grid` returns nil then, without reading its arguments or any
  match field.

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

## The map

`game.map` is the match map. Outside a match it is nil. Inside a match it
is one read-only object for the life of the seat. Assigning a field raises
`map is read-only`. An unknown name reads nil. `getmetatable` is false.

`name` is the map's name and `path` is its mission path, such as
`Maps/DryFlats.TNT`. `path` is nil when the facts have no path. `world` is
always nil: the engine has no world type to report. `width` is the map in
cells. `world_width` and `world_height` are the same edges in world units.
`sea_level` is an integer. `tide` is an `f32`. `wind_min` and `wind_max`
are the wind's strength range, and `map:wind()` returns the current speed
and direction as two integers. `surface_metal` is the metal of the first
cell that is not a metal deposit.

The cell count and a corner height share the name `height`, and Lua keeps
one value per name. `map.height` is a function. `map:height()` with no cell
is the height in cells. `map:height(cx, cz)` is that cell's corner height,
or nil off the map. `map:heights(cx, cz)` is the cell's low and high
height, or nil off the map. `map:ground(x, z)` is the sampled ground height
at a world position, including off the map, where the engine reports -1.
`x` and `z` are fixed values or whole world units. `map:metal(cx, cz)` is
that cell's metal, or 0 off the map. `map:metal_feature(cx, cz)` is whether
the cell is a metal deposit, false off the map.

`map:metal_spots()` and `map:geo_spots()` are the deposits in the engine's
scan order, rows then columns. Each entry is `{cx, cz}`. Walking the map
costs one charge per cell. Outside a match, `map:metal_spots()` walks the
seat's metal-feature query when the seat has a world and that query, in
the same order, and is nil when the query is absent. `map:geo_spots()`
stays nil outside a match.

`map:classes()` lists the movement classes the match's unit types use, in
handle order. Each entry is `name`, `handle`, `footprint_x` and
`footprint_z`. A class the match did not build a movement map for is left
out. Walking the types costs one charge per type.

`map:passable(class, cx, cz)` is that class's cell value: 0 blocked, 1 slow
or not clear, 3 clear, and 0 off the map. It does not look at what the seat
has seen. `map:anchor(class, position)` is the cell a unit of that class
would stand on at that position.

`map:reachable(a, b, class)` is true when the two positions can meet by
moving through that class's open cells, including a diagonal step. The
regions are the ones the movement map holds for the seat. They are built
the first time that class is asked in a tick, and kept for later questions
in the same tick. Four classes are kept. A fifth frees the oldest tick, and
a tie frees the smaller handle. A question does not search for a path and
does not change the movement map, its projection or its occupancy. Building
the regions costs one charge per sixteen cells, rounded up. Each question
costs 16 more. A question that fails its arguments costs nothing.

A class is a class name, ignoring ASCII case, a unit type's record, a unit
type's name, or an integer. An integer that is a class handle is that
class. Any other integer in range is a unit type. A type with no movement
class can reach any two positions, including off the map. `passable` and
`anchor` refuse that type with `unknown movement class`. A unit handle is
not a class. A position the seat may not read makes `reachable` return nil
and still costs 16. It is not an error, and the class is not checked.

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
alliance and defeat, a missing hook, a hook that spends the bank and hands
the seat back, a full queue, and that two
seated brains and two matches with no brain keep the same random stream and
state hash.

`sim-brains-sight` (`tests/sight_test.cpp`) seats `fog-reader` and
`full-sight` on that fixture with mapping and line of sight on. It checks
what a handle reads in and out of sight, the seen list, the sight and
explored grids, radar contacts and their events, and the full-awareness
search.

`sim-brains-networks` (`tests/networks_test.cpp`) seats the made-up brain
`steady-commander` on that fixture, with the made-up mod `ridge`. It builds
networks with `oann::write`, runs them from Lua, refuses a bad file, a run
that is too large and a set that would pass 64 MiB, and checks tensors,
charges and `brain.grid`.

`sim-brains-failure` (`tests/failure_test.cpp`) seats `steady-commander` on
the same fixture. A turn that raises, spends the bank, hits the real-time
limit, fills memory, or builds a string past the limit closes the sandbox
and clears the hooks on that turn. The built-in player's squad countdown
stays where it was on that tick and moves on the next. Printed lines keep
the tick they were printed on. A brain that never fails is not handed back.
`fits` and `start` do not report a stop.

`sim-brains-chat` (`tests/chat_test.cpp`) seats the made-up brain
`chat-listener` on the same fixture. Lines queued with `hear_chat` reach
`onChat` at the next turn, in order, after the events and before `onTick`.
`brain.say` passes each audience to the test's say hook and returns each
answer. Two matches with no brain, and two matches whose brain says a line
every tick, keep the same random stream and state hash.

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
unit, and another player's live unit while the seat can see it now or the
brain has full awareness. `health` and `health_percent` stay nil when the
type hides damage, unless the seat owns the unit or the brain has full
awareness. An own field or method reads only the seat's own live unit.
Order methods are registered through `add_unit_method` with access
`always`. The method itself counts a dead handle and a unit the seat does
not own. Access `own` would return nil before that method ran, so the
refusal would not be counted.

`me` stays the table B01 created. `player`, `level`, `difficulty` and
`options` move into a hidden table, and assigning `me` raises `me is
read-only`. The seat then reads its name, side, team and color, its resources
as `f32`, and its surpluses as Lua numbers. `tick` is the match tick.
`handed_over` is recorded when the seat is made: false before the built-in
player's set-up has run, true when a seat is made after that. `me.knowledge`
is that built-in player's knowledge record for the seat (`tick`, `builders`,
`base`, and `knowledge:owned`), or nil when the record is not there. The
record is the seat's knowledge call. Outside a match that call is null, so
`me.knowledge` stays nil and the call is not made through a null match.

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
fired, then the changes in the seat's radar and sonar contacts, then
alliance changes for the other players in index order, then a defeat for
each player who was participating and no longer is, then chat, then the
timers whose due tick has come, then `onTick`. The timers run even when the
hooks table is missing. The first turn stores the alliances and who is
participating and reports none of those changes. A failed call closes the
sandbox, clears the seat's hooks and reports the stop once. The failing
turn still counts as played. The next turn has no hooks, so the built-in
computer player takes it. Orders a hook gives take effect in the next
tick's unit sweep.

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
a signed 32-bit value, is below 2. A Classic seat draws the match's stream.
A shadow draws a copy of it, put back at each section (see Shadow). For any
other brain, and for every brain with no match, `brain.shared_random` is nil.

`brain.section(name)` marks a section of the turn. `name` is 1 to 15
letters, digits or hyphens; anything else raises `brain.section: a section
name is up to 15 letters, digits or hyphens`. In a game the mark does
nothing. A shadow, and a later trace, record it. With no match it does
nothing.

`brain.setup_random(bound)` draws from a copy of the shared stream taken at
the seat's built-in set-up, and advances only that copy. The bound is
checked as `brain.shared_random` checks it. Before the set-up record exists,
and with no match, it raises `brain.setup_random: the seat is not set up
yet`. A brain that is neither Classic nor a shadow has no
`brain.setup_random`.

## Shadow

`--brain-shadow` runs a brain beside the built-in player on every computer
seat this machine plays. The built-in player plays the seat. The brain reads
the seat as it stood before that turn, draws from a copy of the shared
stream, and its orders, squad changes, standing-bit writes and draws are
recorded and not applied. A seat a brain already plays is not shadowed. A
network game is not shadowed. A failed shadow is not called again; the
built-in player keeps the seat, and the run prints the stop instead of a
comparison. No notice is posted.

The set-up record keeps the stream state just before the seat's first
set-up draw, and the first eight draws (bound and value). `set_up` then
calls `start`. `brain.setup_random` draws from a copy restored to that
state. Draws it makes are the `start` section.

At each `brain.section(name)` the copy is set to the stream state at the
built-in player's marker of that name, and the squad view is the snapshot
plus that player's assigns from the sections before the marker. Before the
brain's n-th draw in a section, when the built-in player's n-th draw there
exists and the stream moved between the previous draw (or the marker) and
that draw, the copy is set to the state before that draw and the log records
`resync`. A resync does not change the bound. The reset does not copy the
built-in player's entries of that same section into the brain's log.

Compared, for the sections named by `--brain-shadow-sections` (default
every one of `start`, `schedule`, `sort`, `task1` to `task9`):

- `schedule` is the list of section markers.
- Each other section is its entries in order. Every field counts: the kind,
  the slots, the type, the 16.16 coordinates, the queue flag, and the bound
  and value of each draw. `resync` lines are left out.
- `sort` also compares the stance list, in order.
- `start` compares the set-up record's draws with the brain's
  `setup_random` draws, whenever the brain makes them.

Each time the built-in player acts, including the load's player pass, that
turn is compared. One line is printed per seat at the end of the run. The
logs kept are the set-up and, when the logs differ, the first differing
tick and the two before it. Older ticks are dropped. `--brain-shadow-log
DIR` writes
`seat-<p>-start-builtin.log`, `seat-<p>-start-brain.log`, and
`seat-<p>-tick-<t>-builtin.log` and `seat-<p>-tick-<t>-brain.log` for those
ticks. `<p>` is the engine player index.

Both logs use the same lines, one entry a line, numbers in decimal,
coordinates as signed 16.16 integers:

```text
tick <t> seat <p> builtin|brain
section <name>
draw <bound> <value>
assign <slot> <squad>
move <slot> <x> <y> <z> <queue>
patrol <slot> <x> <y> <z> <queue>
attack <slot> <target>
build <slot> <type> <x> <y> <z>
queue <slot> <type> <count>
on <slot> <0|1>
attack_point <slot> <x> <y> <z>
stance <slot> <move> <fire>
refused <name> <slot>
resync <state>
```

`sort` is the squad sort. `task1` to `task9` are the built-in tasks.
`start` is the set-up. A section the list does not name is not compared.
An empty squad's siege task (`task9`) marks the section, draws once from a
bound of 150, and orders nothing.

## Logs and records

`--brain-trace FILE` writes one brain log for the whole match. It needs a
headless run of a set number of ticks. The file is created when the match
starts. The first line is `brain log 1`. After that, every computer seat
this machine plays contributes the call log of each of its ticks: a
brain's hook calls, orders and draws, and a built-in seat's orders and
draws. A shadow run writes both of its sides, the built-in player first.
`<p>` in `tick <t> seat <p>` is the engine's player index. `fits` and
`start` are tick 0.

The brain log uses the shadow's lines, and four more:

```text
brain log 1
call <hook> <n>…
own-draw <low> <high> <value>
own-draw float <16 hex digits>
order <name> <slot> <n>…
```

`call` is written before the hook runs. Its numbers are the hook's
arguments: a unit is its slot, a player is its index, an amount is the
amount delivered. A timer is named `timer`. `brain.random(n)` writes
`own-draw 1 n`, and `brain.random(0)` writes `own-draw 0 0` with the raw
word. `math.random(m, n)` writes `own-draw m n`. `math.random()`
writes `own-draw float` and 16 lower-case hex digits of the value's bits.
`brain.shared_random` is a `draw` line, the same line the shadow writes.
An order the shadow's lines already name is written as that line. Any
other order is `order`, with its numbers in the order the brain passed
them.

`--brain-record FILE` writes what a brain passes to `brain.record`, as
JSON lines. The same match writes the same bytes on every machine.

```lua
brain.record{
  features = { sectors = brain.grid({"own", "explored"}, 8) },
  decision = dx
}
```

`features` is one to 16 tensors. A name is 1 to 31 letters, digits or
underscores. `decision` is an integer or a tensor. One call holds at most
4194304 elements. A wrong call raises, for example
`brain.record: feature 'grid' is not a tensor`. The call costs 64
instructions plus one more for every 64 elements, whether or not a file
is open, and it returns nothing. Outside a match it checks and charges
the same and records nothing.

The file's first line is a header: the map, the seed, and each computer
seat's brain or `builtin`, its level, and a brain's version. A sample
line follows each call, features in byte order of their names. Values are
counts of 65536ths, so `65536` is 1. An integer decision is written as
that integer. A brain match that plays to its report then writes one
result line a seat: whether it won, the tick, and how many units it still
has. Record seats are numbered from 1, so record seat 1 is log seat 0.
The file stops at 1 GiB with a last line `{"kind":"full"}`.

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

## Chat

`onChat(from, text, to)` runs once per line that reached the seat since its
last turn, in arrival order. `from` is the sender's player index. `text` is
the words, with no name head. `to` is `all`, `allies`, `enemies` or `me`
(a line addressed to this seat). A missing `onChat` is skipped, and the line
is still consumed. A line heard while the hooks run waits for the next turn.
At most 32 lines wait for one turn; a further line is dropped and counted by
`dropped_chat_lines`. Words longer than 256 bytes are cut between whole
characters. A long line that arrived as several parts is several calls.

`brain.say(text [, to])` hands one line to the match at once. `to` is
`all` (the default), `allies`, `enemies` or a player index from 0 to 9.
Anything else raises. It returns true when the line was queued, and otherwise
false plus `not_a_seat`, `empty`, `command`, `too_soon`, `no_target` or
`no_chat`. It does not pace, post to the log or send a command itself: the
match does that. With no say hook, or outside a match, the answer is
`no_chat`.

The app sets that hook, and the listener that queues heard lines, only once
a brain is seated. A match with no brain keeps the chat listener it already
had. The simulation reaches the app only through this hook.

## Orders and sites

A unit handle takes the orders a player can give. Each call reaches the
match at once. The order runs in the next tick's unit sweep. A refusal
returns false and is counted. It is not raised.

`move` and `patrol` call `issue_ground_move` and `issue_patrol`. `attack`
calls `issue_attack_command` with no point. `attack_ground` is a click on
open ground (`issue_attack_ground`). `attack_point` is the built-in
player's attack on a point: it resolves the attack command over open ground
and gives that mission with `issue_order`, unqueued, and returns false when
no mission kind resolves. Classic's attack on a point is `attack_point`.
`dgun` calls `issue_attack_special`, and a unit argument uses that unit's
slot and its current position. `guard`, `repair`, `help`, `capture`, `load`
and `unload` call `issue_guard`, `issue_repair`, `issue_help_build`,
`issue_capture`, `issue_load` and `issue_unload`. `reclaim` of a unit calls
`issue_reclaim`; `reclaim` of a position calls `issue_feature_reclaim`.
`build` calls `issue_mobile_build`, with a facing from 0 to 3. `queue`
calls `queue_factory_build`, with a count from 1 to 1000 and a default of 1.
`requeue` calls `change_queued_count` for the `BuildingBuild` kind. `stop`
calls `issue_stop`. `on` calls `toggle_activation` for the active bit only
when that bit differs, and returns true when nothing needed changing.
`cloak` calls `issue_cloak`. `self_destruct` calls `toggle_self_destruct`
for that one unit. Where a method takes a queue flag, a missing flag is
false.

`stance` gives `issue_state_order` for each of `move` (`hold`, `manoeuvre`,
`roam`) and `fire` (`hold`, `return`, `at_will`) that the table names.
`direct = true` writes the standing bits the way the built-in player writes
them and gives no order. The only test for that write is `seat_is_classic`.
Any other seat, one with full awareness included, raises `stance: direct
writes are for the Classic brain only`.

An order is refused when the handle is dead (`dead`), the unit is not the
seat's (`not_own`), the turn has already passed 4,096 orders to the match
(`over_limit`), the target is not one the seat may read (`not_seen`), or
the type is not on the builder's list (`not_buildable`). The first failing
check is the one that counts. A match refusal, a null mission or a caught
exception counts as `by_engine` and does not reach Lua. `on` with nothing
to change, a direct stance and a site check are not orders toward that
limit. `brain.order_counts()` is this turn. `brain.order_counts("match")`
is the match. A live turn starts the turn's counts again. `start` is one
turn.

`brain.site(type, cx, cz [, facing [, check]])` charges 16. On a seat with
full awareness, `"site"` (the default) is `site_clear_for` and `"yard"` is
`building_site_clear`. On any other seat the type must be a building,
`"site"` is that check's map-edge test and then `building_site` with the
seat as the placing player, and `"yard"` is `building_site` alone. Full
awareness is the only choice. `seat_is_classic` does not decide it.
`brain.place(type, near, radius [, facing])` starts at the footprint cell
whose centre is `near`, then walks rings at Chebyshev distance up to
`radius` world units divided by 16, each ring from its north-west corner
clockwise. It returns the first cell where `"site"` holds, and that cell's
centre, or nil after the ring limit or 4,096 cells. Each cell tried
charges 16.

`sim-brains-orders` gives every order from the made-up brain `order-giver`
and from C++ on a second match, and compares the queues and the next tick.
`sim-brains-sites` compares `brain.site` with the check the seat's
awareness selects, over a grid of cells and every facing, and compares
`brain.place` with that same walk.

## When a brain fails

A failed turn closes the sandbox and clears the seat's hooks before that
turn returns. The report hooks hear the stop once, with the script error
still alive, after the printed lines of that call have been drained. A
later turn finds no hooks and does not call into the brain. The built-in
computer player already has a controller for the seat, made when the
computer players were set up, and the next order tick resumes the tasks it
already holds. Every task still has its next tick at 0, so they all run on
that first order tick. The squad sort follows 30 order ticks later and
reads the squads the brain left. Nothing about that controller is rebuilt.

A brain that is refused, fails to start, or does not fit never has hooks,
so the built-in player plays from the start. `fits` and `start` drain
their printed lines and do not report a stop. The app posts the seating
notice from the outcome. A defeated player is simply no longer called. The
sandbox closes with the match.

## What a seat may see

A brain without full awareness may know only what its seat could see, what
its own radar and sonar picture holds, and what it has explored. A seen
field of another player's unit (`type`, `owner`, position, `heading`,
`build_remaining`, `finished`, `building`, and health) reads while
`unit_visible` is true for that slot now. Health still reads nil when the
type hides damage, unless the seat owns the unit. A dead handle reads nil
for those fields. `id` and `slot` always read.

`me.full_awareness` is the flag the seat was given. `me.seen` is the seat's
seen list, in that list's order, as of its last refresh, and `me.seen_tick`
is the refresh tick. Each entry is a unit handle. `me:sees(x, z [, y])`
asks `point_visible` for that point (`y` is 0 when it is left out). `x`,
`z` and `y` are fixed values or whole world units. `me:sees_unit(u)` asks
`unit_visible` for a live handle and is false for a dead one. Neither
question treats full awareness as a yes: they answer what the engine
answers.

`me.sight` and `me.explored` are grids of 32-unit cells (`width`, `height`,
`cell_size`, `at(cx, cz)`, `cell_of(x, z)`). A sight cell is in sight when
the simulation's reading of the seat's coverage is not zero. An explored
cell is one whose bit is set for the seat. `at` is false off the grid.
`cell_of` is the cell of a position with `y` held at 0, or nil off the
grid. Both grids are nil when the match keeps no sight grid. With the line
of sight rule off, `me:sees` follows the engine and reads the viewpoint's
mapped bit, not the queried seat's.

`me.radar` lists the live enemy contacts in the seat's own picture, in slot
order, and `me.radar_tick` is the tick of that picture. A contact gives
`x`, `y`, `z` and `position` while it is still a contact of the latest
picture with the same generation, and nil after that. `sonar` and `radar`
stay as they were when that contact was published. `current` is whether it
is still a contact. A contact has no type, no owner and no unit handle.
`onRadarContact(contact)` and `onRadarLost(contact)` fire, right after the
queued events, for each contact the picture gained or lost since the list
the seat delivered last. The first picture is compared with an empty list.
An allied unit is never a contact. The picture's bytes are the seat's own;
the viewpoint's radar marks are not read.

A full-awareness brain also has `game.units`, `me:nearest_enemy(x, z [, filter])`
and `me:seen_raw()`. Without that flag those names are nil. `game.units:list([player])`
is every occupied slot, in slot order, optionally one player's.
`game.units:each(fn)` walks that same order from a list taken before the
first call. `me:nearest_enemy` is the built-in player's search over every
unit: the seat's alliance row, with the extra relation entry set, and a
filter table whose `widened` and `skip_submerged` booleans are off when
the key is missing. `me:seen_raw()` reads each seen slot's current record
(`slot`, `type_index`, `x`, `y`, `z`), so a unit that died after the
refresh reads type 0 and the position it still holds.

With no match the seen list, the radar list and `me:seen_raw()` are empty,
`me:sees` and `me:sees_unit` are false, the grids are nil, `game.units:list()`
is empty and `me:nearest_enemy` is nil. The picture is kept for the rest of
the match once the seat is made.

`sim-brains-sight` seats the made-up brains `fog-reader` (no full
awareness) and `full-sight` (`fullAwareness`) on the computer-build fixture
with mapping and line of sight on.

## Limitations

Shots are not delivered. A saved game does not keep timers or the brain's
stream: the brain starts again from `start`. Placed map features are not
listed; `features` is the feature-type table.

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

`sim-brains-map` (`tests/map_test.cpp`) seats the made-up brain `map-reader`
on the computer-build fixture. It reads the map, the cells, the deposits
and one movement class, checks reachability around a wall once a gap is
open, and checks that a question does not move the movement map. Outside a
match, `check_brain` sees `game.map` nil. There is no installed-map run:
nothing in the suite loads a map the game would install.

## AI profile data

`game.ai_profile` is the profile the match's computer players read. It has
`path`, `name` (the file name without its folder or a trailing `.txt`, as
written), `text`, `too_large`, `lines` and `report_path`. `text` is nil and
`too_large` is true when the text passes the host's string limit, 64 KiB;
the lines are still parsed. `report_path` is the map's profile slot, nil
when that slot is empty, and it can differ from `path`. An empty path makes
`game.ai_profile` nil.

`game.ai_profiles()` returns every profile in the facts, in the listed
order. `game.parse_profile(text)` parses any text, such as a type's
`ai_weight`, and leaves `path` and `name` unset. Each call builds a new
read-only table. Assigning a field raises `a profile is read-only`. The
charge is the text's bytes plus one unit per word.

`lines` is one table per line `computer_profile_lines` reports. A line has
`number`, `words`, `command` (the first word in lower case) and, by command:

- `plan`: `any`, `easy`, `medium`, `hard`. `any` is true only when at least
  one word follows `plan` and the first of them is `any`. Each keyword is
  true when some word after `plan` equals it. ASCII case is ignored.
- `weight`: `name` (the second word, or `""`), `factor` (an `f32`,
  `static_cast<float>(std::strtod)` of the third word, or 0 when it is
  missing) and `unit`.
- `limit`: `name`, `count` (`static_cast<int32_t>(std::strtol)` base 10 of
  the third word; `-1` stays `-1`) and `unit`.

`unit` is true when `name` equals some type's unit name, ignoring ASCII
case. An empty name is not a unit. Outside a match `unit` is nil.

The grammar is the built-in player's. A line ends at `\n`, and a file with
no final newline still has that last line. Words are separated by space,
tab, `\r`, `\n`, `\v` or `\f`. `#` ends the line, even inside a word. A
line keeps at most 20 words, and those words share 126 bytes, so a long
word is cut. Anything that is not `plan`, `weight` or `limit`, including
`//`, is still a line.

`profile:select(keyword)` is one pass for `"easy"`, `"medium"` or `"hard"`.
Any other keyword raises `select expects easy, medium or hard`. The plan
flag starts matched, so lines before the first plan apply. A plan line
sets the flag from that line's `any` or its keyword, and the flag is not
kept after the call. The result is `{weights = {…}, limits = {…}}`, the
line tables that apply. The built-in player keeps the flag from one pass
to the next and applies a downloadable type's own lines twice. This pass
does neither.

In a match, `game.build_list(type)` is a new sequence of `units` records.
`type` is a unit record, a unit name or a type index, resolved by
`resolve_unit_type`. The list is computed from the runs
`match_computer_players` holds, by the built-in player's rules: from type
0, a count then that many ids; a type gets a list only when it is not 0
and it is a builder; an id that is 0 or not below the type count is
skipped; at most `build_list_kept(Match::limits().build_lists) + 1` ids
are kept. A non-builder, or a type the runs do not reach, is an empty
sequence. `game.build_list_cap` is that limit. `game.downloadables_allowed`
is false when the computer players were configured for a campaign.

`brain.strengths(type)` returns the seat's three strategic bytes, each read
as `int8_t`: build priority, metal, energy. The charge is 2. It returns
nil when the seat's table does not reach the type. The table is empty
until the match prepares its computer players, which is before the brain's
first `onTick`, and it is replaced when the match refreshes that player.

Outside a match the profile fields still work and `unit` is nil.
`build_list` and `build_list_cap` are nil unless the seat carries build-list
runs, in which case they follow the same rules from those runs and the seat's
kept count. `downloadables_allowed` and `brain.strengths` are nil.

`sim-brains-profile` seats the made-up brain `profile-reader` on the
computer-build fixture. `sim-brains-profile-data` parses every installed
`ai/*.txt` profile.

## Networks

After every network file's SHA-256 matches the manifest, and before the
sandbox opens, the load reads each file with `oann::read` and builds it
with `Network::build`. `Network::memory_bytes_of` runs first. A network
that would take the seat past 64 MiB (`67108864` bytes) is refused before
`build` allocates it, and so is a file `oann::read` refuses or a network
`build` refuses. The reason is `network <id>: <the oann or build problem>`.
The seat is removed and the built-in computer player keeps the seat. An
empty network list holds no `NetworkSet`. The load is outside every phase
and is charged to nothing. Nothing here repeats the SHA-256 check.

A network holds at most 8,388,608 weight elements and one run at most
268435456 operations. A tensor holds at most 4194304 elements. A grid side
is 8, 16, 32, 64 or 128. 64 is the default.

`brain.network(id)` returns the handle of a network the load built, the
same userdata every time for one id. A missing id raises
`brain.network: this brain has no network 'x'` and costs nothing. A found
id costs 1.

`brain.tensor(values[, shape])` builds an immutable tensor. `values` is a
sequence of 1 to 4194304 numbers. An integer `i` is stored as `i × 65536`.
A fractional number is multiplied by 65536 and truncated toward zero; a
non-finite number is refused. A fixed value is stored as its count. A
value outside -32768 to 32767.99998 is refused with
`brain.tensor: value 5 is outside -32768 to 32767` (the index is 1-based;
the text stops at 32767). `shape` is 1 to 4 positive integers whose
product is the number of values, or `{#values}` when it is omitted. The
other refusals are `brain.tensor: the shape must be 1 to 4 positive
integers`, `brain.tensor: the shape covers N values, not M`,
`brain.tensor: expects 1 to 4194304 numbers`,
`brain.tensor: value N is not a number` and
`brain.tensor: value N is not a finite number`. The result's metatable is
`oa.brain.tensor`. `tostring(t)` is `tensor: 2x8x8`. Assigning a field
raises `a tensor cannot be changed` and costs nothing.

`t:get(i)` is the 1-based element as a fixed value. An index outside
`1` to the element count raises `brain.tensor: index 0 is outside 1 to N`
and costs nothing. `t:argmax()` is the 1-based index of the largest
element, the first when several match. `t:totable()` is a sequence of
fixed values. `t:size()` and `#t` are the element count. `t:shape()` is
the shape as a sequence.

`net:run(inputs)` takes a table with exactly the network's input names.
An unknown name is refused with the first in raw byte order:
`network <id>: unknown input '<name>'`. A key that is not a string, when
every string key is a real input, raises
`network <id>: input names must be strings`. A missing input is refused
with the first in file order: `network <id>: missing input '<name>'`. A
value that is not a tensor raises
`network <id>: input '<name>' must be a tensor`. A tensor whose element
count differs raises
`network <id>: input '<name>' has <got> elements, not <want>` (always the
word elements; the shape is not compared). Each element becomes that
input's int16 by `rescale(count, -16, exponent)` and `saturate_int16`.
The result is a new table. An int16 output is a tensor of that output's
shape, each element `saturate_int32(rescale(q, exponent, -16))`. An
argmax output is a Lua integer, the 0-based index plus one. An embedding
row out of range raises before that table exists:
`network threat: layer 4 (embedding) has no row 70`. A call whose first
argument is not a table raises
`network <id>: run expects a table of inputs` and costs nothing.
`run` on a value that is not a network raises `run expects a network`.

`net:inputs()` and `net:outputs()` are the port names in file order.
`net:cost()` is the units one run is charged, `charge_units()`.

Charges are fixed by sizes and taken once, before the work, with
`charge_work`. An argument error taken before that charge costs nothing.
A charge the allowance cannot hold stops the phase before any result
exists.

- `brain.tensor`: one unit per value and one per shape entry.
- `t:totable()`: one unit per element.
- `t:get`: one unit. The brief's "as shown" is read this way.
- `t:argmax`: one unit per 64 elements, rounded up.
- `t:size` and `#t`: one unit.
- `t:shape`: one unit per shape entry.
- `net:run`: `charge_units()`, plus one unit per 64 input elements
  converted, plus one unit per 64 output elements converted, plus
  `scan_call_charge` (4096) for the one walk of the argument keys.
- `brain.grid`: one unit per 16 cells of `layers × size × size`, rounded
  up, plus one unit per unit slot when any of `own`, `allies`, `enemies`,
  `threat` or `radar` is asked (one walk, however many of those five),
  plus one unit per 64 map plots when `height` or `metal` is asked, plus
  one unit per 64 sight cells when `sight`, `explored` or `water` is
  asked. `water`'s value is read from map plots; its charge is the sight
  cells, grouped with `sight` and `explored`.
- `brain.network`, `tostring`, and `net:cost`: one unit. `net:cost`
  returns the run charge and the call itself costs one.
- `net:inputs` and `net:outputs`: one unit per name, and one unit when
  the network has no ports of that kind.
- Assigning a tensor or a network costs nothing; it raises.

`brain.grid(layers[, size])` returns a tensor `{#layers, size, size}`.
`layers` is a sequence of 1 to 10 distinct names, in the order the tensor
stores them. Rows run from north (`z = 0`) to south and columns from west
to east. Cell `(row, col)` covers world x from `col × W / size` to
`(col + 1) × W / size`, and z alike, with `W` and `H` the map's
`map_width_world` and `map_height_world`. The division is whole numbers.
A map plot is 16 world units and a sight cell is 32. A source cell belongs
to the grid cell that holds its centre. A grid cell that no centre falls
in reads the source cell under its own centre. The ten layers are:

- `sight`: the share of its sight cells the seat sees now, `count × 65536 / total`, truncated, from 0 to 1.
- `explored`: the share of its sight cells the seat has ever seen, the same way.
- `water`: the share of its map plots whose height is below `Game.sea_level`.
- `height`: the mean `MapPlot.height`, truncated to 1/65536.
- `metal`: the mean `MapPlot.metal`, the same way.
- `own`: the seat's live units in the cell, nanoframes included, stored as `n × 65536`.
- `allies`: allied players' live units the seat may see, excluding the seat's own.
- `enemies`: other players' live units the seat may see.
- `threat`: those enemies whose type has `OA_UNIT_DEF_FLAG_HAS_WEAPONS`.
- `radar`: radar and sonar contacts that are not also in sight.

Units are walked once, in slot order. A unit whose world position is
negative or past the map is skipped. Sight and explored are 0 when the
sight grid has no cells; water is still read from the plots. Full
awareness shows units the seat could not otherwise see. It does not invent
radar contacts, and a contact still in sight stays off `radar`. An unknown
layer raises `brain.grid: unknown layer 'foo'`. A repeated name raises
`brain.grid: layer 'sight' is repeated`. A size other than the five above
raises `brain.grid: size must be 8, 16, 32, 64 or 128`. A sequence that is
empty or longer than 10 raises `brain.grid: expects 1 to 10 layer names`.
Those refusals cost nothing. Outside a match the call returns nil and
costs nothing.
