# The OABrain standard

An OABrain (`.oabrain`) is a package that plays a computer player: a Lua
5.4 script in the engine's sandboxed script host, called on the
simulation's ticks and unit events, giving orders through the same paths a
player's orders take. It sits beside `.oamod`, `.oamap`, `.oamut`,
`.oaexp` and `.oalang`. Brains are opt-in. The engine's own computer
player stays the default and stays in the engine. Classic is an example.

This document is the format and its rules as the engine implements them.
The archive a package is shipped in, and the catalogue entry a registry
publishes, are described with the other package kinds. This standard names
what is different for brains and does not repeat those rules.

## Contents

- [1. Goals and principles](#1-goals-and-principles)
- [2. The package](#2-the-package)
- [3. The manifest](#3-the-manifest)
- [4. Where a brain runs](#4-where-a-brain-runs)
- [5. The runtime](#5-the-runtime)
- [6. Hooks](#6-hooks)
- [7. What a brain may know](#7-what-a-brain-may-know)
- [8. What a brain may do](#8-what-a-brain-may-do)
- [9. Neural networks](#9-neural-networks)
- [10. Saved games and recordings](#10-saved-games-and-recordings)
- [11. Compatibility](#11-compatibility)
- [12. When a brain fails](#12-when-a-brain-fails)
- [13. Classic](#13-classic)
- [14. Checks](#14-checks)
- [15. Versioning](#15-versioning)
- [16. Glossary](#16-glossary)

## 1. Goals and principles

A brain plays one computer seat. The player picks it. The engine runs it.
It can only do what a player could do.

| Goal | What it means in practice |
| --- | --- |
| Opt-in | The built-in computer player stays the default and stays in the engine. A brain plays only on a seat a player set to it. An empty brain id is that built-in player. |
| One seat | A brain controls the seat it was given. It does not steer another player's units. |
| Orders a player could give | Every action reaches the match through the same order paths a player's orders take. An order takes effect in the next tick. |
| One machine | Only the machine that seats the computer player runs the script. Other machines mirror the seat's units. A recording played back does not run the script. |
| The same draws on that machine | The brain's own random stream is seeded from the match. A real-time limit can stop a script on a slow machine and let it finish on a fast one. The orders that do come back do not depend on the machine. |
| Lua and data only | A brain is Lua source and data. It has no native code, no file, no network and no clock. |
| Classic is an example | Classic is written in Lua and plays as the built-in computer player does. It is documentation of that player, not a replacement. |

The principles that follow:

1. **The built-in player remains.** A seat with no brain, a brain that does
   not fit, and a brain that stops are all played by the engine's own
   computer player. Campaign missions follow the default brain the player
   set, the same way a skirmish does.
2. **One manifest.** `oabrain.yaml` is what the packer writes, what the
   installer checks and what the Library lists. A key spelled wrong is
   refused. A key that belongs in a mod or a mutator is refused with its
   own message.
3. **Full awareness is a flag, not a privilege of one package.** A brain
   may set `fullAwareness: true`. That brain may play in a network game.
   The seat shows the flag. The choice of brain is not part of the
   simulation hash. The mod's `ai.*` hack values still reach every machine,
   because they are part of the mod profile that hash covers.

## 2. The package

A brain is one `.oabrain` file, or the folder that file unpacks to. The
archive is the zip every package kind uses. The names a zip may hold, the
shared size limits, and the address a registry uses are
[Packages](../registries/packages.md). They are not defined a second time.
OA installs a brain into `Brains/<id>`, using the id inside the manifest.

The manifest is the strict YAML a mod profile uses
([3.3 Strict YAML rules](../mods/oamod-standard.md#33-strict-yaml-rules)).
One document, one top-level mapping, no anchors and no block scalars.

### 2.1 Folder layout

The top of the package holds `oabrain.yaml`. Beside it, a file is one of
these. The comparison ignores the case of ASCII letters except where the
row says the bytes must match.

| Path | What it is |
| --- | --- |
| `oabrain.yaml` | The manifest. Required. At most 256 KiB. |
| The entry script | Lua source at the package's top. The default name is `brain.lua`. `entry` names another. The file has to be in the package. |
| `lua/` and a name ending `.lua` | Further Lua source. Each part is lower-case letters, digits and `_`. The dotted module name is at most 64 bytes. `require` reads only these files. |
| `nets/<stem>.oann` | A network the manifest names, byte for byte. The stem is 1 to 64 bytes of lower-case letters, digits and `_`. Any other path under `nets/` is outside. |
| `data/<stem>.csv`, `.tdf`, `.json` or `.txt` | One data table, one folder deep. The stem is 1 to 64 bytes of lower-case letters, digits and `_`. |
| `icon.png` | The picture, only when `icon` names it. Without that key the file is outside. |
| `README.md` | A note at the top. The game does not load it. |
| `LICENSE` | The licence at the top. `licence`, and a readme in a subfolder, are outside. |

A file outside that list is refused. `brain.lua` is outside when `entry`
names some other file.

```text
steady-commander/
  oabrain.yaml
  brain.lua
  icon.png
  lua/hold.lua
  data/openings.txt
  nets/steady.oann
  README.md
  LICENSE
```

### 2.2 Sizes that are the brain's own

The shared zip rules, the unpacked-size cap and the folder cap stay in
[Packages](../registries/packages.md). A brain adds these:

| What | Limit |
| --- | --- |
| `oabrain.yaml` | 256 KiB |
| One Lua file | 1 MiB (1,048,576 bytes) |
| Every network together | 64 MiB |
| One data table | 4 MiB |
| The icon | A square PNG, each side at most 256 pixels, at most 256 KiB |
| The package, when a seat reads it | 128 MiB. A larger file is refused with `the package is larger than 128 MiB` |

### 2.3 Pack and check

`oa-tool brain pack FOLDER` packs a folder whose top holds `oabrain.yaml`.
`oa-tool brain check FILE --game-dir DIR` loads a `.oabrain`, or a folder
whose top holds `oabrain.yaml`, against a game. It seats the brain in a
match of the game's definitions on a flat stand-in map and never ticks
that match. It reports whether the brain loaded, fitted and started.
`oa-tool brain net` reads an ONNX model of opset 11 to 21 and writes a
`.oann`. The shared `oa-tool pack` and `oa-tool check` lines, the default
file name `<id>-<version>.oabrain`, and the JSON keys a brain reports are
[Packages](../registries/packages.md).

A brain pack refuses a broken manifest, a file outside the layout, Lua
that is not source, a network whose bytes are not the digest the manifest
names, and an icon that is not a square PNG within the limits. A manifest
problem is `oabrain.yaml:<line>:<column>: <key>: <message>`. A file outside
the layout ends with `: outside the files a brain may hold`. A network
hash is the file, then `: its SHA-256 is `, the bytes' digest,
`, not the `, the manifest's digest, and ` oabrain.yaml gives`. Lua is
compiled and not run. A `.oann` file is hashed and not interpreted.
`--game-dir` on `brain pack` is a usage error. `brain check` takes
`--game-dir`.

## 3. The manifest

The reader collects every broken rule. The manifest is left unchanged when
any rule is broken. A document the strict reader refuses is one problem on
the key `document`. Indexes in a path start at 0.

The top-level keys, in the order the writer emits them, are `oabrain`,
`id`, `name`, `version`, `author`, `description`, `homepage`, `icon`,
`tags`, `works-with`, `requires`, `levels`, `fullAwareness`, `networks`,
`options`, `strings` and `entry`.

| Key | Type | Required | Limits | Meaning |
| --- | --- | --- | --- | --- |
| `oabrain` | integer | yes | `1` | The format. `1.0` and `"1"` are refused with `oabrain must be 1`. |
| `id` | string | yes | 1 to 64 bytes | Lower-case kebab-case. No leading, trailing or doubled hyphen. The folder name under `Brains/`. |
| `name` | string | yes | 1 to 64 bytes | The name players see. No control character. |
| `version` | string | yes | a mutator version | Quoted, one to three parts, such as `"2.0"`. See [15. Versioning](#15-versioning). |
| `author` | mapping | yes | the mutator author | `name` (1 to 128 bytes) and an optional `email`. |
| `description` | string | yes | 1 to 2,000 bytes | A line feed is allowed. Any other control character is refused. The first line, cut to 200 bytes on a character boundary, is the summary. |
| `homepage` | string | no | an `http` or `https` address | The shared address rule. |
| `icon` | string | no | `icon.png` | Anything else is `icon must be icon.png, the picture at the package's top`. |
| `tags` | list | yes | 1 to 4 of the nine tags | [3.1 Tags](#31-tags). |
| `works-with` | list | yes | the mutator rule, 1 to 32 | [3.7 works-with and requires](#37-works-with-and-requires). |
| `requires` | mapping | no | `engine` and `expansions` only | Any other key, `mutators` included, is `<key> is not a key of requires`. |
| `levels` | list | yes | 1 to 6 | [3.2 Levels](#32-levels). An empty list is `levels must list 1 to `, the limit 6, and ` levels`. A value that is not a list is `levels must be a list`. |
| `fullAwareness` | boolean | no | `true` or `false` | Absent means false. [3.3 fullAwareness](#33-fullawareness). |
| `networks` | list | no | 1 to 16 | [3.4 Networks](#34-networks). |
| `options` | mapping | no | at most 16 | [3.5 Options](#35-options). |
| `strings` | mapping | no | at most 16 languages | [3.6 Strings](#36-strings). |
| `entry` | string | no | `<stem>.lua` at the top | Absent means `brain.lua`. Anything else is `entry must be a Lua file at the package's top, named with lower-case letters, ` and `digits and _`. |

A profile key is refused with the key, then ` belongs in a mod's oamod.yaml; a brain cannot carry it`.
The profile keys are `identity`, `layout`, `limits`, `script-extensions`,
`data-keys`, `hacks`, `settings`, `media`, `packaging` and
`requiredExpansions`, and the games key a mod writes as one camel-case
word: support, then edGames. A mutator key is refused with
`<key> is a mutator's key; a brain cannot carry it`. Those keys are
`layers`, `clashes`, `conflicts-with` and `legacy`. Any other unknown key
is `<key> is not a key of oabrain.yaml`.

`homepage`, the shape of `tags`, and `requires.engine` follow the shared
package-key rules. `author`, `works-with`, `options` and `strings` follow
the mutator rules. A brain then limits tags to four of its own nine, and a
language block may also hold `levels`.

### 3.1 Tags

A tag is one of `classic`, `economy`, `rush`, `turtle`, `air`, `naval`,
`learned`, `teaching` and `experimental`, compared byte for byte. The
shared tag rule runs first (1 to 8, each lower-case kebab-case of 1 to 32
bytes, none repeated). When that rule already names a problem, the
four-tag limit and the membership check are not named again. More than
four, when the shared rule passed, is one problem:
`tags lists `, the count, ` tags; at most `, and 4. A tag that is not one
of the nine is `tags[`, the index, `] must be one of `, and the nine names.

### 3.2 Levels

`levels` is a list of 1 to 6 mappings. Each mapping is exactly `id`,
`name` and `difficulty`, all required. `id` is lower-case kebab-case of 1
to 32 bytes, and no id is repeated. `name` is 1 to 64 bytes with no
control character. `difficulty` is `easy`, `medium` or `hard`, byte for
byte. Those names are the values 0, 1 and 2, and a seat that picks the
level uses that value as its income rules, the same difficulty the game
already uses. A missing difficulty and a wrong one are the same problem:
the level's path, then `.difficulty must be easy, medium or hard`.

### 3.3 fullAwareness

`fullAwareness` is `true` or `false`. Absent means false. A string such as
`"true"` is `fullAwareness must be true or false`.

A brain with the flag sees units a seat in the fog would not, the way the
built-in computer player does, and its site checks do not need a placing
player. The flag is allowed in a network game. The Library shows
`Sees every unit on the map, as Total Annihilation's own computer player does`.
The flag is the manifest's own. It is not granted by the package id, and
it is not one of the things only Classic is given. What the flag adds to
the Lua API is [7.7 Full awareness](#77-full-awareness) and
[8.3 Sites](#83-sites).

### 3.4 Networks

`networks` is a list of 1 to 16 mappings of exactly `id`, `file` and
`sha256`. An empty list is `networks must list 1 to 16 networks`. A value
that is not a list is `networks must be a list`.

`id` is a Lua name of 1 to 32 bytes, `[a-z]` then `[a-z0-9_]*`, and not a
word Lua reserves (`is a word Lua reserves`). No id is repeated. `file` is
`nets/<stem>.oann`. The stem is 1 to 64 bytes of `a`-`z`, `0`-`9` and `_`,
and no file is repeated. `sha256` is a quoted string of 64 lower-case hex
digits. A number, an unquoted scalar, or any other text is the network's
path, then `.sha256 must be 64 lower-case hex digits in quotes`.

The digest is checked when the package is packed, when it is installed,
and again when a seat loads the brain, before the sandbox opens. A load
that does not match refuses the seat with the file's path, then
` does not match its sha256 in oabrain.yaml`.

### 3.5 Options

`options` is the mutator's options block
([3.4 Options](../mutators/oamut-standard.md#34-options)), at most 16.
Each id is a Lua name. A number option's `min`, `max`, `step` and
`default` are that same arithmetic. The seat copies at most 16 options.
`me.options` is the resolved values: a toggle is a boolean, a number is a
number, and a choice is its text.

### 3.6 Strings

`strings` is the mutator's strings block
([3.5 Strings](../mutators/oamut-standard.md#35-strings)), at most 16
language tags. Each language may hold `name`, `description`, `options`,
`labels` and `levels`. `levels` is a mapping from a level id this brain
defines to a name of 1 to 64 bytes with no control character. An unknown
id is `strings.de.levels.brutal is not a level of this brain`. A language
tag that is refused is not kept, and its `levels` are not read again.

### 3.7 works-with and requires

`works-with` and `requires.engine` use the mutator grammar
([3.1 Versions, ranges and conditions](../mutators/oamut-standard.md#31-versions-ranges-and-conditions)).
`*` is the only entry when it appears. `ta-3.1c` means the base game with
no mod. Any other id is a mod, optionally with a range, such as
`ridge >= 28`.

`requires` holds only `engine` and `expansions`. `engine` is the shared
engine range ([13. Versioning](../mutators/oamut-standard.md#13-versioning)).
`expansions` is a list of 1 to 16 lower-case kebab-case ids of 1 to 64
bytes, none twice. The installer checks each id against the built-in
bundles. A brain cannot require a mutator.

### 3.8 Example

`steady-commander` plays the base game and Ridge. The version is quoted.
`fullAwareness` is written so a reader sees the flag; leaving the key out
means the same false.

```yaml
oabrain: 1
id: "steady-commander"
name: "Steady Commander"
version: "1.0"
author:
  name: "Ridge Workshop"
description: "Holds the start and spends metal on a factory.\nThe second line stays in the description."
icon: "icon.png"
tags:
  - "economy"
  - "turtle"
works-with:
  - "ta-3.1c"
  - "ridge >= 28"
requires:
  engine: ">= 0.8.5"
  expansions:
    - "core-contingency"
levels:
  - id: "steady"
    name: "Steady"
    difficulty: medium
  - id: "pressed"
    name: "Pressed"
    difficulty: hard
fullAwareness: false
networks:
  - id: "steady"
    file: "nets/steady.oann"
    sha256: "0000000000000000000000000000000000000000000000000000000000000000"
options:
  announce:
    type: toggle
    label: "Announce each factory"
    default: false
strings:
  de:
    name: "Ruhiger Kommandant"
    levels:
      steady: "Ruhig"
      pressed: "Gedrängt"
entry: "brain.lua"
```

The bytes of `nets/steady.oann` have to hash to the `sha256` above. The
zeros stand in for the digest of that file.

## 4. Where a brain runs

Only the machine that seats the computer player needs the package, and
only that machine runs the script. A seat is a computer player of this
machine. A human seat is refused with `the player is not a computer`.

| Situation | What runs |
| --- | --- |
| Skirmish | Each computer seat this machine plays, when a player set a brain on it. An empty id is the built-in computer player. |
| Campaign mission | The campaign seat follows the default brain the player set. A mission does not ignore that default. |
| Network game | The machine that owns the computer seat runs the brain, full awareness included. The other machines do not. |
| Saved game loaded later | The save names each seat. [10. Saved games and recordings](#10-saved-games-and-recordings) says what happens when the package is missing or the version differs. |
| Recording played back | The recording stores the brains text. Playback applies the recorded packets. It does not seat a brain and does not call its hooks. |
| Deathmatch or tournament | The launch seats no computer players. |

The brain a seat plays is not an input of the simulation hash. Two matches
that differ only in which brain is seated keep the same match state hash.
The mod's `ai.*` hack values are part of the mod profile that hash covers,
so they still reach every machine. They are visible to the script as
`game.mod.hacks` and `game:hack`.

`game.mode` is `skirmish`, `campaign` or `network`.

### 4.1 The loadout entry

The seats are one JSON object. Its key is `seats`, a list sorted by `place`. Each
seat is an object. Members are written in byte order. An empty string is
left out. `place` is always written. `version`, `package_sha256` and
`name` are empty in setup memory and filled when a game is saved.

| Member | Meaning |
| --- | --- |
| `brain` | The brain id. Empty means the built-in computer player. |
| `level` | The level id. |
| `name` | The manifest name, filled when the game is saved. |
| `options` | An object of option id to text. Left out when empty. |
| `package_sha256` | The package digest, filled when the game is saved. |
| `place` | The player index, 0 to 9. |
| `version` | The manifest version, filled when the game is saved. |

The command line's `--computer-player` fifth field is a brain id. The
rules, including a brain that is not installed and one that does not fit,
are [the command line](../command-line.md).

## 5. The runtime

A brain runs as Lua 5.4.9 in one sandbox. Two brains share no modules, no
globals and no budgets. The profile is named `brain`.

The script has no file, no clock, no environment, no process and no
network. Every chunk is loaded as text. A failure comes back as a value
that names the package, the phase, the chunk and the line. `pcall` and
`xpcall` cannot keep a spent phase alive.

What this profile shares with a mutator's sandbox is the mutator
standard's sandbox, and it is not copied here:

- [10.1 Libraries](../mutators/oamut-standard.md#101-libraries): `_G`,
  `string`, `table`, `utf8`, `math` and `fixed`. `io`, `os`, `debug`,
  `package` and `coroutine` are not opened. `print` keeps at most 256
  lines and 16,384 bytes.
- [10.2 Loading code](../mutators/oamut-standard.md#102-loading-code)
- [10.3 require](../mutators/oamut-standard.md#103-require)
- [10.4 Budgets](../mutators/oamut-standard.md#104-budgets), for a phase
  that is not a banked turn
- [10.5 Charged work](../mutators/oamut-standard.md#105-charged-work)
- [10.6 Strings](../mutators/oamut-standard.md#106-strings)
- [10.7 Text, order and numbers](../mutators/oamut-standard.md#107-text-order-and-numbers)
- [10.8 The time limit](../mutators/oamut-standard.md#108-the-time-limit)
- [10.9 The fixed library](../mutators/oamut-standard.md#109-the-fixed-library)

The brain profile does not remove those libraries. It adds `f32` and
`geometry`, a per-tick bank, and the tables in the later sections.

### 5.1 Phases

| Phase | When | Allowance |
| --- | --- | --- |
| `load` | The entry script runs and must return a table of hooks. | The phase allowance: 50,000,000 instructions and 10 seconds. |
| `fits` | Before the first tick, when the table has `fits`. The argument is `game`. | The same phase allowance. |
| `start` | Before the first tick, when the table has `start`. The arguments are `game`, `me` and `saved`. | The same phase allowance. |
| Play | Each turn, one banked call. | 2,000,000 instructions a tick, banked up to 30 ticks, and 250 milliseconds. |
| `save` and `report` | When a save or the console asks. | The phase allowance. |

`fits` returns true, or a string reason. Anything else stops seating with
`fits must return true or a reason`. The entry must return a table. A
missing table is the entry path, then ` must return a table of hooks`. A
known hook name whose value is neither nil nor a function is the entry
path, `: `, the hook name, and ` must be a function`.

Name the hooks table something other than `brain`. A file that starts
`local brain = {}` shadows the global `brain` table.

### 5.2 Budgets

| Budget | Value |
| --- | --- |
| Instructions in `load`, `fits`, `start`, `save` and `report` | 50,000,000 |
| Instructions one tick adds | 2,000,000 |
| Ticks the bank may hold, the current tick included | 30, so the balance opens at 60,000,000 |
| Lua memory | 64 MiB (67,108,864 bytes). It is not reset between phases. |
| Network memory | 64 MiB, separate from the Lua cap. See [9. Neural networks](#9-neural-networks). |
| Real time, a phase that is not a turn | 10,000 milliseconds |
| Real time, one turn | 250 milliseconds |
| One string | 65,536 bytes |

A phase that spends its instruction allowance stops with the words
`the script ran past its budget of `, the allowance, and ` instructions`.
A turn that spends the bank stops with
`the script ran past its allowance of `, the balance,
` instructions (`, the per-tick allowance,
` a tick, banked up to `, the bank limit, and `)`.
Passing the Lua memory cap stops with
`the script used more than its `, the cap in mebibytes, and
` MiB of memory`. For this cap that number is 64.

A phase that runs past its real-time limit stops with the phase name, then
` ran for more than `, the limit, and
`; its work is too large or never ends`.
A turn appends
` on this computer; the stop is for the computer's speed, not the script's `
and `instructions`
instead of that last clause. The turn's limit is 250 milliseconds, so the
unit is `milliseconds`. A phase of 10,000 milliseconds is a whole number
of seconds, so the unit is `seconds`.

### 5.3 Numbers

`fixed` counts 65536ths, the engine's 16.16 scale. The rules are
[10.9 The fixed library](../mutators/oamut-standard.md#109-the-fixed-library).
Axes are x east, z south and y up. An angle is an integer, 65536 to a full
turn. A distance the map and the orders take is a `fixed` value or a whole
number of world units.

`f32` holds one binary32 value, the width of a player's resources. The
profile opens it. A mutator does not see it. `f32(v)` takes an `f32`, an
integer, a Lua float or a `fixed` value. `v:tonumber()` widens to a Lua
float. `v:tofixed()` multiplies by 65536 and truncates toward zero.
`v:bits()` is an integer from 0 to 4294967295, and `f32.from_bits` builds
a value from those bits. `v:isnan` reports a NaN. Two `f32` values, or an
`f32` and an integer, use float arithmetic and the result is an `f32`. An
`f32` and a Lua float use double arithmetic and the result is a Lua float.
An `f32` and a `fixed` value do not mix.

`geometry` is a table the profile opens. `geometry.direction(x, z)` is the
engine's heading, an integer from 0 to 65535. `geometry.planar_length(x, z)`
and `geometry.length(x, y, z)` are the engine's distances as `fixed`
values. `geometry.scale(x, y, z, factor)` returns three `fixed` values.
`geometry.sine_scaled(heading, magnitude)` and
`geometry.cosine_scaled(heading, magnitude)` are the engine's scaled sine
and cosine, as a `fixed` value. `heading` is an integer. `magnitude` is a
`fixed` value or an integer of whole world units. A bad heading is
`geometry.%s: argument 1 must be an integer`. A magnitude that is neither
fixed nor an integer is
`geometry.%s: argument 2 must be a fixed value or an integer`. A magnitude
outside the coordinate range is
`geometry.%s: argument 2 is outside -32768 to 32767.99998`.

A unit handle is one Lua object for one slot and one generation. The
generation is the count the match already keeps for that slot. The same
live unit is the same table key. A slot reused after a death is not the
old unit: `id` and `slot` still read, and `alive` is false. The metatable
is `oa.brain.unit`. `getmetatable` is false.

### 5.4 Random

`brain.random` is the `math.random` function taken when the profile
installs, so both draw from the brain's own xoshiro256** stream. A script
that replaces `math.random` does not change `brain.random`.
`brain.random(n)` returns an integer from 1 to n, as `math.random(n)` does.

The stream is seeded from the first eight bytes, little-endian, of the
SHA-256 of the match seed written as eight little-endian bytes (the 32-bit
seed zero-extended), then the player index as one byte, then the brain's
id, a zero byte, and the version. A load check has no match seed, so it
uses 0. Draws from this stream do not advance the match's shared stream.

`brain.shared_random` and `brain.setup_random` are Classic's, and a
shadow's. [13. Classic](#13-classic).

`brain.float_heap_order(distances)` returns the 1-based indexes of integer
distances, nearest first. Equal keys keep the later entry. It is installed
for every seat. A value that is not a table of integers is
`float_heap_order expects a table of integer distances`. The call can
also raise `float_heap_order: out of memory`. The call is charged by the
length of the list. It reads no match.

### 5.5 Timers

`brain.after(ticks, fn)` runs `fn` once, that many turns after the turn
that created it. `brain.every(ticks, fn)` runs then, and again every
`ticks` turns after the due tick it just met. `ticks` is an integer from 1
to 2147483647. Anything else raises `brain.after` or `brain.every`, then
`: `, then `ticks must be an integer from 1 to 2147483647`. Each call
returns an id. Ids start at 1 and only grow. `brain.cancel(id)` returns
true when it removed a waiting timer and false otherwise. A non-integer is
`brain.cancel: argument 1 must be an integer`. A brain may keep 1024
timers waiting; one more raises the function name, `: `, and
`a brain may keep at most 1024 timers waiting`.

Due timers run after chat and before `onTick`, in order of due tick, then
sequence number. One cancelled by an earlier timer of the same turn does
not run. Each run charges one element of work. An error inside a timer
stops the seat the way a hook error does.

### 5.6 print and the log

`print` follows
[10.1 Libraries](../mutators/oamut-standard.md#101-libraries). The app
also writes each kept line to the seat's log. The file is
`<data>/logs/brains/seat-<n>.log`, where n is the player index plus one
and `<data>` is the folder `--data-dir` names, or the account's data folder
without it. It is begun anew for each brain seated there. The first line is the
manifest name, a space, the version, the id in parentheses, `, level `,
the level, `, seat `, that same n, `, match seed `, and the seed. Each
printed line is the word tick, the tick, `: `, and the text. The file
stops at 1 MiB. The closing line is `the log is full`. A log that cannot
be opened is printed as
`open-annihilation: brains: seat `, the player index, and
`: the log could not be written`. That index is not the number in the
file name. The file and the header use the player index plus one.

## 6. Hooks

The entry returns one table. The engine calls the functions it finds
there. A missing function is skipped. One turn is one banked call, at the
seat's controller point, after the unit sweep. Orders a hook gives take
effect in the next tick.

The turn delivers, in this order:

1. Queued unit events, in the order they fired.
2. Radar and sonar changes, lost then gained, in slot order.
3. Alliance changes for the other players, in index order.
4. A defeat for each player who was participating and no longer is.
5. Chat lines, in arrival order.
6. Timers whose due tick has come.
7. `onTick`.

The first turn stores the alliances and who is participating and reports
none of those changes. The first radar picture is compared with an empty
list. Timers run even when the hooks table is missing. A missing `onTick`
is fine.

The queued hooks, in the order the dispatcher names them, are not a fixed
sequence among themselves. They run in the order the events fired.

| Hook | When | Arguments | Who it is about |
| --- | --- | --- | --- |
| `onUnitCreated` | A unit is created. | `unit`, `builder` | The seat's own unit. `builder` is nil when there is none. |
| `onUnitFinished` | A unit finishes. | `unit`, `builder` | The seat's own unit. `builder` is nil when there is none or it is the unit. A finished event is dropped when a cancelled death of the same unit follows it. |
| `onUnitDamaged` | A unit takes damage. | `unit`, `attacker`, `amount`, `kind` | The seat's own unit. `kind` is `weapon`, `paralyze`, `heal` or `environment`. `attacker` is passed only when the seat may know that unit, and is nil otherwise. |
| `onUnitDestroyed` | A unit dies. | `unit`, `killer`, `wreck` | The seat's own death, an ally's death, an enemy on the seen list, or any death when the brain has full awareness. A death of kind captured is not this hook. `wreck` is `{name, x, z}` or nil. |
| `onUnitGiven` | A unit is given. | `unit`, `old`, `from`, `to` | The old owner and the new owner. |
| `onUnitIdle` | A unit becomes idle. | `unit` | The seat's own unit. |
| `onCommandDone` | An order finishes. | `unit`, `order`, `result` | The seat's own unit. `result` is `done` or `failed`. |
| `onUnitLoaded` | A unit is loaded. | `unit`, `carrier` | The owner of the unit and the owner of the carrier. |
| `onUnitUnloaded` | A unit is unloaded. | `unit`, `carrier` | The owner of the unit and the owner of the carrier. |
| `onUnitSeen` | The seat gains sight of a unit. | `unit` | That player. |
| `onUnitLost` | The seat loses sight of a unit. | `unit` | That player. |
| `onPathFailed` | A unit cannot find a path. | `unit` | The unit's owner. |
| `onRadarLost` | A contact leaves the picture. | `contact` | After the queued events. Lost before gained for the same slot. |
| `onRadarContact` | A contact enters the picture. | `contact` | The same pass. An allied unit is never a contact. |
| `onAllianceChanged` | An alliance bit changes. | `player`, `allied` | Other players, in index order. |
| `onPlayerDefeated` | A player stops participating. | `player` | Each such player. |
| `onChat` | A chat line reached the seat. | `from`, `text`, `to` | `from` is the sender's index. `text` has no name head. `to` is `all`, `allies`, `enemies` or `me`. |
| `onTick` | The end of the turn. | `tick` | `Game.tick`. |

An event about the seat's own unit is queued for that seat. A death and a
wreck are queued for every seated brain, and an enemy death is then passed
on only when the seat may know it. A gift is queued for both owners. Sight
and a path failure are queued for that player. At most 65,536 events are
kept for one turn. Past that, the event is dropped. An event that fires
while the hooks run waits for the next turn.

`onChat` runs once per line since the last turn. A missing `onChat` is
skipped, and the line is still consumed. At most 32 lines wait; a further
line is dropped. Words longer than 256 bytes are cut between whole
characters.

### 6.1 save and report

`save()` takes no arguments. It returns the plain data a saved game keeps,
or nothing. The types, the 1 MiB cap and the refusals are
[10. Saved games and recordings](#10-saved-games-and-recordings). An error
inside `save` hands the seat back. A value that is not plain data is left
out of the save.

`report()` takes no arguments. It returns text, at most 64 KiB. The
console command that prints AI weights writes that text into the named
file. A brain with no `report` writes one line: the manifest name, a
space, the version, and ` gives no report.`. A report that fails writes
nothing. A built-in seat keeps the report it already prints.

The console command that reloads AI profiles stores the reloaded profile
in `game.ai_profile` and calls `start` again, with what `save` returns
now, or nil. Both calls use the phase allowance in the same sandbox. On
success the run prints
`open-annihilation: brains: player `, the player index, and
` starts again with the reloaded profile`.

`fits` and `start` are not part of the turn. They are in
[5.1 Phases](#51-phases).

## 7. What a brain may know

`game` is one read-only table for the life of the seat. A write raises
`game is read-only`. `fits` and `start` receive that same table.
`has_expansion` and `hack` live in the hidden table, so a write to either
name raises and `pairs(game)` yields both.

### 7.1 game

| Name | What it is |
| --- | --- |
| `game.base` | The content bundle. |
| `game.expansions` | The expansions that are on, in the built-in order. The base game is not an expansion. |
| `game:has_expansion(id)` | True when that expansion is on. |
| `game.engine` | The engine's version. |
| `game.mode` | `skirmish`, `campaign` or `network`. |
| `game.starts` | `fixed` or `random`, the start rule the match used. Nil outside a match, and nil until the match has placed its players. |
| `game.difficulty` | `easy`, `medium` or `hard`. |
| `game.in_match` | True for a seated brain. False for a load check. |
| `game.mod` | `id`, `name`, `version`, `sides`, `hacks` and `limits`. `sides` is every loaded SIDEDATA side in order from `SIDE0`, up to the mod's `limits.sides` (five without a mod), each `name`, `prefix` and `commander`. `limits` holds each limit by its registry id, with that limit's parameters. |
| `game:hack(id)` | That hack's table, or nil. |
| `game.mutators` | The loadout in apply order: `id`, `version` and `options`. |
| `game.map` | The map. Nil when the seat has no world. [7.2 The map](#72-the-map). |
| `game.ai_profile` | The profile the match's computer players read. [7.4 Profiles and strengths](#74-profiles-and-strengths). |
| `game.ai_profiles()` | Every profile in the facts, in listed order. |
| `game.parse_profile(text)` | Parses any text. The charge is the text's bytes plus one unit per word. |
| `game.build_list(type)` | A new sequence of `units` records, the builder's list. |
| `game.build_list_cap` | How many ids that list keeps. |
| `game.downloadables_allowed` | False when the computer players were configured for a campaign. Nil outside a match. |
| `game.units` | Full awareness only. [7.7 Full awareness](#77-full-awareness). |

`game.mod.sides` lists every loaded SIDEDATA side in order from `SIDE0`,
up to the mod's `limits.sides` (five without a mod). Lua indexes them from
1. Each side is `name`, `prefix` and `commander`. Each simulation hack is
`on` plus its parameters, listed whether the hack is on or off. A view
hack is left out. `game.mod.limits` is keyed by registry id. Each limit is
its parameters, with no `on`:
`game.mod.limits["limits.weapon-types"].types` and
`game.mod.limits["limits.sides"].count`.

`game.starts` is the rule the match placed with. A campaign mission is
`fixed`. So is a match whose players were placed by the map's own units.
Otherwise it is the skirmish StartLocation, or the battle room's starts
setting. The word is charged when `game` is built, like the other text
fields. A seat opened before the players are placed reads it once they are.

### 7.2 The map

`game.map` is one read-only object. Assigning a field raises
`map is read-only`. An unknown name reads nil.

| Name | What it returns |
| --- | --- |
| `name` | The map's name. |
| `path` | The mission path, or nil when the facts have none. |
| `world` | Always nil. |
| `width` | The map in cells. |
| `world_width`, `world_height` | The same edges in world units. |
| `sea_level` | An integer. |
| `tide` | An `f32`. |
| `wind_min`, `wind_max` | The wind's strength range. |
| `surface_metal` | The metal of the first cell that is not a metal deposit. |
| `map:wind()` | The current speed and direction, two integers. |
| `map:height()` | The height in cells. `map:height(cx, cz)` is that cell's corner height, or nil off the map. |
| `map:heights(cx, cz)` | The cell's low and high height, or nil off the map. |
| `map:ground(x, z)` | The sampled ground height. Off the map the engine reports -1. |
| `map:metal(cx, cz)` | That cell's metal, or 0 off the map. |
| `map:metal_feature(cx, cz)` | Whether the cell is a metal deposit. False off the map. |
| `map:metal_spots()` | Deposits in scan order, rows then columns. Each entry is `cx` and `cz`. One charge per cell. |
| `map:geo_spots()` | The same for geothermal spots. Nil outside a match. |
| `map:starts()` | Every start position of the schema in play, in StartPos order. Each entry is `index` (the StartPos number, from 1), `x` and `z` (world units) and `cx`, `cz` (the cell). An empty list when the map defines none. Nil outside a match. The charge is one unit per start, plus one. |
| `map:classes()` | Movement classes the match's unit types use, in handle order. Each entry is `name`, `handle`, `footprint_x` and `footprint_z`. One charge per type. |
| `map:passable(class, cx, cz)` | 0 blocked, 1 slow or not clear, 3 clear, and 0 off the map. It does not look at what the seat has seen. |
| `map:anchor(class, position)` | The cell a unit of that class would stand on. |
| `map:reachable(a, b, class)` | True when the two positions can meet through that class's open cells, including a diagonal step. |
| `map:route(a, b, class [, options])` | The route a unit of that class would take. Nil when the two cannot meet, when a position is one the seat may not read, when the search passes its cap, or outside a match. Otherwise `length`, `cells` and, when `options.points` is true, `points`. |

A class is a class name, ignoring ASCII case, a unit type's record, a unit
type's name, or an integer. A unit handle is not a class. A type with no
movement class can reach any two positions. `passable` and `anchor` refuse
that type with `unknown movement class: `. Four classes are cached in a
tick. Building the regions costs one charge per sixteen cells, rounded up.
Each question costs 16 more. A position the seat may not read makes
`reachable` return nil and still costs 16.

`map:route(a, b, class [, options])` estimates that route. `a` and `b`
are positions or unit handles, as `reachable` takes them. `class` is the
same argument `passable` takes. The route uses the class's movement map
as the game knows it, the same open cells `passable` reads, not what the
seat has seen. A diagonal step is allowed, as `reachable` allows it. A
clear cell costs 16 world units for a step to the side and 22 for a
diagonal step. A slow cell (`passable` 1) adds 30. The costs are
integers, and when two steps cost the same the earlier one in that fixed
neighbour order is kept, so every machine agrees. `length` is that cost.
On clear ground a straight side-to-side route's length is the straight
distance. `cells` counts the cells the route crosses, including both
ends. `points`, when `options.points` is true, starts at `a` and ends at
`b`. The entries between are the turns, each an `x` and a `z`. A whole
world unit is an integer. A fraction is a fixed value.

A position the seat may not read returns nil and still costs 16, and the
class is not checked. Two ends that cannot meet return nil, the same
answer `reachable` gives. A brain that only needs to know whether two
places meet should ask `map:reachable` first, which is far cheaper, and
only then ask `route` for the length. A type with no movement class has
no map to search: `length` is the straight distance in whole world units
and `cells` is 0. One search charges one instruction per cell it expands,
plus 16. The search stops after 1,048,576 expanded cells, the cells of a
1024 by 1024 map, and then returns nil. That call still costs its charge.
A search that reaches the cap costs 1,048,592, inside one tick's
allowance. When `options.points` is true the search runs again and that
second search is charged the same way. The call does not change the
movement map, its projection, its occupancy, or any unit, and it does not
wait on the path-search worker. A call that fails its arguments costs
nothing. Outside a match, `route` returns nil.

### 7.3 Definitions

`units`, `weapons` and `features` are the match's loaded records, after
the mod and the mutators. They are read-only. Assigning a field raises
`a collection cannot be assigned` or `a record cannot be assigned`.
Replacing the global `units` itself is ordinary Lua.

`units[i]` is type index `i`, from 1. Index 0 is not a record.
`weapons[id]` is the weapon id from 0 to the mod's `weapon-types` minus one
(255 without a mod), or nil when that slot was not loaded. `features[i]` is the feature type at index `i - 1`. Names are
matched without regard to ASCII case. `pairs` and `each` walk type index,
then weapon id, then feature index.

A record answers `rec.key` and `rec:get(key)`. `rec:fixed(key)` is that
value as a fixed count. `rec:keys()` is the kept fields in schema order.
`rec:index()` is the type index, the weapon id, or the feature's index
from 0. `weapon:damage()` is the weapon's default damage.
`weapon:damage(unit)` takes a unit record, a unit name or a type index. A
call that is not a weapon is `damage expects a weapon`. A bad argument is
`damage expects a unit type or nothing`. An unknown type is
`%s: unknown unit type %s` or
`%s expects a unit type: a record, a name or an index`.

An integer field is a Lua integer. A 16.16 count is a `fixed` value. A
stored float is an `f32`. A tick field is the stored tick count, thirty to
a second. These unit keys are not on the record, so they read nil:
`copyright`, `version`, `yardmap`, `corpse`, `badslope`, `badwaterslope`,
`wpri_badtargetcategory`, `wsec_badtargetcategory`,
`wspe_badtargetcategory` and `nochasecategory`.

A unit handle's `type` is that unit's record. It is a seen field.

### 7.4 Profiles and strengths

`game.ai_profile` has `path`, `name`, `text`, `too_large`, `lines` and
`report_path`. `text` is nil and `too_large` is true when the text passes
64 KiB. An empty path makes `game.ai_profile` nil. Assigning a field raises
`a profile is read-only`.

`lines` is one table per line. A line has `number`, `words`, `command`
and, by command:

- `plan`: `any`, `easy`, `medium`, `hard`.
- `weight`: `name`, `factor` (an `f32`) and `unit`.
- `limit`: `name`, `count` and `unit`.

`unit` is true when `name` equals some type's unit name, ignoring ASCII
case. Outside a match `unit` is nil.

`profile:select(keyword)` takes `"easy"`, `"medium"` or `"hard"`. Any
other keyword raises `select expects easy, medium or hard`. The result is
`weights` and `limits`, the line tables that apply.

`brain.strengths(type)` returns the seat's three strategic bytes, each
read as a signed 8-bit value: build priority, metal, energy. The charge
is 2. It returns nil when the seat's table does not reach the type.

### 7.5 The seat

`me` is read-only. Assigning a field raises `me is read-only`.

| Name | What it is |
| --- | --- |
| `me.player` | The player index, 0 to 9. |
| `me.level` | The level id. |
| `me.difficulty` | `easy`, `medium` or `hard`. |
| `me.options` | The resolved options. |
| `me.name` | The player's name. |
| `me.side`, `me.side_name` | The side index, and the name of any side the mod loaded. A side past that table has no name. |
| `me.team` | The team. |
| `me.color` | The colour. |
| `me.energy`, `me.metal` | What the stores hold now, in energy and in metal, as `f32`. |
| `me.energy_storage`, `me.metal_storage` | How much the stores can hold, in energy and in metal, as `f32`. |
| `me.energy_produced`, `me.metal_produced` | Income, per second, as `f32`. See [7.5.1 The economy](#751-the-economy). |
| `me.energy_requested`, `me.metal_requested` | Demand, per second, as `f32`: what the seat's units asked for, paid or not. |
| `me.energy_spent`, `me.metal_spent` | What was actually paid out, per second, as `f32`. Never more than the store and the income could pay. Nil with no match. |
| `me.energy_surplus`, `me.metal_surplus` | Produced less requested, per second, as Lua numbers. |
| `me.tick` | The match tick. |
| `me.handed_over` | False when the seat is made before the built-in player's set-up has run. |
| `me.refused_assignments` | Assignments the seat refused. |
| `me.knowledge` | `tick`, `builders`, `base`, and `knowledge:owned(type)`. Nil when the record is not there. |
| `me.builds_held_until` | The tick a capturing builder waits for. Nil when the seat was not given that call. 0 is a real tick. |
| `me:allied(player)` | Whether that player is an ally. |
| `me:allies()` | The allied players. |
| `me:players()` | The players in the game. `#me:players()` is how many. Each row has `player`, `name`, `side`, `team`, `allied`, `computer`, `alive`, `start`, `start_x`, `start_z` and `allies`. |
| `me:units()`, `me:each_unit(fn)` | The seat's occupied slots, in slot order. `each_unit` takes the list before the first call. |
| `me:unit_count()` | How many of those units. |
| `me:squad(n)`, `me:squad_size(n)`, `me:squad_member(n, i)` | Squads in the engine's member order. Squads 1 to 9 are the lists the engine holds. |
| `me.full_awareness` | The flag the seat was given. |
| `me.seen`, `me.seen_tick` | The seen list, as unit handles, and the refresh tick. |
| `me:sees(x, z [, y])` | Whether the point is visible. `y` is 0 when it is left out. |
| `me:sees_unit(u)` | Whether a live handle is visible. False for a dead one. |
| `me.sight`, `me.explored` | Grids of 32-unit cells: `width`, `height`, `cell_size`, `at(cx, cz)`, `cell_of(x, z)`. Nil when the match keeps no sight grid. |
| `me.radar`, `me.radar_tick` | Live enemy contacts in the seat's own picture, in slot order, and the tick of that picture. |

`start` is that player's StartPos number, from 1. `start_x` and `start_z`
are that position in world units, the same units `brain.distance` and the
order calls take. When `game.starts` is `fixed`, every player who was
given a start has these three. When it is `random`, only this seat's row
has them. A watcher, or a player with no start, has nil for all three.
In a network game every machine gives a player the start the host dealt
it, though each machine numbers the players by its own slots.
`allies` is the other players that row counts as allies, in index order,
from the alliance table as the match holds it now. It changes when an
alliance changes. The player itself is not in the list. Outside a match
the start fields are nil and each `allies` list is empty. The charge is
one unit per player, plus one, and one more unit per ally in each list,
plus one per list.

A contact gives `x`, `y`, `z` and `position` while it is still a contact
of the latest picture with the same generation. `sonar` and `radar` stay
as they were when the contact was published. `current` is whether it is
still a contact. A contact has no type, no owner and no unit handle.

`brain.distance` and `brain.direction` measure between two units or two
positions with the engine's own maths. A unit whose position the seat may
not read contributes nil.

#### 7.5.1 The economy

Energy and metal are counted in the game's own units, the numbers the
resource bar shows. Each player's economy settles once a second of game
time, every 30 ticks. `produced`, `requested`, `spent` and `surplus` are
the figures of the seat's last settlement: amounts per second. They hold
until the next settlement, and a turn reads the last one before it. They
are 0 until the seat's first settlement. `me.energy` and `me.metal` are
the stores as they are now, which change between settlements.

`produced` is the second's income: each finished unit's EnergyMake and
MetalMake, a negative EnergyUse (a solar collector's -20 makes 20 energy a
second), wind and tidal output, metal extracted or made, what was
reclaimed, and what allies gave. A computer seat's income is scaled by
its level's difficulty: easy makes half, medium 0.7 of it, hard all of it,
unless the mod sets its own income rules. `produced` is after that
scaling.

`requested` is the second's demand: what the seat's units asked for,
whether it was paid or not. That is construction (a build's cost spread
over its build time at the builders' rate), a positive EnergyUse while a
unit is active or moving (upkeep, such as a commander's 1 energy a
second, or a metal maker's), cloak upkeep, the energy and metal a weapon
takes per shot, and what the seat gave its allies. It is demand, not
spending. The resource bar shows `produced` and `requested` as its two
rates.

`surplus` is `produced` less `requested`, in a Lua number at double
precision, the same difference the built-in computer player compares with
0. It is negative whenever the units ask for more than comes in, which is
nearly every second something is built: building is paid for from the
store, and income alone rarely covers it.

`spent` is what was actually paid out. A shot, a cloak's upkeep and a
gift are paid whole from the store, or not at all. The rest is paid at
the settlement from the store and the second's income together. When
they cannot pay a unit's share in full, the unit owes what is left: until
a later settlement pays it, the unit's construction does not advance and
a metal maker or extractor that owes energy stops, while what the unit
asks for meanwhile still counts as requested. Owing either resource holds
construction back, so a builder that owes metal leaves its energy unspent
too. A settlement pays what is owed first. So `spent` is never more than
the store held plus the second's income. With enough in store, `spent`
equals `requested`. With the store empty, `spent` is about the income and
falls short of `requested`. That difference is the real shortfall; a
negative `surplus` alone is not.

A worked example, from the base game's data. A Core commander makes 25
energy and 1 metal a second, uses 1 energy a second, and works through
300 of a build time a second (its WorkerTime), 10 a tick. A Core solar
collector costs 790 energy and 141 metal with a build time of 2416, so
the commander takes 2416 / 300, about 8 seconds, to build it. While it
builds, every second:

| Figure | Energy | Metal |
| --- | --- | --- |
| `produced`, hard | 25 | 1 |
| `requested` | 790 × 300 / 2416 + 1 = 99.10 | 141 × 300 / 2416 = 17.51 |
| `surplus`, hard | -74.10 | -16.51 |
| `spent`, 1000 in store | 99.10 | 17.51 |
| `produced` and `surplus`, easy | 12.5 and -86.60 | 0.5 and -17.01 |

With 1000 energy in store, the store falls by about 74 a second, nearly
600 over the build, and the collector is finished at full speed. With an
empty energy store, `spent` is about the 25 that comes in each second
while `requested` stays at 99.10: the commander builds one second in four
and the collector takes about four times as long. With both stores empty,
once the energy owed is paid the commander still owes metal, so energy
`spent` drops to the upkeep of 1 while energy `requested` stays at 99.10
and the energy store fills. An Arm commander has no upkeep, and an Arm solar collector costs 760
energy and 145 metal with a build time of 2495: 91.38 energy and 17.43
metal requested a second.

`me.energy_spent` and `me.metal_spent` came with the engine's 0.8.7. On
an earlier engine they are nil. A brain that needs them asks for
`">= 0.8.7"` in `requires.engine`
([3.7 works-with and requires](#37-works-with-and-requires)).

### 7.6 What a handle reads

`id` and `slot` read on any handle. A seen field reads the seat's own live
unit, and another player's live unit while the seat can see it now or the
brain has full awareness. An own field reads only the seat's own live unit.

| Access | Fields |
| --- | --- |
| Always | `id`, `slot` |
| Seen | `alive`, `owner`, `type_index`, `type_name`, `type`, `x`, `y`, `z`, `position`, `heading`, `health`, `health_percent`, `max_health`, `build_remaining`, `finished`, `building` |
| Own | `armed`, `selectable`, `live`, `dying`, `active`, `cloaked`, `mobile`, `squad`, `move_order`, `fire_order`, `kills`, `head_order`, `has_background_order`, `orders`, `reaches` |

`health` and `health_percent` stay nil when the type hides damage, unless
the seat owns the unit or the brain has full awareness. `position` is
`x`, `y` and `z` as `fixed` values. `orders` lists at most 256 queue
records. Each record is `kind`, `target`, `x`, `y`, `z`, `parameter_1`
and `parameter_2`. The charge is one unit per record, plus one.
`reaches(position [, weapon])` asks whether that weapon reaches the
position. `weapon` is 1, 2 or 3, and 1 is the default. Anything else is
`reaches expects a weapon from 1 to 3`. The charge is 16.

### 7.7 Full awareness

Without the flag, `game.units`, `me:nearest_enemy` and `me:seen_raw` are
nil. With the flag:

- `game.units:list([player])` is every occupied slot, in slot order,
  optionally one player's.
- `game.units:each(fn)` walks that same order from a list taken before the
  first call.
- `me:nearest_enemy(x, z [, filter])` is the built-in player's search.
  `filter.widened` and `filter.skip_submerged` are off when the key is
  missing.
- `me:seen_raw()` reads each seen slot's current record: `slot`,
  `type_index`, `x`, `y` and `z`.

`me:sees` and `me:sees_unit` do not treat the flag as a yes. They answer
what the engine answers. The flag does not grant `brain.shared_random` or
a direct stance.

## 8. What a brain may do

A unit handle takes the orders a player can give. Each call reaches the
match at once. The order runs in the next tick's unit sweep. A refusal
returns false and is counted. It is not raised.

The first failing check is the one that counts: `dead`, `not_own`,
`over_limit` (the turn has already passed 4,096 orders to the match),
`not_seen`, or `not_buildable`. A match refusal counts as `by_engine` and
does not reach Lua. `on` with nothing to change, a direct stance and a
site check are not orders toward that limit.

`brain.order_counts()` is this turn. `brain.order_counts("match")` is the
match. Anything else is `order_counts expects ` and then the word match
in quotes. With no seat the call raises `order_counts has no seat`. The
fields are
`given`, `not_own`, `dead`, `not_seen`, `not_buildable`, `over_limit` and
`by_engine`. `start` is one turn. A live turn starts the turn's counts
again.

Where a method takes a queue flag, a missing flag is false.

| Call | What it does |
| --- | --- |
| `unit:move(position [, queued])` | A ground move. |
| `unit:patrol(position [, queued])` | A patrol. |
| `unit:attack(target [, queued])` | An attack on a unit. |
| `unit:attack_ground(position [, queued])` | An attack on open ground. |
| `unit:attack_point(position)` | An attack on a point, unqueued. Returns false when no mission kind resolves. |
| `unit:dgun(position or unit [, queued])` | The special attack. A unit uses that unit's slot and its current position. A bad argument is `dgun expects a position or a unit`. |
| `unit:guard(target [, queued])` | Guard. |
| `unit:repair(target [, queued])` | Repair. |
| `unit:help(target [, queued])` | Help a build. |
| `unit:reclaim(unit or position [, queued])` | Reclaim a unit, or a feature at a position. A bad argument is `reclaim expects a unit or a position`. |
| `unit:capture(target [, queued])` | Capture. |
| `unit:load(target [, queued])` | Load a unit. |
| `unit:unload(position [, queued])` | Unload at a position. |
| `unit:build(type, position [, facing [, queued]])` | A mobile build. `facing` is 0 to 3. A bad facing is `%s expects a facing from 0 to 3`, with the method's name. |
| `unit:queue(type [, count])` | A factory build. `count` is 1 to 1000, and the default is 1. A bad count is `queue expects a count from 1 to 1000`. |
| `unit:requeue(type, delta)` | Changes the queued count. `delta` is a non-zero integer from -1000 to 1000. A bad delta is `requeue expects a non-zero delta from -1000 to 1000`. |
| `unit:stop()` | Stop. |
| `unit:on(flag)` | Sets the active bit. Returns true when nothing needed changing. |
| `unit:cloak(flag)` | Cloak. |
| `unit:self_destruct()` | Toggles self-destruct for that one unit. |
| `unit:stance(table)` | `move` is `hold`, `manoeuvre` or `roam`. `fire` is `hold`, `return` or `at_will`. A bad move is `stance expects ` and then those three words in quotes. A bad fire is `stance expects ` and then `hold`, `return` and `at_will` in quotes. A value that is not a table is `stance expects a table`. |

`direct = true` on that table writes the standing bits and gives no
order. The only test for that write is whether the seat is Classic.
[13. Classic](#13-classic). Any other seat raises
`stance: direct writes are for the Classic brain only`. `direct` that is
not a boolean is `stance expects true or false`.

### 8.1 Assign

`me:assign(unit, squad)` calls the squad change at once. A unit the seat
does not own, or a dead handle, returns false and counts one refusal.
Squad 0 can still list a unit whose own squad field has moved on.

### 8.2 Chat

`brain.say(text [, to])` hands one line to the match at once. `to` is
`all` (the default), `allies`, `enemies` or a player index from 0 to 9.
Text that is not a string is `say expects text`. A bad audience is
`say expects ` and then `all`, `allies`, `enemies` or a player from 0 to
9, the words in quotes. The call
returns true when the line was queued, and otherwise false plus
`not_a_seat`, `empty`, `command`, `too_soon`, `no_target` or `no_chat`.
It does not pace itself. The match does, at 30 ticks between lines a
computer seat says. With no say hook, or outside a match, the answer is
`no_chat`. The charge is 16.

### 8.3 Sites

`brain.site(type, cx, cz [, facing [, check]])` charges 16. `facing` is 0
to 3. A bad cell is `site expects a cell`. A bad facing is
`%s expects a facing from 0 to 3`. `check` is `"site"` (the default) or
`"yard"`. Anything else is `site expects ` and then the words site and
yard in quotes. With no seat the call raises `site has no seat`. A seat
with no match returns nil, unless the seat has full awareness and the
host provides the check.

On a seat with full awareness, `"site"` is the clear-for-site test and
`"yard"` is the building-yard test. On any other seat the type must be a
building (`site tests buildings; %s is not one`). `"site"` is the map-edge
test and then
the building site with the seat as the placing player, and `"yard"` is the
building site alone. Full awareness is the only choice. Being Classic does
not decide it.

`brain.place(type, near, radius [, facing])` starts at the footprint cell
whose centre is `near`, then walks rings at Chebyshev distance up to
`radius` world units divided by 16, each ring from its north-west corner
clockwise. It returns the first cell where `"site"` holds, and that cell's
centre, or nil after the ring limit or 4,096 cells. Each cell tried
charges 16. A bad radius is `place expects a radius in world units`.
With no seat the call raises `place has no seat`. Outside a match,
`place` returns nil.

### 8.4 Reachability

`map:reachable` and `map:route` are [7.2 The map](#72-the-map).
`reachable` answers whether the two positions can meet. It does not
search for a path and does not change the movement map. `route` estimates
the path a unit of that class would take, on the calling thread, and does
not change the movement map or any unit. It charges one instruction per
expanded cell, plus 16, and stops after 1,048,576 expanded cells. Asking
for the turning points searches and charges a second time. A brain that
only needs to know whether two places meet should ask `reachable` first.

## 9. Neural networks

A network file is a format-1 `.oann`. The byte layout is
[the .oann format](../../src/formats/oann/README.md). This section states
what a script sees.

The format version is 1. The file is at most 64 MiB. A tensor type is
`int8`, `int16` or `int32`. A stored exponent is -48 to 16. A tensor name
is 1 to 31 bytes of `a`-`z`, `0`-`9` and `_`, and starts with a letter.
One file holds 2 to 4,096 tensors and 1 to 1,024 layers. A dimension is 1
to 65,535, and a tensor rank is 1 to 4. A file tensor holds at most
16,777,216 elements. The dense, conv2d and embedding weights together hold
at most 8,388,608 elements.

The layer kinds, in order, are `dense`, `conv2d`, `maxpool`, `relu`,
`leaky_relu`, `tanh`, `sigmoid`, `add`, `concat`, `flatten`, `softmax`,
`argmax` and `embedding`. Inputs and activations are int16. Outputs are
int16, except an `argmax` output, which is int32.

### 9.1 Checks

The digest is checked at pack, at install and at load. See
[3.4 Networks](#34-networks). After the digest matches, and before the
sandbox opens, the load reads the file and builds the network. A network
that would take the seat past 64 MiB (67,108,864 bytes) is refused before
it is allocated, and so is a file the format refuses or a network the
build refuses. The reason is `network %.*s: %.*s`, the id and then the
builder's detail. One size refusal inside that detail is
`networks would take %llu bytes; a brain may hold %llu`. The seat is removed
and the built-in computer player keeps the seat. The load is outside every
phase and is charged to nothing. One run may perform at most 268,435,456
operations.

The Library labels a brain that carries weights with `Network` and the
line `Uses a learned network ({size})`.

### 9.2 Calling a network

`brain.network(id)` returns the handle, the same userdata every time for
one id. A missing id raises `brain.network: this brain has no network 'x'`
and costs nothing. A found id costs 1.

`brain.tensor(values [, shape])` builds an immutable tensor. `values` is a
sequence of 1 to 4,194,304 numbers. An integer `i` is stored as
`i × 65536`. A fractional number is multiplied by 65536 and truncated
toward zero. A fixed value is stored as its count. `shape` is 1 to 4
positive integers whose product is the number of values, or the length of
`values` when it is omitted. The metatable is `oa.brain.tensor`.
`tostring(t)` is the ranks joined by `x`, such as `tensor: 2x8x8`.
Assigning a field raises `a tensor cannot be changed` and costs nothing.

| Refusal | Message |
| --- | --- |
| Too many or too few numbers | `brain.tensor: expects 1 to 4194304 numbers` |
| A value that is not a number | `brain.tensor: value N is not a number` |
| A non-finite number | `brain.tensor: value N is not a finite number` |
| Outside the fixed range | `brain.tensor: value %d is outside -32768 to 32767` |
| A bad shape | `brain.tensor: the shape must be 1 to 4 positive integers` |
| A shape whose product differs | `brain.tensor: the shape covers N values, not M` |

`t:get(i)` is the 1-based element as a fixed value. An index outside the
element count raises `brain.tensor: index 0 is outside 1 to N` and costs
nothing. `t:argmax()` is the 1-based index of the largest element, the
first when several match. `t:totable()` is a sequence of fixed values.
`t:size()` and `#t` are the element count. `t:shape()` is the shape.

`net:run(inputs)` takes a table with exactly the network's input names.
The result is a new table. An int16 output is a tensor. An argmax output
is a Lua integer, the 0-based index plus one.

| Refusal | Message |
| --- | --- |
| The value is not a network | `run expects a network` |
| The argument is not a table | `network <id>: run expects a table of inputs` |
| An unknown name, first in raw byte order | `network <id>: unknown input '<name>'` |
| A key that is not a string | `network <id>: input names must be strings` |
| A missing input, first in file order | `network <id>: missing input '<name>'` |
| A value that is not a tensor | `network <id>: input '<name>' must be a tensor` |
| The wrong element count | `network <id>: input '<name>' has <got> elements, not <want>` |
| An embedding row out of range | `network %s: layer %I (embedding) has no row %I` |

`net:inputs()` and `net:outputs()` are the port names in file order.
`net:cost()` is the units one run is charged. The call itself costs one.

### 9.3 What a call costs

An argument error taken before the charge costs nothing. A charge the
allowance cannot hold stops the phase before any result exists.

| Call | Charge |
| --- | --- |
| `brain.tensor` | One unit per value and one per shape entry. |
| `t:totable()` | One unit per element. |
| `t:get` | One unit. |
| `t:argmax` | One unit per 64 elements, rounded up. |
| `t:size` and `#t` | One unit. |
| `t:shape` | One unit per shape entry. |
| `net:run` | `net:cost()`, plus one unit per 64 input elements, plus one unit per 64 output elements, plus 4,096 for the one walk of the argument keys. |
| `brain.network`, `tostring`, `net:cost` | One unit. |
| `net:inputs`, `net:outputs` | One unit per name, and one unit when the network has no ports of that kind. |

### 9.4 Grids

`brain.grid(layers [, size])` returns a tensor of shape
`{#layers, size, size}`. `size` is 8, 16, 32, 64 or 128. 64 is the
default. Anything else is
`brain.grid: size must be 8, 16, 32, 64 or 128`. `layers` is a sequence of
1 to 10 distinct names, in the order the tensor stores them. An empty
sequence or one longer than 10 is
`brain.grid: expects 1 to 10 layer names`. An unknown name is
`brain.grid: unknown layer '%s'`. A repeated name is
`brain.grid: layer '%s' is repeated`. A match that cannot be read is
`brain.grid: the match could not be read`. Those refusals cost nothing.
Outside a match the call returns nil and costs nothing.

Rows run from north to south and columns from west to east. The ten names
are `sight`, `explored`, `water`, `height`, `metal`, `own`, `allies`,
`enemies`, `threat` and `radar`.

| Layer | Value |
| --- | --- |
| `sight` | The share of its sight cells the seat sees now, from 0 to 1. |
| `explored` | The share the seat has ever seen. |
| `water` | The share of its map plots whose height is below sea level. |
| `height` | The mean plot height, truncated to 1/65536. |
| `metal` | The mean plot metal, the same way. |
| `own` | The seat's live units in the cell, nanoframes included, stored as `n × 65536`. |
| `allies` | Allied players' live units the seat may see, excluding the seat's own. |
| `enemies` | Other players' live units the seat may see. |
| `threat` | Those enemies whose type has weapons. |
| `radar` | Radar and sonar contacts that are not also in sight. |

Full awareness shows units the seat could not otherwise see. It does not
invent radar contacts.

The charge is one unit per 16 cells of `layers × size × size`, rounded
up, plus one unit per unit slot when any of `own`, `allies`, `enemies`,
`threat` or `radar` is asked, plus one unit per 64 map plots when
`height` or `metal` is asked, plus one unit per 64 sight cells when
`sight`, `explored` or `water` is asked.

### 9.5 Records

`brain.record` is installed for every brain. It writes a training sample
when the match is recording, and it checks and charges the same when no
file is open. The command that opens the file is
[the command line](../command-line.md).

```lua
brain.record{
  features = { sectors = brain.grid({"own", "explored"}, 8) },
  decision = 0
}
```

`features` is 1 to 16 tensors. A name is 1 to 31 letters, digits or
underscores. `decision` is an integer or a tensor. One call holds at most
4,194,304 elements. The call costs 64 instructions plus one more for every
64 elements, and it returns nothing.

| Refusal | Message |
| --- | --- |
| Not one table | `brain.record: expects one table` |
| The wrong feature table | `brain.record: features must be a table of 1 to 16 tensors` |
| A bad name | `brain.record: a feature name is 1 to 31 letters, digits or underscores` |
| A repeated name | `brain.record: feature '%s' is repeated` |
| Not a tensor | `brain.record: feature '%s' is not a tensor` |
| A bad decision | `brain.record: decision must be an integer or a tensor` |
| Too many elements | `brain.record: at most 4194304 elements` |

The file stops at 1 GiB. The last line is `{"kind":"full"}`.

## 10. Saved games and recordings

`save()` returns plain data: nil, booleans, integers, floats, strings,
`fixed` counts and tables of those. Nil is only the top value, the one a
save that returns nothing produces. A function, a coroutine, userdata that
is not a `fixed` value, a table with a metatable, a table that contains
itself, and a key that is not an integer or a string are refused. The
refusal names the place, as in `saved.units[3]: a function cannot be saved`.
The other refusals are `a number cannot be saved`,
`a string cannot be saved`, `userdata cannot be saved`,
`a function cannot be saved`, `a coroutine cannot be saved`,
`a value of this kind cannot be saved`,
`a table with a metatable cannot be saved`,
`a table that contains itself cannot be saved`,
`a float key cannot be saved`, `a string key cannot be saved`,
`a boolean key cannot be saved`, `a table key cannot be saved`,
`a function key cannot be saved`, `a coroutine key cannot be saved`,
`a userdata key cannot be saved` and
`a key of this kind cannot be saved`.

The bytes may be 1 MiB, header included. Past that is
`the saved data is larger than 1 MiB`. The value may be 64 levels deep.
A save that cannot read the bytes back raises
`the saved data is damaged: ` and the place. The same bytes come out on every machine.
Timers and the brain's own stream are not kept. The brain starts again
from `start`.

`start(game, me, saved)` receives that value, or nil when the seat has
none. `saved` is kept only when the installed version and the package
digest both match the save.

### 10.1 What a save names

The Brains account holds the seats' text in a blob named `Seats` and each
seat's state in `State0` to `State9`. The seats' text is at most 16 KiB.
One state is at most 1 MiB. An account that cannot be written is
`the computer players' account cannot be saved`. The JSON members are
[4.1 The loadout entry](#41-the-loadout-entry).

A content note names `brain`, `brain-version` and `brain-level` for each
computer seat.

A recording stores the same brains text in its brain sector. Playback
does not run the brain.

### 10.2 Loading

Seats text that cannot be read sets
`The computer players this game was saved with cannot be read.`
A brain that is not installed, when the player did not ask to play the
built-in player instead, sets
`This saved game needs {name} {version}, which is not installed.`
`{name}` is the saved name, or the id when the name is empty. When the
player did ask to play the built-in player, that seat is skipped and the
built-in player plays it.

A level the installed manifest does not have falls back to the level for
the saved game's difficulty. When that level is missing too, the seat is
skipped. An option the manifest does not have, or a value the option does
not accept, is dropped, and the default is used when the option accepts
it.

When the saved version or the saved package digest differs from the
installed brain, the installed brain still plays and the saved Lua state
is left out. The run prints
`open-annihilation: brains: player `, the player index,
`: the saved state was left out: it was saved by `, and the saved version.

## 11. Compatibility

`works-with` is checked against the game being played: `*` for any mod
and the base game alone, `ta-3.1c` for the base game with no mod, and any
other id for a mod, with the range when one was written.
`requires.expansions` is checked against the expansions that are on.
`requires.engine` is checked against this build, with the range grammar
in [13. Versioning](../mutators/oamut-standard.md#13-versioning). The
grammar itself is
[3.1 Versions, ranges and conditions](../mutators/oamut-standard.md#31-versions-ranges-and-conditions).

A seat the package does not fit is not seated. The player sees
[12. When a brain fails](#12-when-a-brain-fails). The reasons a seat
refuses before Lua runs are listed in [14. Checks](#14-checks).

`fits(game)` runs after the package has loaded and before `start`. It
returns true, or a string the notice shows. The data `fits` reads is the
data the match loaded, after the mod and the mutators. `units`,
`weapons`, `features`, `game.mod`, `game.mutators` and `game.ai_profile`
are that data. A brain does not see the files from before those changes.

## 12. When a brain fails

A brain stops for an error in a hook or a timer, a spent instruction
allowance, the real-time backstop, the Lua memory cap, a string past
65,536 bytes, or a load the seat refuses. The sentences for the allowance,
the clock and the memory cap are [5.2 Budgets](#52-budgets).

A failed turn closes the sandbox and clears the seat's hooks before that
turn returns. The failing turn still counts as played. The next order tick
has no hooks, so the built-in computer player takes it. That player
already has a controller for the seat. Nothing about that controller is
rebuilt. The squad sort follows 30 order ticks later and reads the squads
the brain left.

A brain that is refused, fails to start, or does not fit never has hooks,
so the built-in player plays from the start. `fits` and `start` do not
report a stop. The app posts the seating notice. A defeated player is
simply no longer called. The sandbox closes with the match.

The notice uses the manifest name and the reason, cut to 160 bytes on a
character boundary. The three sentences are:

- `{brain} stopped ({reason}); the engine plays this seat on`
- `{brain} could not start ({reason}); the engine plays this seat`
- `{brain} does not fit this game ({reason}); the engine plays this seat`

When the seat has no reason of its own, the reason is
`the brain could not be seated`. A seat that never gets as far as a
notice also prints
`open-annihilation: brains: seat `, the player index, `: `, the id,
`: `, and the reason.

The log is [5.6 print and the log](#56-print-and-the-log). A stop writes
`tick `, the tick, `: stopped: `, the error, and the traceback under it.

## 13. Classic

Classic is an example. It is written in Lua. It plays as the built-in
computer player does, draw for draw, because it draws the match's shared
stream and writes the standing move and fire bits the way that player
writes them.

A seat is Classic only when its files are the files of this build's
`brains/classic/` folder: the same paths and the same bytes. The id, the
name, the manifest and the install path prove nothing. The test compares
fingerprints. One line per file, in byte order of the path, is the SHA-256
of that file in lower-case hex, a space, the path and a newline. The
installer's record `.oa-origin.yaml` is left out. The fingerprint is the
SHA-256 of that text. A missing folder, or one with no other file, has an
empty fingerprint, and then no package is Classic.

That test is the only grant for two behaviours:

- `brain.shared_random(bound)` draws the match's shared stream, the one
  unit scripts and the simulation draw from. `bound` is an integer from 0
  to 4294967295. Anything else raises
  `brain.shared_random: argument 1 must be an integer from 0 to 4294967295`.
  The result is the match's own bounded draw.
- `unit:stance` with `direct = true` writes the standing move and fire
  bits and gives no order. Any other seat raises
  `stance: direct writes are for the Classic brain only`.

`brain.setup_random(bound)` draws from a copy of that stream taken at the
seat's built-in set-up, and advances only the copy. The bound is checked
the same way. Before the set-up record exists, the call raises
`brain.setup_random: the seat is not set up yet`.

With a match, `brain.shared_random` is also installed for a shadow. With
no match it is installed only for Classic, and only when the host provides
the draw. Otherwise it is nil. A call raises
`brain.shared_random: there is no match` when there is no match to draw,
and `brain.shared_random: the stream is not ready` when the stream is not
ready. A shadow draws a copy of the stream. Its orders are recorded and
not applied. The direct write is not given to a shadow.
`brain.setup_random` is installed for Classic and for a shadow whether or
not a match is open. A bad bound raises
`brain.setup_random: argument 1 must be an integer from 0 to 4294967295`.
Any other brain has neither function. A shadow is a tooling run, named on
[the command line](../command-line.md). A package does not claim it.

`brain.section(name)` is installed for every brain. `name` is 1 to 15
letters, digits or hyphens. Anything else raises
`brain.section: a section name is up to 15 letters, digits or hyphens`.
In a game the mark does nothing.

Full awareness is not in this list. It is the manifest flag.

## 14. Checks

Packing, installing and loading refuse the same layout. The messages
below are the ones a pack, an install or a seat shows. Shared zip
refusals that every package kind uses stay in
[Packages](../registries/packages.md).

### 14.1 Packing and installing

| What is wrong | Message |
| --- | --- |
| The file cannot be read | `It cannot be read.` |
| Not a zip | `It is not a zip archive.` |
| Damaged | `It is damaged.` or `It is damaged: {name} does not unpack as recorded.` |
| No manifest | `It holds no oabrain.yaml at its top, or in a single folder at its top.` |
| Manifest too big | `Its oabrain.yaml is larger than 256 KiB.` |
| Manifest errors | `Its oabrain.yaml has errors:` |
| An unsafe name | `It holds a file it cannot unpack safely: {name}.` |
| Two names that differ only in case | `It holds two names that differ only in case: {name}.` |
| A link | `It holds a link to another file: {name}.` |
| Encrypted | `It holds an encrypted file: {name}.` |
| A compression the installer cannot unpack | `It holds a file packed in a way it cannot unpack: {name}. Pack it again with ` and `standard (deflate) compression.` |
| Unpacked size | `It unpacks to {size}, more than the {limit} a brain may take.` |
| A zip bomb | `It unpacks to {size} from {packed}, far more than a brain's files take.` |
| Too many folders | `It holds more than {limit} folders, more than a brain may take.` |
| No disk space | `It needs {size} free, and the disk holding your Brains folder has {free}.` |
| A reserved id | `Its id, {id}, cannot name a folder on Windows.` |
| No free folder | `Your Brains folder has no free folder name for it.` |
| Not this kind | `It is not a kind of package this game installs.` |
| Engine range | `It needs a different Open Annihilation.` |
| A folder that is not this brain | `The folder {name} does not hold this brain. It is left as it is.` |
| Script missing or too big | `Its script {name} is missing.` or `Its script {name} is larger than 1 MiB.` |
| Precompiled Lua | `{name} is precompiled Lua. A brain holds Lua source only.` |
| Outside the layout | `The package holds {name}, outside the files a brain may hold.` |
| Icon | `Its icon {name} is not a square PNG of at most 256 pixels and 256 KiB.` |
| Network missing | `Its network {name} is missing.` |
| Networks too big | `Its networks take {size}, more than the {limit} a brain may hold.` |
| Data table too big | `Its table {name} is larger than 4 MiB.` |
| Network digest | `Its network {name} does not match the SHA-256 its oabrain.yaml gives.` |

The install dialog's title is `BRAIN NOT INSTALLED`. A pack reports a
file outside the layout as the path and `: outside the files a brain may hold`,
and a network hash as the file, `: its SHA-256 is `, the bytes' digest,
`, not the `, the manifest's digest, and ` oabrain.yaml gives`.

Manifest messages are in [3. The manifest](#3-the-manifest). The line is
`oabrain.yaml:<line>:<column>: <key>: <message>`.

### 14.2 Loading a seat

| What is wrong | Message |
| --- | --- |
| Player index | `the player is outside 0 to 9` |
| No such player | `the player is not in the game` |
| Not a computer | `the player is not a computer` |
| Already seated | `the seat already has a brain` |
| Id, name, version, level, entry | `the brain id is longer than 64 bytes`, `the name is longer than 128 bytes`, `the version is longer than 32 bytes`, `the level is longer than 64 bytes`, `the entry path is longer than 256 bytes` |
| Options | `the seat has more than 16 options`, `an option has no id`, `an option id is longer than 64 bytes`, `an option value is longer than 64 bytes` |
| Entry missing | the path, then ` is not among the package files` |
| Networks too big | `the networks are larger than 64 MiB` |
| Digest | the path, then ` does not match its sha256 in oabrain.yaml` |
| Network build | `network %.*s: %.*s` |
| Package too big to read | `the package is larger than 128 MiB` |
| The seat could not be stored | `the seat could not be stored` |
| No reason | `the brain could not be seated` |

`oa-tool brain check` seats the brain, so `load`, `fits` and `start` run,
and it does not tick the match. A phase that did not run is `not run`.

## 15. Versioning

`oabrain` is the integer 1. A later format is refused. `1.0` and `"1"`
are `oabrain must be 1`.

`version` is a mutator version of one to three parts, written in quotes.
A plain number, and an unquoted scalar, are
`write the version in quotes, as "2.0"`. A quoted text that is not a
mutator version is `version must be a mutator version, such as ` and a
quoted version, the example in that same sentence.
Anything else is `version must be a string`. A missing part compares as
0, so `2`, `2.0` and `2.0.0` are equal. The writer still prints as many
parts as were written. The range grammar another package would use to
name this version is
[3.1 Versions, ranges and conditions](../mutators/oamut-standard.md#31-versions-ranges-and-conditions).

`requires.engine` uses that standard's engine range
([13. Versioning](../mutators/oamut-standard.md#13-versioning)):
three-part releases, one to four comparisons, at most 64 bytes, and the
value quoted when it starts with `>`. The installer and `oa-tool brain check`
ask whether this build meets it. Reading the manifest does not.

## 16. Glossary

| Term | Meaning |
| --- | --- |
| Brain | A `.oabrain` package that plays one computer seat. |
| Seat | One computer player of this machine, and the sandbox that plays it. |
| Controller point | The moment in a tick, after the unit sweep, when the seat's hooks run. |
| Built-in computer player | The engine's own computer player. It stays the default and stays in the engine. |
| Level | One row of `levels`: an id, a name and a difficulty that sets the seat's income. |
| Full awareness | `fullAwareness: true`. The brain sees units through the fog, and its site checks do not need a placing player. Allowed in a network game. |
| Allowance | The instructions one phase or one turn may run. |
| Bank | The balance a turn spends. It gains 2,000,000 instructions a tick and holds at most 30 ticks. |
| Loadout entry | One object in the seats JSON: `brain`, `level`, `name`, `options`, `package_sha256`, `place` and `version`. |
| Handle | The Lua object for one unit slot and one generation. |
| Settlement | A player's economy settling, once a second of game time: the second's income comes in and the units' requests are paid. |
| `fixed` | A 16.16 count, 65536 to one world unit. |
| `f32` | One binary32 value, the width of a player's resources. |
| Network | A format-1 `.oann` file the manifest names, run with `brain.network`. |
| Classic | The example brain whose files match this build's `brains/classic/` bytes. It draws the shared stream and may write standing bits directly. |
