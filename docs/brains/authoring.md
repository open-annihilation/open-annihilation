# Writing a brain

A brain is one folder, packed as a `.oabrain`. This guide walks that
folder from empty to a file you can seat in a headless match, then covers
the choices that keep the same package working on another mod. The rules
are [the OABrain standard](oabrain-standard.md). Publishing a package is
[Packages](../registries/packages.md).

The engine's own computer player stays the default. A brain plays only a
seat a player set to it. Classic, walked through below, is an example of
that computer player. It is not a replacement.

## Contents

- [1. A first brain](#1-a-first-brain)
- [2. Reading the game](#2-reading-the-game)
- [3. Giving orders](#3-giving-orders)
- [4. Events and timers](#4-events-and-timers)
- [5. Seeing the enemy](#5-seeing-the-enemy)
- [6. Levels and options](#6-levels-and-options)
- [7. Fitting a mod](#7-fitting-a-mod)
- [8. Keeping state across a save](#8-keeping-state-across-a-save)
- [9. Chat](#9-chat)
- [10. Learned networks](#10-learned-networks)
- [11. Testing](#11-testing)
- [12. When a brain fails](#12-when-a-brain-fails)
- [13. Classic, a walkthrough](#13-classic-a-walkthrough)
- [14. Publishing](#14-publishing)

## 1. A first brain

Make a folder and put `oabrain.yaml` in it. The id is lower-case
kebab-case. The version is quoted, because a bare `1.0` is a number.
`author` is required. Steady Commander is the made-up brain in
`brains/examples/steady-commander/`. `works-with` is `"*"`, quoted because
a bare `*` is a YAML alias, so the package fits any mod and the base game
alone ([11. Compatibility](oabrain-standard.md#11-compatibility)). Three
levels use the difficulties the game already has. One choice, `attack`,
says how soon the squad moves.

```yaml

oabrain: 1
id: steady-commander
name: Steady Commander
version: "1.0"
author:
  name: Ridge Workshop
description: "Builds a little economy, then a factory, and sends a squad when it is ready."
tags:
  - economy
  - teaching
works-with:
  - "*"
levels:
  - id: easy
    name: Easy
    difficulty: easy
  - id: medium
    name: Medium
    difficulty: medium
  - id: hard
    name: Hard
    difficulty: hard
options:
  attack:
    type: choice
    label: "How soon to attack"
    values: ["soon", "steady", "late"]
    default: "steady"
    labels: ["Soon", "Steady", "Late"]
entry: brain.lua
```

`brain.lua` sits beside the manifest. It returns a table of hooks. Name
that table something other than `brain`, which is the global the profile
installs ([5.1 Phases](oabrain-standard.md#51-phases)).

The script reads the commander's build list and classifies records. A
metal extractor is a land record with `extractsmetal` above zero. An
energy building is a land record that makes energy, metal, wind or tide.
Land means `minwaterdepth` at most 0. A record that only stands in water
has no site beside a land commander, because `brain.place` returns nil
for it, so those records stay off the lists. A land factory is a
structure that builds (`builder`, `bmcode` 0, land) whose own build list
holds a land combat unit: it can move and attack, it is not a builder,
it cannot fly, and it stands on land. A record whose categories are only
words the commander already has is skipped, so the commander does not
build a copy of itself. An empty category list still counts.
`brain.random` picks among the records that qualify, and again for the
building's facing.

Easy builds one metal extractor and one energy building. Medium builds
two and one. Hard builds two and two. Then the commander builds one land
factory. The place call is
`brain.place(record, { x = builder.x, z = builder.z }, 200, facing)`,
with `facing` equal to `brain.random(4) - 1`. The centre that call
returns is what `unit:build` receives. When it returns nil, the same turn
tries the next building: energy after metal, then the factory.
`onUnitIdle` on the commander asks for the next building. `onUnitIdle`
on a structure that builds queues the combat unit. `onUnitFinished`
counts the building, or adds a combat unit to the squad, and sends the
squad when it is large enough. `brain.every` runs the same step every 30
ticks, once the first `start` has armed it. The squad's size is 4, 8 or
12 from the choice `soon`, `steady` or `late`. The squad attacks the
nearest living enemy on `me.seen`. `save` returns how many squads have
been sent. `fits` returns true.

`extractsmetal`, `energymake` and the other resource fields are `f32`
values. Read them with `:tonumber()` ([5.3 Numbers](oabrain-standard.md#53-numbers)).
`brain.place` wants a radius in whole world units and returns a cell and
that cell's centre, or nil. Pass the centre to `unit:build`. A position
component is a `fixed` value or a whole number.

```lua

-- Economy, then one land factory. Combat units gather until the attack
-- option's count is met, then go to the nearest enemy in sight.
-- save() keeps how many squads have been sent.

local hooks = {}
local sent, armed = 0, false
local commander, metal_type, energy_type, factory_type, combat_type
local metal_done, energy_done, factory_ordered = 0, 0, false
local factories, squad, sent_ids = {}, {}, {}
local ready = false

local function real_of(record, key)
    local value = record[key]
    if type(value) == "userdata" and type(value.tonumber) == "function" then
        return value:tonumber()
    end
    return type(value) == "number" and value or 0
end

local function positive(record, key)
    return real_of(record, key) > 0
end

local function words(record)
    local found, list = {}, record.category
    if type(list) ~= "table" then return found end
    for index = 1, #list do found[list[index]] = true end
    return found
end

-- A category the commander lacks, or no categories, so a copy is skipped.
local function labelled(record, commander_words)
    local any, fresh = false, false
    for word in pairs(words(record)) do
        any, fresh = true, fresh or commander_words[word] ~= true
    end
    return fresh or not any
end

local function land(record)
    return real_of(record, "minwaterdepth") <= 0
end

local function land_combat(record)
    return record.canmove == true and record.canattack == true and record.builder ~= true
        and record.canfly ~= true and land(record)
end

local function energy_record(record)
    return positive(record, "energymake") or positive(record, "makesmetal")
        or positive(record, "windgenerator") or positive(record, "tidalgenerator")
end

local function pick(list)
    if #list == 0 then return nil end
    return list[brain.random(#list)]
end

local function classify(list, commander_words)
    local metals, energies, plants = {}, {}, {}
    for index = 1, #list do
        local record = list[index]
        if record.commander ~= true and labelled(record, commander_words) then
            if positive(record, "extractsmetal") and land(record) then
                metals[#metals + 1] = record
            elseif energy_record(record) and land(record) then
                energies[#energies + 1] = record
            elseif record.builder == true and record.bmcode == 0 and land(record) then
                local built, fights = game.build_list(record), {}
                if type(built) == "table" then
                    for slot = 1, #built do
                        if land_combat(built[slot]) then fights[#fights + 1] = built[slot] end
                    end
                end
                local fight = pick(fights)
                if fight ~= nil then plants[#plants + 1] = { plant = record, unit = fight } end
            end
        end
    end
    return metals, energies, plants
end

local function squad_size()
    if me.options.attack == "soon" then return 4 end
    if me.options.attack == "late" then return 12 end
    return 8
end

local function plan_counts()
    if me.difficulty == "easy" then return 1, 1 end
    if me.difficulty == "hard" then return 2, 2 end
    return 2, 1
end

local function find_commander()
    local found = nil
    me:each_unit(function(unit)
        if found == nil and unit.type ~= nil and unit.type.commander == true then found = unit end
    end)
    return found
end

local function free(unit)
    local kind, _, idle = unit:head_order()
    return kind == nil or idle == true
end

local function place_build(builder, record)
    if builder.x == nil or builder.z == nil then return false end
    local facing = brain.random(4) - 1
    local _, _, x, z = brain.place(record, { x = builder.x, z = builder.z }, 200, facing)
    if x == nil then return false end
    return builder:build(record, { x = x, z = z }, facing) == true
end

local function note_finished(unit)
    local record = unit.type
    if record == nil or record.commander == true or unit.alive ~= true then return end
    if positive(record, "extractsmetal") then metal_done = metal_done + 1
    elseif energy_record(record) then energy_done = energy_done + 1
    elseif record.builder == true and record.bmcode == 0 then
        factories[unit.id], factory_ordered = unit, true
    elseif land_combat(record) and sent_ids[unit.id] ~= true then squad[unit.id] = unit
    end
end

local function nearest(origin)
    local best, best_distance
    for index = 1, #me.seen do
        local unit = me.seen[index]
        if unit.alive == true and unit.owner ~= me.player and me:allied(unit.owner) ~= true then
            local distance = brain.distance(origin, unit)
            if distance ~= nil and (best_distance == nil or distance < best_distance) then
                best, best_distance = unit, distance
            end
        end
    end
    return best
end

local function count_squad()
    local count = 0
    for _ in pairs(squad) do count = count + 1 end
    return count
end

local function try_send()
    if count_squad() < squad_size() then return end
    local origin = (commander ~= nil and commander.alive == true) and commander or nil
    if origin == nil then
        for _, unit in pairs(squad) do origin = unit break end
    end
    local target = origin ~= nil and nearest(origin) or nil
    if target == nil then return end
    for id, unit in pairs(squad) do
        if unit.alive == true then unit:attack(target) end
        sent_ids[id] = true
    end
    squad, sent = {}, sent + 1
end

local function prepare()
    if ready then return end
    if commander == nil or commander.alive ~= true then commander = find_commander() end
    if commander == nil or commander.type == nil then return end
    local list = game.build_list(commander.type)
    if type(list) ~= "table" or #list == 0 then return end
    local metals, energies, plants = classify(list, words(commander.type))
    metal_type, energy_type = pick(metals), pick(energies)
    local plant = pick(plants)
    if plant ~= nil then factory_type, combat_type = plant.plant, plant.unit end
    ready = metal_type ~= nil or energy_type ~= nil or factory_type ~= nil
end

local function build_next()
    prepare()
    if commander == nil or commander.alive ~= true then commander = find_commander() end
    if commander == nil or not free(commander) then return end
    local metal_need, energy_need = plan_counts()
    if metal_type ~= nil and metal_done < metal_need then
        if place_build(commander, metal_type) then return end
    end
    if energy_type ~= nil and energy_done < energy_need then
        if place_build(commander, energy_type) then return end
    end
    if factory_type ~= nil and not factory_ordered then
        if place_build(commander, factory_type) then factory_ordered = true end
    end
end

local function queue_plants()
    if combat_type == nil then return end
    for _, plant in pairs(factories) do
        if plant.alive == true and free(plant) then plant:queue(combat_type) end
    end
end

local function step()
    build_next()
    queue_plants()
    try_send()
end

function hooks.fits() return true end

function hooks.start(_, _, saved)
    sent = type(saved) == "table" and type(saved.sent) == "number" and saved.sent or 0
    metal_done, energy_done, factory_ordered, factories, squad = 0, 0, false, {}, {}
    metal_type, energy_type, factory_type, combat_type = nil, nil, nil, nil
    ready, commander = false, find_commander()
    prepare()
    me:each_unit(function(unit)
        if unit.finished == true then note_finished(unit) end
    end)
    if not armed then
        brain.every(30, step)
        armed = true
    end
end

function hooks.onUnitFinished(unit)
    note_finished(unit)
    try_send()
end

function hooks.onUnitIdle(unit)
    local record = unit.type
    if record == nil then return end
    if record.commander == true then
        commander = unit
        build_next()
    elseif record.builder == true and record.bmcode == 0 then
        factories[unit.id] = unit
        queue_plants()
    end
end

function hooks.save() return { sent = sent } end

return hooks
```

Pack from outside the package folder. An existing `--out` without
`--force` stops with
`oa-tool brain pack: the output already exists: ` and the path.

```sh
build/oa-tool brain pack brains/examples/steady-commander --out /tmp/brain-scratch/steady-commander.oabrain
```

```text
packed /tmp/brain-scratch/steady-commander.oabrain: 2 files, 2822 bytes, sha256 e691879fd12193d72b01d2465948da5698980fca37804dcb1bf5272570856969
```

Check the package against the base game:

```sh
build/oa-tool brain check /tmp/brain-scratch/steady-commander.oabrain --game-dir "$OA_GAME_DIR"
```

```text
brain: steady-commander 1.0 level easy
game: totala
mod: none
load: ok, 0 instructions
fits: ok, 0 instructions
start: ok, 15 instructions
lua: 1231655 bytes
networks: 0 bytes
result: ok
```

The check exits 0. [11. Compatibility](oabrain-standard.md#11-compatibility)
says `*` fits any mod and the base game alone, and the Library's
**Works with** line for this manifest is **Any mod**. `fits: ok` is
`hooks.fits` returning true. Load and start both ran. The manifest keeps
`*`, which is what lets the script play another mod.

The first match seats the folder against Classic on Painted Desert:

```sh
open-annihilation --skip-intro --mute --headless-check --game-dir "$OA_GAME_DIR" \
  --user-folder /tmp/brain-user --data-dir /tmp/brain-scratch/data \
  --brain-match brains/examples/steady-commander,brains/classic \
  --brain-levels medium,medium \
  --brain-map "Painted Desert" --seed 4242 --match-ticks 1800 \
  --brain-record /tmp/brain-scratch/record.txt
```

It exits 0:

```text
brain match: map Painted Desert seed 4242 ticks 1800
brain match: seat 1 steady-commander 1.0 level medium side 0
brain match: seat 2 classic 1.0 level medium side 1
brain match: end tick 1800 winner none reason time
brain match: seat 1 units 6 built 6 lost 0 kills 0 metal 9155 energy 8064 metal-produced 45 energy-produced 1492 instructions 746835 status playing
brain match: seat 2 units 4 built 4 lost 0 kills 0 metal 943 energy 542 metal-produced 94 energy-produced 1234 instructions 11247757 status playing
brain match: digest a166a0bb22a312d5
```

`status playing` means the script stayed inside its budget for the whole
match ([5.2 Budgets](oabrain-standard.md#52-budgets)). Seat 1 has built 6
units. Medium asks for two metal extractors and one energy building, then
one land factory, and that factory queues a combat unit. Steady Commander
never reads `me.knowledge`. The record of this match is quoted in
[11. Testing](#11-testing).

The same folder against the built-in player is the run whose trace shows
Steady Commander's own orders. Classic's log of calls is a long one; the
built-in seat's orders sit in the same file and the example's lines are
the `seat 0 brain` blocks.

```sh
open-annihilation --skip-intro --mute --headless-check --game-dir "$OA_GAME_DIR" \
  --user-folder /tmp/brain-user --data-dir /tmp/brain-scratch/data \
  --brain-match brains/examples/steady-commander,builtin \
  --brain-levels medium,hard \
  --brain-map "Painted Desert" --seed 4242 --match-ticks 2400 \
  --brain-trace /tmp/brain-scratch/trace-play.txt \
  --brain-record /tmp/brain-scratch/record-play.txt
```

It exits 0:

```text
brain match: map Painted Desert seed 4242 ticks 2400
brain match: seat 1 steady-commander 1.0 level medium side 0
brain match: seat 2 built-in level hard side 1
brain match: end tick 2400 winner none reason time
brain match: seat 1 units 7 built 7 lost 0 kills 0 metal 9108 energy 7846 metal-produced 60 energy-produced 2206 instructions 965058 status playing
brain match: seat 2 units 5 built 5 lost 0 kills 0 metal 703 energy 656 metal-produced 195 energy-produced 3225 instructions - status built-in
brain match: digest 8c8a9e89f53f3ff1
```

Tick 1 calls `onUnitIdle`, draws while it classifies, and places the
first building:

```text
tick 1 seat 0 brain
call onUnitIdle 1
own-draw 1 6 1
own-draw 1 4 4
own-draw 1 1 1
own-draw 1 3 1
own-draw 1 2 1
own-draw 1 4 2
build 1 78 116916224 0 63438848
```

`own-draw 1 n` is `brain.random(n)`
([`--brain-trace`](../command-line.md#--brain-trace-and---brain-record)).
The draws pick a qualifying record and a facing. `build` is `unit:build`
([the command line](../command-line.md#--brain-trace-and---brain-record)).
Two more economy placements follow as each one finishes. The fourth
`build` is the land factory, once medium's two extractors and one energy
building are done:

```text
tick 568 seat 0 brain
call onUnitFinished 4 1
call onUnitIdle 1
own-draw 1 4 3
build 1 68 116391936 0 69206016
```

The factory then queues the combat unit. One timer after it finishes:

```text
tick 1440 seat 0 brain
call timer
queue 5 95 1
```

`queue` is `unit:queue`. How to read a report is
[11. Testing](#11-testing). The words on each line are
[the command line](../command-line.md#--brain-match).

## 2. Reading the game

`game` and `me` are the two tables a match gives the script. What each
field holds is [7. What a brain may know](oabrain-standard.md#7-what-a-brain-may-know).
`game.build_list` returns the build list of a unit record, a name or an
index, after the mod and the mutators have changed it. `game.mod` names
the mod. The `ai.*` hacks are part of that game data. Read them through
the fields the standard lists, and link those sections rather than
copying the tables.

Read records, categories and definition fields. A manifest that says `*`
is claiming the script still runs when a mod renames every unit and
changes what a factory builds. A unit name written into the script plays
one game. Steady Commander never writes one. `category` is a list of
strings. `commander`, `builder`, `canmove`, `canattack` and `canfly` are
booleans. `bmcode` is 0 for a structure. `minwaterdepth` is an `f32`.

`me.player` is the seat. `me:each_unit` walks the seat's own units.
`me:allied` answers another player. `me.seen` is the units the seat can
know about. During `start`, before the first tick, `each_unit` can still
be empty: the commander arrives as `onUnitIdle` on tick 1. Classify then,
or from the timer, and remember the records so later ticks do not draw
again.

## 3. Giving orders

Orders are methods on a unit handle. A factory queues with `unit:queue`.
A builder places with `unit:build`. A combat unit attacks with
`unit:attack`. The full list, and the charge on each call, is
[8. What a brain may do](oabrain-standard.md#8-what-a-brain-may-do).

`brain.place(type, near, radius [, facing])` starts at the footprint
nearest `near` and walks outward. It returns the cell and the centre, or
nil. `facing` is 0 to 3. Steady Commander passes the builder's `x` and
`z`, a radius of 200 and `brain.random(4) - 1`. `brain.random` is never
called with 0. On a seat without full awareness the site test is the
building site with this seat as the placing player. A nil return leaves
that building unplaced, and the same turn tries the next one.

A refused order returns false and is counted. It is not raised. The
reasons include a dead unit, a unit the seat does not own, the order
limit, a target the seat has not seen, a type that unit cannot build, and
a refusal from the match. `unit:head_order` returns the kind and whether
that order is idle. A finished order can sit at the head with idle true,
so a builder is free when the kind is nil or idle is true. Steady
Commander uses that test before it builds or queues, and it gives one
order at a time.

An order takes effect on the next tick
([6. Hooks](oabrain-standard.md#6-hooks)).

## 4. Events and timers

One turn is one banked call, at the seat's controller point. The turn
delivers queued unit events, then radar and sonar changes, then alliance
changes, then defeats, then chat, then timers, then `onTick`. The queued
hooks run in the order the events fired. The table of hooks and their
arguments is [6. Hooks](oabrain-standard.md#6-hooks).

Steady Commander uses three of them. `onUnitFinished` sees a unit of this
seat complete. `onUnitIdle` sees one of this seat's units with nothing to
do. `onTick` is left unset. A missing hook is skipped.

`brain.after(ticks, fn)` runs once. `brain.every(ticks, fn)` runs first
that many turns later, then again at the same interval. Ids start at 1
and only grow. `brain.cancel` removes a waiting timer. The limits and the
error text are [5.5 Timers](oabrain-standard.md#55-timers). Due timers run
after chat and before `onTick`.

A long plan has to fit the turn's allowance and the bank of unused ticks.
Both numbers are [5.2 Budgets](oabrain-standard.md#52-budgets). Split the
work: do one placement a turn, and let `brain.every` carry the rest. An
error in a timer stops the seat the way a hook error does.

`ReloadAIProfiles` calls `start` again in the same sandbox
([6.1 save and report](oabrain-standard.md#61-save-and-report)). A second
`brain.every` would stack another timer on the first. Steady Commander
keeps a flag and arms the timer once. A saved game loads in a new
sandbox, so the flag starts clear and the timer is armed again.

## 5. Seeing the enemy

`me.seen` is the list the seat may know. Indexes start at 1. Attack a
unit on that list. An attack on a unit the seat has not seen is refused.
`onUnitSeen` and `onUnitLost` fire as the picture changes. Radar and
sonar contacts are `onRadarContact` and `onRadarLost`, after the queued
events, lost before gained for the same slot. An allied unit is never a
contact. The sight grid and the fields on a contact are
[7. What a brain may know](oabrain-standard.md#7-what-a-brain-may-know).

`fullAwareness: true` is the manifest saying the brain sees every unit,
the way the built-in computer player does, and that its site checks do
not need a placing player. Leave it out, as Steady Commander does, when
the plan can live on `me.seen`. Set it when the plan needs units in the
fog. It is allowed in a network game. The seat shows it: the Library chip
**Full awareness**, the fact
"Sees every unit on the map, as Total Annihilation's own computer player does",
and the picker's shorter line "Sees every unit on the map". The flag is
the manifest's own. It is not one of the calls only Classic is given
([3.3 fullAwareness](oabrain-standard.md#33-fullawareness) and
[7.7 Full awareness](oabrain-standard.md#77-full-awareness)).

`brain.distance` returns a `fixed` length. Compare two lengths with `<`.
Steady Commander uses that to keep the nearest enemy.

## 6. Levels and options

`me.level` is the level id from the manifest. `me.difficulty` is that
level's `easy`, `medium` or `hard`. The seat uses that value as its
income rules, the same difficulty the game already uses
([3.2 Levels](oabrain-standard.md#32-levels)). Steady Commander reads
`me.difficulty` to decide how many economy buildings to start.

`me.options` holds the options for this seat. A choice's value is the
text of the chosen entry, so `me.options.attack` is `"soon"`, `"steady"`
or `"late"`. A toggle is a boolean. A number option, or a choice whose
values are all numbers, arrives as a number. The shapes are
[3.5 Options](oabrain-standard.md#35-options) and
[7.5 The seat](oabrain-standard.md#75-the-seat).

The picker shows the level and the options for the brain the player
highlights. **USE THIS** hands that choice back. The Library's brain
details list levels and do not list options; the options live on the
picker.

## 7. Fitting a mod

`works-with` is the manifest's claim about which games the package
plays. `*` is any mod and the base game alone. `ta-3.1c` is the base
game with no mod. Any other id is a mod, with a range when one is
written. `*` beside another entry is refused: it is the only entry.
`requires.expansions` names expansions that have to be on.
`requires.engine` names a build range. The grammar is shared with
mutators and linked from
[11. Compatibility](oabrain-standard.md#11-compatibility).

`fits(game)` runs after the package has loaded and before `start`.
Return true, or a string. The string is the reason the seat shows, in
"{brain} does not fit this game ({reason}); the engine plays this seat".
Anything else stops seating with `fits must return true or a reason`.
The data `fits` reads is the match's data, after the mod and the
mutators. Steady Commander returns true. Its claim to play any mod is
the `works-with` entry plus the script reading build lists instead of
names.

The Library's **Checked** fact, after **CHECK WITH THIS MOD**, uses the
same sentences a mutator uses. A brain that fits fills `{summary}` with
"it fits this game", so the fact is "With {mod}: {summary}", or "With No
Mod: {summary}" when no mod is playing.

The check quoted in [1. A first brain](#1-a-first-brain) is this package
on the base game with no `--mod`. `result: ok` and exit 0 are that run.
`fits: ok` is the script returning true. A package that defines no
`fits` reports `fits: none` for that phase and can still finish
`result: ok`. Classic is that case:

```sh
build/oa-tool brain check brains/classic --game-dir "$OA_GAME_DIR"
```

```text
brain: classic 1.0 level easy
game: totala
mod: none
load: ok, 119 instructions
fits: none, 0 instructions
start: ok, 10223791 instructions
lua: 2410674 bytes
networks: 0 bytes
result: ok
```

## 8. Keeping state across a save

`save()` takes no arguments. It returns plain data: nil, booleans,
integers, floats, strings, `fixed` counts and tables of those. Steady
Commander returns `{ sent = sent }`, the number of squads it has sent.
The types, the 1 MiB cap and the refusals are
[10. Saved games and recordings](oabrain-standard.md#10-saved-games-and-recordings).

`start(game, me, saved)` receives that value, or nil when the seat has
none. `saved` is kept only when the installed version and the package
digest both match the save. Timers and the brain's own random stream are
not kept. The brain starts again from `start`. Read `saved` before you
use it: a missing seat, a damaged value or a version that does not match
leaves it nil, and the run prints that the saved state was left out.

A loaded game is a new sandbox. Arm timers again inside `start`. Do not
keep unit handles in the saved table. Look the units up from `me` after
the load.

What the save file and the content note name, for a player, is
[Playing with brains](../playing-with-brains.md#saves-and-recordings).

## 9. Chat

`onChat(from, text, to)` runs once per line since the last turn. `from`
is the sender's index. `text` has no name on the front. `to` is `all`,
`allies`, `enemies` or `me`. A missing `onChat` is skipped, and the line
is still consumed. At most 32 lines wait.

`brain.say(text [, to])` hands one line to the match. `to` is `all` by
default, or `allies`, `enemies`, or a player index from 0 to 9. The call
returns true when the line was queued. The match paces lines a computer
seat says. The interval, and the refusals (`too_soon` among them), are
[8.2 Chat](oabrain-standard.md#82-chat). The call does not pace itself.

## 10. Learned networks

A network is trained elsewhere. `oa-tool brain net` reads an ONNX model
and writes a `.oann`. This is `oa-tool brain net --help`:

```text
oa-tool brain net MODEL.onnx --calibrate DATA [--out FILE] [--weights int8|int16] [--max-difference X] [--force] [--json]

Reads MODEL.onnx, a model of the default domain at opset 11 to 21, and writes a .oann. The layers are Gemm and MatMul (a following Add of a constant is the bias), Conv, MaxPool, Relu, LeakyRelu, Tanh, Sigmoid, Softmax, Add, Concat, Flatten, Reshape, ArgMax and Gather. Identity and Dropout are skipped. The first dimension of every input is the batch and is removed. --calibrate is a file of JSON lines; a line whose kind is sample holds features as whole numbers in 65536ths. Weights are int8 unless --weights int16 is set. The integer network is run on the same samples, and the report gives each output's largest difference, or the share of samples an argmax agrees on. --max-difference refuses a larger difference and writes nothing. Without --out the file is the model's path with .oann. An existing file needs --force. --json prints one object. Exit 0 when the file was written and every difference is within the limit. Exit 1 when the model cannot be converted or a difference passes the limit. Exit 2 when the arguments are not the command's.
```

The calibration file is the `--calibrate` argument. The report is the
largest difference on each output, or the share of samples an argmax
agrees on. `--max-difference` refuses a larger difference and writes
nothing.

List the file in the manifest with its `sha256`
([3.4 Networks](oabrain-standard.md#34-networks)). The bytes have to hash
to that digest. Call it with `brain.network`. The arguments, the grids
and what a call costs are [9. Neural networks](oabrain-standard.md#9-neural-networks).
An option can leave the call unused: read `me.options` and skip the
network when the player switches it off. The network is still loaded
with the package.

This guide does not walk through a learned example. That is a later pass.

## 11. Testing

`build/oa-tool help brain` lists the three commands:

```text
oa-tool brain pack FOLDER [--out FILE] [--force]
    Pack a brain folder as a .oabrain.
oa-tool brain check FILE --game-dir DIR [--mod FOLDER] [--level LEVEL] [--side N] [--json]
    Load a brain against a game and report how it started.
oa-tool brain net MODEL.onnx --calibrate DATA [--out FILE] [--weights int8|int16] [--max-difference X] [--force] [--json]
    Convert an ONNX model to a .oann.
```

### Check

`brain check` loads a `.oabrain`, or a folder whose top holds
`oabrain.yaml`, against a game. It seats the brain on a flat stand-in
map and never ticks that match. The report quoted in
[1. A first brain](#1-a-first-brain) is the run against the base game
with no `--mod`. `result: ok` and exit 0 are that run. A phase that
did not run says `not run`. A brain with no `fits` hook says `fits: none`,
which is Classic's line in [7. Fitting a mod](#7-fitting-a-mod).
`--json` prints one object. `--mod FOLDER` checks against a mod.
`--level` picks a level id, and the first level when it is left out.

### A headless match

`--brain-match` plays two computer seats. Each seat is an installed
brain id, a folder (a path that holds `/` or `\`, or starts with `.`),
or `builtin`. It needs `--headless-check`, `--match-ticks` and
`--brain-map`. Both reports are quoted in
[1. A first brain](#1-a-first-brain).

Every line starts with `brain match: `. `winner none` and `reason time`
mean both seats still had units when the tick count was spent. `units`
counts the living units. `built` counts units the seat has built. In the
Classic match Steady Commander's `built` count is 6, and in the built-in
match it is 7. `instructions` is what the brain's calls have used, or
`-` for a built-in seat. `status playing` means the brain was still
being called. `digest` is the world digest. Status 0 means the match
played to the end. The rest of the words are
[the command line](../command-line.md#--brain-match).

The match against Classic, quoted above, exits 0 and prints that report.

### Trace and record

`--brain-trace` and `--brain-record` ride on a match. Both need
`--headless-check` and `--match-ticks`. The trace of the built-in match
begins:

```text
brain log 1
tick 0 seat 0 brain
call fits
tick 0 seat 0 brain
call start
```

Tick 1 is the `onUnitIdle` excerpt in
[1. A first brain](#1-a-first-brain). A timer is a line `call timer`.
Report seat 1 is log seat 0. The line shapes are
[the command line](../command-line.md#--brain-trace-and---brain-record).

The Classic match's record is three JSON lines:

```text
{"kind":"header","format":1,"map":"Painted Desert","seed":4242,"seats":[{"seat":1,"brain":"steady-commander","version":"1.0","level":"medium"},{"seat":2,"brain":"classic","version":"1.0","level":"medium"}]}
{"kind":"result","seat":1,"winner":false,"tick":1800,"units":6}
{"kind":"result","seat":2,"winner":false,"tick":1800,"units":4}
```

The built-in match's record is the same shape:

```text
{"kind":"header","format":1,"map":"Painted Desert","seed":4242,"seats":[{"seat":1,"brain":"steady-commander","version":"1.0","level":"medium"},{"seat":2,"brain":"builtin","level":"hard"}]}
{"kind":"result","seat":1,"winner":false,"tick":2400,"units":7}
{"kind":"result","seat":2,"winner":false,"tick":2400,"units":5}
```

Steady Commander never calls `brain.record`, so there is no `sample`
line. The two `result` lines are written because the match played to its
report.

### The seat log

The app writes `<data>/logs/brains/seat-<n>.log`, where n is the player
index plus one. The file starts anew for each brain seated there. The
first line of seat 1 after the built-in match is:

```text
Steady Commander 1.0 (steady-commander), level medium, seat 1, match seed 4242
```

The script does not call `print`, so the file holds that line alone.
A printed line is appended as the tick and the text. The path, the
1 MiB cap and the closing line are
[5.6 print and the log](oabrain-standard.md#56-print-and-the-log).
On this run the log was written in the account's data folder. The
match's `--data-dir` did not move it.

### The console

`PrintWeights` writes `report()` into the named file. Steady Commander
defines no `report`, so the file would hold one line: the manifest name,
a space, the version, and ` gives no report.`. A report that fails
writes nothing.

`ReloadAIProfiles` stores the reloaded profile in `game.ai_profile` and
calls `start` again with what `save` returns now. On success the run
prints `open-annihilation: brains: player `, the player index, and
` starts again with the reloaded profile`.

Both are [6.1 save and report](oabrain-standard.md#61-save-and-report).
This guide does not quote a console session. A brain match cannot be
combined with `--stage`, and these runs did not open the console.

### A seat from the command line

`--computer-player` seats an installed brain by id. A folder path is not
accepted there. `core:2::hard:steady-commander` seats that installed
brain at the level `hard` gives. A brain that is not installed refuses
the launch. One that does not fit plays the built-in player. The fields
are [Launch settings](../command-line.md#launch-settings). This guide
does not quote a launch: the runs above seat the folder through
`--brain-match`.

### CHECK WITH THIS MOD

**CHECK WITH THIS MOD** is a button on an installed brain in the
Library, shown when a verdict is set. It loads that copy and runs
`fits`. It does not seat the brain. It is off during a game and while
the check is running. The **Checked** fact then reads one of the
sentences in [7. Fitting a mod](#7-fitting-a-mod). There is no command
output to quote. The button's words are the screen's.

## 12. When a brain fails

A brain stops for an error in a hook or a timer, a spent instruction
allowance, the real-time backstop, the Lua memory cap, a string past its
limit, or a load the seat refuses. The sentences for the allowance, the
clock and the memory cap are
[5.2 Budgets](oabrain-standard.md#52-budgets).

A failed turn closes the sandbox and clears the hooks before that turn
returns. The failing turn still counts as played. The next order tick
has no hooks, so the built-in computer player takes it. That player
already has a controller for the seat. The squad sort follows 30 order
ticks later and reads the squads the brain left.

A brain that is refused, fails to start, or does not fit never has
hooks, so the built-in player plays from the start. `fits` and `start`
do not report a stop. The app posts the seating notice.

The owner sees one of these, with the manifest name and the reason:

- "{brain} stopped ({reason}); the engine plays this seat on"
- "{brain} could not start ({reason}); the engine plays this seat"
- "{brain} does not fit this game ({reason}); the engine plays this seat"

When the seat has no reason of its own, the reason is
`the brain could not be seated`. The log writes `tick `, the tick,
`: stopped: `, the error, and the traceback under it. The rest is
[12. When a brain fails](oabrain-standard.md#12-when-a-brain-fails).

In the headless report a stopped brain uses `status stopped-at` and the
tick, and a following line names the script and the line. Steady
Commander's match stayed `playing`.

## 13. Classic, a walkthrough

Classic is `brains/classic/`. It is documentation of the built-in
computer player: the same orders, written so you can read them. The
built-in player stays in the engine and stays the default. Choosing
Classic seats this package. Leaving the seat on the built-in player
runs the engine's own computer player.

The manifest says `works-with: ["*"]` and `fullAwareness: true`. Its
description says it plays exactly as the built-in computer player does.
Full awareness is the manifest flag. It is allowed in a network game,
and the seat flags it.

### The files

- `oabrain.yaml`, `brain.lua`, `LICENSE`, `README.md`
- `lua/build.lua` scores a type and picks one from a builder's list
- `lua/knowledge.lua` holds weights, limits, the build grids and the
  metal spots, and reads the engine's record from `me.knowledge`
- `lua/place.lua` searches for a site, widening until the search is as
  wide as the map
- `lua/profile.lua` reads the profile text and applies weights
- `lua/report.lua` writes the text `report()` returns
- `lua/rules.lua` reads the `ai.*` rules Classic plays by
- `lua/squads.lua` sorts units into squads and gives a squad an order
- `lua/types.lua` reads the type fields the computer player uses
- `lua/tasks/structures.lua`, `construction.lua`, `strike.lua`,
  `rally.lua`, `air_raid.lua` and `siege.lua` are the squad tasks

`brain.lua` builds one task for each squad from 0 to 9. On the first tick
`onTick` refreshes the build lists and seeds the grids. Each tick then
runs the squad sort on its countdown and runs each task whose turn has
come.

### The task table

```lua
-- …
    for index = 0, 9 do
        seat.tasks[index] = blank(index)
    end
    local wave = loaded_rules.attack_wave
    seat.tasks[1].kind = "structures"
    seat.tasks[1].every = 30
    seat.tasks[2].kind = "strike"
    seat.tasks[2].min_size = 3
    seat.tasks[2].launch = wave
    seat.tasks[2].merge_radius = 20000
    seat.tasks[2].source = 3
    seat.tasks[2].every = 300
    seat.tasks[3].kind = "rally"
    seat.tasks[3].source = 2
    seat.tasks[3].every = 150
    seat.tasks[4].kind = "construction"
    seat.tasks[4].every = 90
    if loaded_rules.squad5_factory_tick then
        seat.tasks[5].kind = "structures"
        seat.tasks[5].every = 30
    else
        seat.tasks[5].kind = "idle"
    end
    seat.tasks[6].kind = "strike"
    seat.tasks[6].min_size = 3
    seat.tasks[6].launch = wave
    seat.tasks[6].merge_radius = 50000
    seat.tasks[6].source = 7
    seat.tasks[6].every = 300
    seat.tasks[7].kind = "rally"
    seat.tasks[7].source = 6
    seat.tasks[7].every = 150
    seat.tasks[8].kind = "air_raid"
    seat.tasks[8].every = function()
        return 30 + brain.shared_random(900)
    end
    seat.tasks[8].run = function()
        air_raid.run(seat, seat.tasks[8])
    end
    -- @quirk The sort never fills squad 9, so the siege task only keeps its
    -- timetable unless a unit was already in that squad.
    local centre = map_centre(game)
    seat.tasks[9].kind = "siege"
    seat.tasks[9].every = function()
        return 30 + brain.shared_random(150)
    end
    seat.tasks[9].siege_target = point_of(centre)
    seat.tasks[9].siege_probe = point_of(centre)
    seat.tasks[9].siege_step = point_of(centre)
    seat.tasks[9].siege_weight = 0
-- …
```

Squads 1 and 5 build structures. Squad 5 does that only when the rules
say so, and otherwise idles. Squads 2 and 6 are strike groups. Squads 3
and 7 rally onto them. Squad 4 is construction. Squad 8 is the aircraft
wing. Squad 9 is siege. The comment on squad 9 is a quirk: the sort
never fills that squad, so the siege task only keeps its timetable
unless a unit was already in it.

On the first tick `onTick` refreshes the build lists and seeds the grids.
The sort itself is the countdown. It starts at 30. Each tick subtracts
one, and at the bottom it returns to 30 and calls `squads.sort`.

```lua
-- …
function hooks.onTick(tick)
    local seat = hooks.seat
    if seat.builds_ready ~= 1 then
        types.refresh_builds(seat.types, seat.game)
        seat.builds_ready = 1
    end
    if seat.grids_ready ~= 1 then
        knowledge.seed_grids(seat)
    end
    seat.sort_countdown = seat.sort_countdown - 1
    if seat.sort_countdown < 1 then
        seat.sort_countdown = 30
        brain.section("sort")
        squads.sort(seat)
    end
    for index = 0, 9 do
        local task = seat.tasks[index]
        if task.kind ~= "none" and task.next <= tick then
            if task.kind ~= "idle" then
                brain.section("task" .. index)
                local interval = task.every
                if type(interval) == "function" then
                    interval = interval()
                end
                task.next = (tick + interval) & 0xFFFFFFFF
                task.run()
            end
        end
    end
end
-- …
```

`squads.lua` says what the sort does: selectable units in the seat's
range take standing orders, and a unit still in squad 0 joins the squad
its type sorts into. A unit already in a squad keeps that squad.

### Profile, knowledge, scoring and placement

`profile.apply_all` reads the profile and applies the weights. Two
quirks sit in that file. The first is how the word `any` is read on a
`plan` line:

```lua
-- …
-- @quirk "any" counts only as the first word after "plan", and that same
-- word is tested again on every later word of the line.
local function plan_line(tokens, keyword)
    local matches = false
    for index = 2, #tokens do
        if equal_nocase(tokens[2], "any") then
            matches = true
-- …
```

The second applies a downloadable type's `ai_weight` once per computer
player, so the weight compounds by that count:

```lua
-- …
    -- @quirk A downloadable type's ai_weight is applied once per computer
    -- player for each lock table, so a weight compounds by that count.
    for _ = 1, computers do
        seat.plan = profile.downloadable(seat, seat.weight_locked, seat.plan, game)
        seat.plan = profile.downloadable(seat, seat.limit_locked, seat.plan, game)
    end
-- …
```

`knowledge.lua` keeps the per-type weights and the metal spots.
`knowledge.scan_metal` reads `map:metal_spots()` during `start`. Owned
counts, the builder count and the base position come from `me.knowledge`
when a task asks for them, on a tick, after the match has prepared the
computer players. The match in [1. A first brain](#1-a-first-brain)
plays to its report.

`build.lua` scores a type from the stores, the income and the type's
strength, then walks the builder's list once. `place.lua` grows a
search until it is as wide as the map. An extractor looks at metal
deposits when the map's surface metal loses a draw. Anything else tries
the build grid thirty times.

### One task

`lua/tasks/rally.lua` is the whole of the rally task. When the rally
squad and the strike group both have members, the rally squad moves to
the strike group's centre.

```lua
-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- A waiting squad. When it and the strike group both have members, it moves
-- to the strike group's centre. The timetable is already set.

local squads = require("squads")

local rally = {}

function rally.run(seat, task)
    local me = seat.me
    if squads.count(me, task.squad) < 1 or squads.count(me, task.source) < 1 then
        return
    end
    local center = squads.centroid(me, task.source)
    if center == nil then
        return
    end
    squads.order(me, task.squad, "move", false, nil, squads.fixed_point(center))
end

return rally
```

### The quirk notes

A line marked `@quirk` records a behaviour the built-in computer player
has. The note is in the script so a reader sees it beside the code.
Construction withholds builds, and still patrols:

```lua
-- …
-- @quirk ai.builder-withhold-threshold moves only the build limit: an idle
-- capturing builder still circles the base from five build-capable units.
-- …
```

A small aircraft wing with no base patrols the map edge:

```lua
-- …
    -- @quirk Under ai.patrol-null-enemy-skip a small wing with no base draws
    -- a map-edge patrol, the same three draws a large wing takes, and patrols
    -- there instead of queueing a patrol on the nearest enemy.
-- …
```

The squad 9 note and the two profile notes are quoted above.

### What only Classic is given

A seat is Classic only when its files are this build's `brains/classic/`
folder, the same paths and the same bytes. The id proves nothing. That
test grants two behaviours, and a third draw from the set-up stream.
The list, the error text and the reason are
[13. Classic](oabrain-standard.md#13-classic).

- `brain.shared_random` draws the match's shared stream, the one unit
  scripts and the simulation draw from. Classic's aircraft and siege
  intervals use it, so the draws stay in step with the built-in player.
- `unit:stance` with `direct = true` writes the standing move and fire
  bits and gives no order. The built-in player writes those bits that
  way. Any other seat is refused.
- `brain.setup_random` draws from a copy of that stream taken at the
  seat's built-in set-up, and advances only the copy.

Any other brain has neither random function, and a direct stance write
is refused. `brain.section` is installed for every brain. In a game the
mark does nothing. Full awareness is not in this list.

Steady Commander calls none of them.

### How it is proven

In shadow, Classic is seated beside the built-in player. The built-in
player's orders are the ones the match plays. Classic's orders are
recorded and compared. No tick may differ. The shadow host's log of
calls is what the comparison reads. This tree registers that work as
`sim-brains-shadow` and `sim-brains-shadow-match`. A shadow run from the
command line is
[`--brain-shadow`](../command-line.md#--brain-shadow).

Swapped in, the tests that seat Classic where the built-in player would
play are the ones this tree's `CMakeLists.txt` registers:
`sim-brains-classic`, `sim-brains-classic-data`,
`sim-brains-classic-match`, `ai-computer-player`,
`match-computer-build`, `native-trace`, `native-stage`,
`native-director-render`, `native-classic-trace`, `native-classic-stage`,
`native-classic-director-render`, `native-net-loopback-computer`,
`native-net-loopback-computer-watcher`, `native-net-loopback-each-ai`,
`native-net-loopback-computer-classic`,
`native-net-loopback-computer-watcher-classic` and
`native-net-loopback-each-ai-classic`.

A soak plays 1,000 seeded headless skirmishes, 20,000 ticks each, over
the base game and the published mods. The same seed plays the same
match. The soak is the long check that a seat running Classic stays on
the built-in player's orders for a whole game.

The headless match quoted in [1. A first brain](#1-a-first-brain) seats
Steady Commander beside Classic and plays to its report. Shadow, and the
tests named above, compare Classic's orders with the built-in player's.

### Reading it

Read Classic as the built-in computer player's behaviour, written down.
The engine's own computer player is still the player a seat uses when
nobody chose a brain. Installing Classic, or packing it, changes
nothing until a seat is set to it.

## 14. Publishing

A registry serves a brain as one package kind. It installs into
`Brains/<id>`. The rules for the archive, the catalogue entry and the
download are [Packages](../registries/packages.md) and
[the catalogue](../registries/catalogue.md).

The package page can point readers here. The sections above are the
ones to link: the first brain, the match, and Classic's walkthrough.
Classic is published as an example. The built-in computer player stays
the default.
