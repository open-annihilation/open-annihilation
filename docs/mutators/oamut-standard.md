# The OAMUT standard

A mutator package, `oamut.yaml` inside a `.oamut` file, changes one game.
The manifest names the mutator. Its scripts and rule files say how that
game differs from the base game or from a mod. Open Annihilation reads the
manifest without running any code, then runs the scripts in a sandbox
before the match.

This document is the format and its rules as the engine implements them.
The archive a package is shipped in, and the catalogue entry a registry
publishes, are described with the other package kinds. This standard names
what is different for mutators and does not repeat those rules.

## Contents

- [1. Goals and principles](#1-goals-and-principles)
- [2. The package](#2-the-package)
- [3. The manifest](#3-the-manifest)
- [4. The entry point](#4-the-entry-point)
- [5. The Files layer](#5-the-files-layer)
- [6. Rules](#6-rules)
- [7. Hooks and collections](#7-hooks-and-collections)
- [8. Hacks](#8-hacks)
- [9. Stacking](#9-stacking)
- [10. The sandbox](#10-the-sandbox)
- [11. Checks](#11-checks)
- [12. Multiplayer, replays and saves](#12-multiplayer-replays-and-saves)
- [13. Versioning](#13-versioning)
- [14. Glossary](#14-glossary)

## 1. Goals and principles

A mutator changes one game. The script that describes the change runs in
a sandbox, and it runs before the match. The match loads ordinary files.
No mutator code runs while the match runs.

| Goal | What it means in practice |
| --- | --- |
| Sandboxed script | A mutator reaches the game only through the sandbox. It has no file, network, clock or environment of its own. A script that fails stops that mutator. The match does not start, unless the only failure is a refused hack. |
| Before the match only | The script runs, and its files and rules are written, before the match loads. During the match the engine reads those files the way it reads any mod. |
| Per game | Every mutator applies to the game being set up. The next game starts from the base game or the mod again. Nothing a mutator writes is kept for a later launch. |
| Open Annihilation only, and unranked | Mutators are for games played in Open Annihilation. A mutated game is not a ranked game. |

The principles that follow:

1. **Text, then the match.** A mutator rewrites the text the match already
   knows how to load. It does not hand the simulation a private record.
2. **One manifest.** `oamut.yaml` is what the packer writes, what the
   installer checks and what the Library lists. A key spelled wrong is
   refused, so it is noticed. A key that belongs in a mod profile is
   refused with its own message.
3. **The same result on every machine.** The sandbox's numbers, the order
   it walks tables, and the text it writes do not depend on the machine.
   A real-time limit can stop a script on a slow machine and let it finish
   on a fast one. Every machine must produce the same result.

## 2. The package

A mutator is one `.oamut` file, or the folder that file unpacks to. The
archive is the zip every package kind uses. The names it may hold, and the
limits on its size, are
[Packages](../registries/packages.md). They are not defined a second time.

The manifest is the strict YAML a mod profile uses
([3.3 Strict YAML rules](../mods/oamod-standard.md#33-strict-yaml-rules)).
One document, one top-level mapping, no anchors and no block scalars.

### 2.1 Folder layout

The top of the package holds `oamut.yaml`. Beside it, a file is one of
these:

| Path | What it is |
| --- | --- |
| `oamut.yaml` | The manifest. Required. |
| The entry script | Lua source. The default name is `mutator.lua`. `entry` names another. The file has to be in the package. A missing one is refused with `Its script <entry> is missing.` |
| `lua/` and a name ending `.lua` | Further Lua source. `require` reads only these files. |
| `rules/` and a name ending `.sm` | Rule files. |
| `files/` and anything under it | Files and archives the Files layer may add. |
| A top-level name ending `.md` or `.txt` | A note for people. The game does not load it. |
| The icon | The PNG named by `icon`, when the manifest names one. |

A file outside that list is refused. The comparison ignores the case of
ASCII letters. An entry script is not placed under `files/`, `rules/` or
`lua/`.

```text
steady-hands/
  oamut.yaml
  mutator.lua
  icon.png
  lua/hands.lua
  rules/economy.sm
  files/units/ridgecom.fbi
  README.md
```

### 2.2 The .oamut file

`oa-tool mutator pack` and `oa-tool mutator check` are the pack and check
commands limited to a mutator. How a package is packed, which files are
stored, the default name, `--out`, `--force`, `--game-dir`, the problem
lines and the report are [Packages](../registries/packages.md). A mutator's
revision in that report is null.

What those commands add for a mutator:

- They compile the entry script and every file under `lua/` whose name
  ends `.lua`. A `.lua` file under `files/` is not compiled. The
  installer's size and precompiled-source checks still cover every
  `.lua` file, as [2.4 Size limits](#24-size-limits) says.
- They parse every file under `rules/` whose name ends `.sm`. A rule file
  larger than 4 MiB is refused before it is parsed. The rule reader
  refuses a text longer than 1 MiB, so a rule file past 1 MiB is refused
  either way. A rule-file warning is not shown, and it does not refuse the
  package. An error does, and it is shown.
- Neither command runs a script or applies a rule.
- A usage error, such as a missing folder or an argument the command does
  not take, exits 2. A package the command refuses exits 1.
- `mutator check` exits 1 when the installer would refuse the package, and
  also when one of those scripts does not compile or a rule file has an
  error. The installer would accept a package whose only fault is a
  script that does not compile or a rule file that has an error.
- `--out` must name a file ending in `.oamut`. After pack writes the file,
  it reads that file back with the installer and refuses a package the
  installer refuses.

### 2.3 Where it installs

Into `Documents/Open Annihilation/Mutators/<id>`, by the manifest's `id`,
never by the file's name. Installing does not turn the mutator on. The
player turns it on when setting up a game.

Where that folder is on each system is
[Where it keeps its files](../installation/linux.md#where-it-keeps-its-files)
on Linux, and the same heading on
[macOS](../installation/macos.md#where-it-keeps-its-files),
[Windows](../installation/windows.md#where-it-keeps-its-files) and
[Steam Deck](../installation/steam-deck.md#where-it-keeps-its-files).
How a registry serves a package is [Packages](../registries/packages.md).

The install plan is decided from the folder that is already there. The
first of these that fits decides:

1. A folder that is missing: a catalogue package installs with no
   question, and a file the player opened asks before it installs.
2. A folder that holds something else is refused.
3. A catalogue package over one from the same registry replaces it with
   no question, whatever the two versions are.
4. A catalogue package over one from a different registry asks whether
   to replace it. The version installed is kept and can be brought back.
5. The same version already installed asks whether to reinstall.
6. Any other version asks whether to update.

The revision stored for an installed mutator is 0.

### 2.4 Size limits

The limits every package meets, a mutator included, are
[Packages](../registries/packages.md). That covers the manifest's size,
the archive's size, the spare path length, and the name, link, encryption
and compression rules.

A mutator also meets these limits. The Lua limit applies to every `.lua`
file in the package, including one under `files/`:

| Limit | Value |
| --- | --- |
| One Lua file, the entry included | 1 MiB. An empty file is source. A file whose first byte is `0x1B` is precompiled Lua and is refused. |
| The icon, when named | A PNG, square, at least 1 pixel and at most 256 pixels on a side, and at most 256 KiB. Every icon failure shares one sentence. |
| `requires.engine`, when written | This build's version meets the range. The refusal names the range and this build. |

## 3. The manifest

`oamut` is the integer `1`. A key that is not in this table is refused.
`1.0` and `"1"` are refused. The writer prints the keys in the order of
the table, with two-space indentation, every string in double quotes, and
a short list as a flow sequence. An optional key that was left out is left
out again, and so is `entry` when it is `mutator.lua`.

A profile key is refused with
`<key> belongs in a mod's oamod.yaml; a mutator cannot carry it`. Those
keys are `identity`, `layout`, `limits`, `script-extensions`, `data-keys`,
`hacks`, `settings`, `media`, `packaging`, `supportedGames` and
`requiredExpansions`. `hacks` inside `layers` is a layer name. Any other
unknown key is refused with `<key> is not a key of oamut.yaml`.

`homepage`, `tags` and `requires.engine` follow the shared package-key
rules. Tags are then limited to four. When the shared rules already report
a tag problem, the four-tag limit is not reported again.

| Key | Type | Required | Limits | Meaning |
| --- | --- | --- | --- | --- |
| `oamut` | integer | yes | `1` | The format. |
| `id` | string | yes | 1 to 64 bytes, lower-case kebab-case | The mutator's id. No leading, trailing or doubled hyphen. The catalogue uses the same rule for a package key. |
| `name` | string | yes | 1 to 64 bytes, no control character | The name players see. |
| `version` | string | yes | A mutator version, written in quotes, at most 32 bytes | One to three parts joined by `.`, each a decimal from 0 to 65535 with no leading zero. A plain number, and a plain scalar such as `2.0.1`, are refused. |
| `author` | mapping | yes | `name`, and optionally `email` | Who wrote the mutator. |
| `author.name` | string | yes | 1 to 128 bytes | The author or team. |
| `author.email` | string | no | At most 254 bytes | One `@`, text on both sides, a dot in the part after it, that part not starting or ending with `.`, and no space or control character. |
| `description` | string | no | 1 to 2,000 bytes | A line feed is allowed. No other control character. The first line, cut to 200 bytes on a UTF-8 boundary, is the summary the Library and the catalogue show. |
| `homepage` | string | no | 1 to 256 bytes | An `http` or `https` address, the scheme in either case, with at least one byte after the scheme and no space or control character. |
| `icon` | string | no | A package path ending `.png` | The ending matches in either case. Parts separated by `/`. No part is empty, `.` or `..`. No leading `/` or `\`, and no `\` anywhere. The installer checks the pixels. |
| `tags` | list of strings | yes | 1 to 4 | Each tag is lower-case kebab-case of 1 to 32 bytes. None is repeated. An empty list is refused. The key is required, so the list holds one to four tags. |
| `works-with` | list of strings | yes | 1 to 32 | Conditions, below. No id twice. |
| `requires` | mapping | no | Only `engine` and `mutators` | Anything else in the mapping is refused. |
| `requires.engine` | string | no | At most 64 bytes | An engine requirement, written in quotes. |
| `requires.mutators` | list of strings | no | 1 to 16 | Conditions on mutator ids. The order written is the order those mutators apply in, all before this one. |
| `conflicts-with` | list of strings | no | 1 to 32 | Conditions on mutator ids this mutator refuses to run beside. |
| `layers` | list of strings | yes | 1 to 9, none repeated | The nine names below. Each name is what a plan, a hook or a rule may write. |
| `clashes` | string | no | `add` or `replace` | What this mutator does with a unit or weapon whose name is already in the game. Absent leaves the choice to the player, and then to the preference `mutators.clash-default`, which is `replace` when unset. |
| `options` | mapping | no | At most 16 option ids | Settings the player picks before a match. |
| `strings` | mapping | no | At most 16 language tags | Translations of the name, the description and the option labels. |
| `entry` | string | no | A package path ending `.lua` | The entry script. The default is `mutator.lua`. It is not under `files/`, `rules/` or `lua/`. |
| `legacy` | list of mappings | no | 1 to 16 records | TA: Mutation mutators this package was imported from, in the order written. A bare mapping is refused. |

A problem names the value's path, such as `options.factor.default`,
`requires.mutators[0]` or `strings.de.options.factor`. Indexes count from
0. The line and column are the value's, 1-based, in bytes.

### 3.1 Versions, ranges and conditions

A missing version part compares as 0, so `2`, `2.0` and `2.0.0` are equal.
The writer still prints as many parts as were written.

A version range is one to four comparisons (`>=`, `>`, `<=`, `<` or `=`),
separated by commas that spaces may surround, at most 64 bytes. It is the
engine requirement's grammar with mutator versions in place of three-part
releases, so `>= 4.0` and `>= 2` are ranges. A range that holds a comma is
quoted when it sits in a flow list. In a block list, `- ridge >= 1, < 2`
is accepted as written.

`requires.engine` uses three-part releases, `MAJOR.MINOR.PATCH`, each part
a decimal from 0 to 65535 with no leading zero. The same one to four
comparisons apply, and the whole text is at most 64 bytes. A value that
starts with `>` is a block scalar unless it is quoted, and the manifest
reader refuses that before the range is read. Reading the manifest does
not ask whether this Open Annihilation meets the range. The installer and
`oa-tool mutator check` do.

A condition is an id alone, or an id, one or more spaces, and a range. An
id is lower-case kebab-case of 1 to 64 bytes, `*` or `ta-3.1c`. `*` and
`ta-3.1c` take no range. Ids are compared byte for byte. A condition with
no range is met by its id at any version text. A range is met only when
the version text is a mutator version inside the range, so a mod version
such as `5b` misses.

In `works-with`, `*` means any mod and the base game played alone, and it
is the only entry when it appears. `ta-3.1c` means the base game with no
mod. Any other id is a mod.

In `requires.mutators` and `conflicts-with` an id is a mutator id. It is
not this mutator's own id. An id in `conflicts-with` is not an id
`requires.mutators` names. No id is repeated in one list.

### 3.2 Tags

The reader accepts any tag that is lower-case kebab-case of 1 to 32 bytes,
at most four of them. The Library draws an icon, and a translated label,
for these eight ids, matched exactly:

| Id | What the Library files it under |
| --- | --- |
| `statistics` | Statistics |
| `economy` | Economy |
| `commanders` | Commanders |
| `new-units` | New Units |
| `disable` | Disable |
| `gameplay` | Gameplay |
| `ai` | AI |
| `maps` | Maps |

Any other id is shown as it was written, with no icon of its own. When the
manifest names no `icon`, the badge uses one tag: the catalogue listing's
tags when that list has any, and otherwise the installed manifest's tags.
The icon is the first tag's, when that id is one of the eight. An id
outside the eight shows no icon, and a later tag is not tried.

### 3.3 Layers

`layers` names the parts of the game the mutator uses. The names, in the
order the Library shows them, are:

| Name | What a plan, a hook or a rule may write |
| --- | --- |
| `files` | `files.mask` and `files.add`. The paths are [5. The Files layer](#5-the-files-layer). |
| `units` | A `units` function, and a rule whose target is the units folder. |
| `weapons` | A `weapons` function, and a rule whose target is the weapons folder. |
| `features` | A `features` function, and a rule whose target is the features folder. |
| `sides` | A `sides` function, and a rule whose target is `sidedata.tdf`. |
| `tables` | A `tables` function, and a rule whose target is any other folder. A rule on `maps/` is the `maps` layer, not this one. |
| `menus` | A `menus` function. |
| `maps` | A `maps` function or a list of map blocks, and a rule on `maps/*.ota`. |
| `hacks` | The `hacks` table. The parameters it may set are [8. Hacks](#8-hacks). |

A plan key is refused when the manifest does not declare the layer that
key writes. A `rules` list is judged target by target: a block aimed at
the units folder needs `units`, even when another block in the same list
needs `tables`. The files a `files` layer may add are
[5. The Files layer](#5-the-files-layer).

### 3.4 Options

`options` is a mapping of at most 16 entries. The key is the option id: a
Lua name, `[a-z][a-z0-9_]*`, of 1 to 32 bytes. The reader refuses an id
that is a Lua keyword, such as `end`. Each option is a mapping.
The keys any option may hold
are `type`, `label`, `default`, `values`, `labels`, `min`, `max` and
`step`. A key its type does not use is refused.

`type` is `toggle`, `choice` or `number`. `label` is 1 to 64 bytes with no
control character. The words a player reads are the label.

| Type | Keys | Rule |
| --- | --- | --- |
| `toggle` | `type`, `label`, `default` | `default` is `true` or `false`. |
| `choice` | `type`, `label`, `default`, `values`, and optionally `labels` | `values` holds 2 to 16 entries, all numbers or all identifiers, none repeated. `default` equals one of them. An identifier is 1 to 32 bytes of ASCII letters, digits, `_`, `-` and `.`. `labels`, when written, lists one label of 1 to 64 bytes for each value, in the same order. |
| `number` | `type`, `label`, `default`, `min`, `max`, `step` | `min`, `max`, `step` and `default` are exact numbers, the decimals the strict YAML reader keeps. `min` is below `max`. `step` is above 0. `(max - min) / step` is a whole number of at most 1,000 steps. `default` lies on a step, including either end. |

### 3.5 Strings

`strings` maps a language tag to a translation. A tag is 2 to 35 bytes: a
run of letters, then hyphen-joined parts of letters and digits, in either
case. Each language is a mapping of optional `name`, `description`,
`options` and `labels`. Any other key is refused.

`name` is 1 to 64 bytes with no control character. `description` follows
the description rule. `options` maps an option id to a label of 1 to 64
bytes with no control character. `labels` maps a choice option's id to a
list of value labels, one of 1 to 64 bytes with no control character for
each value, in the same order as `values`. An option id the manifest does
not define is refused, and so are value labels for an option that is not a
choice.

### 3.6 Legacy

Each record in `legacy` is a mapping of `name`, `prefix`, `compatibility`
and `files`.

| Key | Limits |
| --- | --- |
| `name` | 1 to 128 bytes, no control character. The TA: Mutation name. |
| `prefix` | `"{}"` or `"}{"`. |
| `compatibility` | 1 to 8 codes. Each code is 1 to 8 letters and digits, or `*`. `*` may sit beside other codes. |
| `files` | 1 to 16 file names of 1 to 255 bytes, with no control character and no `/` or `\`. |

`oa-tool mutator import` reads TA: Mutation `.tam` files and zips and
writes one record for each original. Curating the package keeps those
records. Folding several variants into one package lists every original
as its own record, which is what the packages under `mutator-packages`
do. Its warnings and its flags are
[the tool](../../tools/oa-tool/README.md) and
[the importer](../../src/data/mutator-import/README.md).

### 3.7 A full example

Steady Hands makes every mobile builder faster. It fits the made-up mod
Ridge and the base game, and it refuses to run beside Long Shadows.

```yaml
oamut: 1
id: steady-hands
name: Steady Hands
version: "1.2"
author:
  name: A. Builder
  email: builder@example.com
description: "Builders move a little faster.\nThe second line stays in the description. The first line is the summary."
homepage: "https://example.org/steady-hands"
icon: icon.png
tags: [economy, gameplay]
works-with:
  - ridge >= 1
  - ta-3.1c
requires:
  engine: ">= 0.8.5"
  mutators:
    - long-reach >= 1
conflicts-with:
  - long-shadows
layers: [units, files]
options:
  pace:
    type: choice
    label: How fast
    values: [2, 4]
    default: 2
    labels: [Quicker, Rapid]
  announce:
    type: toggle
    label: Announce each builder
    default: false
  extra:
    type: number
    label: Extra metal
    min: 0
    max: 100
    step: 5
    default: 0
strings:
  de:
    name: Ruhige Hände
    description: "Die Bauarbeiter sind etwas schneller."
    options:
      pace: Wie schnell
    labels:
      pace: [Schneller, Rasch]
entry: mutator.lua
legacy:
  - name: Steady Hands
    prefix: "{}"
    compatibility: ["OTA"]
    files: ["steady.sm"]
```

A plain value such as `ridge >= 1` is read as text. A value is quoted
when it would otherwise be something else: a leading `>` is a block
scalar, so `requires.engine` is quoted; `*` is an alias unless it is
quoted; a range that holds a comma is quoted when it sits in a flow
list. A description that holds a line break is one double-quoted string,
with `\n` for the break. A block scalar (`|` or `>`) is refused. The
summary is `Builders move a little faster.`

### 3.8 Clashes

`clashes` is `add` or `replace`, compared byte for byte. `Add`, `merge`
and a number are refused with `clashes must be add or replace`.

`replace` puts this mutator's unit or weapon in place of the record that
already has that name. `add` brings the new record in beside it. A record
that matches, after this mutator's own weapon names are rewritten, is
shared and the existing record stays. A record that differs is stored
under a private name, and the note says the mutator renamed it. In that
mode, this mutator's `units.NAME`, `weapons.NAME`, a rule's `is("NAME")`,
a rule write, a clone's source and a build-menu builder mean the private
record. A later mutator's use of the original name is left alone.

When the key is absent the reader does not fill one in. The mode in force
is the player's choice, else this key, else the preference
`mutators.clash-default`. That preference is `replace` when it is unset
or not `add` or `replace`. The loadout records the mode that played.

## 4. The entry point

The entry script defines `mutate`. The engine calls `mutate(ctx)` once,
in the plan phase, and reads the table it returns. The call does not
change the game by itself. The table names the work the later phases do.

`mutate` has to be a function. A script that does not define it is refused
with `the entry does not define mutate`. The return has to be one table.
`mutate must return a table` is the refusal otherwise. A table with a
metatable is refused: the plan is read by raw access, so a metamethod
would not be what the script meant. A key that is not text is refused
with `a plan key must be text`. A key the engine does not know is refused
with `the plan key <name> is not supported by this engine`, and the first
such name in byte order is the one named. An empty table is a plan that
changes nothing.

Those four faults, a return that is not a table, a metatable, a key that
is not text, and an unknown key, are reported before any one key is
read. A hook that is not a function is `<key> must be a function`. A
hook whose layer the manifest does not declare is
`returns a <key> hook but its manifest does not declare the <key> layer`.

The known keys, and the order their faults are reported, are `features`,
`files`, `hacks`, `maps`, `menus`, `rules`, `sides`, `tables`, `units`
and `weapons`. The first fault is the one the player sees.

| Key | What it holds | Refused when |
| --- | --- | --- |
| `files` | A plain table of `mask` and `add`, each a list of text. Either may be left out. | It is not a table, it has a metatable, a list is not text, or it holds another key. The manifest must declare `files`. |
| `rules` | A plain list of finished blocks and `sm` paths. See [6. Rules](#6-rules). | It is not a list, or it has a metatable. Each target's layer must be declared. |
| `hacks` | A plain table. See [8. Hacks](#8-hacks). | It is not a table, or it has a metatable. The manifest must declare `hacks`. |
| `maps` | A function `maps(map, ctx)`, or a plain list of finished `where():change()` blocks. | It is neither, or the list has a metatable or a key the map cannot write. The manifest must declare `maps`. |
| `units` | A function `units(units, ctx)`. | It is not a function. The manifest must declare `units`. |
| `weapons` | A function `weapons(weapons, ctx)`. | It is not a function. The manifest must declare `weapons`. |
| `features` | A function `features(features, ctx)`. | It is not a function. The manifest must declare `features`. |
| `sides` | A function `sides(sides, ctx)`. | It is not a function. The manifest must declare `sides`. |
| `tables` | A function `tables(tables, ctx)`. | It is not a function. The manifest must declare `tables`. |
| `menus` | A function `menus(menus, ctx)`. | It is not a function. The manifest must declare `menus`. |

A files key the engine does not know is
`the plan key files.<name> is not supported by this engine`.

### 4.1 The context

`ctx` during `mutate` has four fields.

| Field | What it holds |
| --- | --- |
| `ctx.mod.id` | The mod's id. With no mod, the base game's id, `ta-3.1c`. |
| `ctx.mod.version` | The mod's version text. Empty when no mod is on. |
| `ctx.mod.game` | The base game this match plays. |
| `ctx.engine.version` | This build's version text. |
| `ctx.options` | The player's choices, one field per option id. A toggle is `true` or `false`. A number, and a choice whose values are numbers, is a number. A choice whose values are identifiers is text. An option the player left is the manifest's default. |
| `ctx.incompatible` | A function. Calling it stops this mutator. See below. |

Each call receives a new `ctx`. A field `mutate` sets on its table is
gone when a hook runs. By then `ctx` also holds `units`, `weapons`,
`features`, `sides`, `tables` and `menus`. `ctx.features`, `ctx.sides`
and `ctx.tables` are the same objects the hook receives. The units,
weapons and menus on `ctx` are different objects from the ones passed
in, so `units == ctx.units` is false inside the units hook, and the
same is true of weapons and menus. `mutate` itself does not receive
the collections: they are not loaded until the plan has been read.

The maps function's `ctx` has only `mod`, `engine`, `options` and
`incompatible`. It does not hold the collections.

`ctx.incompatible(reason)` copies the reason and then raises
`incompatible`, so a script that does not catch the error stops. The
reason is text. Anything else, and empty text, is stored as
`incompatible with this mod`. The copy keeps at most 1,023 bytes, and
the player sees that text as one line of at most 200 bytes. A `pcall`
that catches the error does not keep the mutator. The pass is run again
without it, so a write it had staged is dropped. The call works in
`mutate` and in every hook, including the maps function.

```lua
function mutate(ctx)
  if ctx.mod.id ~= "ridge" and ctx.mod.id ~= "ta-3.1c" then
    ctx.incompatible("Steady Hands is for Ridge or the base game")
  end
  local pace = fixed(ctx.options.pace)
  return {
    units = function(units, ctx)
      units:where { builder = true, bmcode = 1 }:each(function(u)
        u.maxvelocity = u:fixed("maxvelocity") * pace -- see 10.9
      end)
    end
  }
end
```

## 5. The Files layer

The Files layer hides files that are already in the game and adds files
from the package. Both happen before the match, and the match then reads
ordinary paths. A mask hides. An add folder contributes.

A plan lists `mask` patterns and `add` folders, both in the order written,
and both optional. The mutators are taken in the player's list order. For
each mutator that is kept, the masks are applied first and then the add
folders. A mask is kept on the layer, and every file an earlier mutator
added that it matches is removed. The note names both mutators. The
mutator's own files are added after its masks, so a mask hides files from
earlier mutators and leaves that mutator's own add in place.

An add folder is a relative path inside the package: no `..`, no drive and
no leading slash, and not a link. Each part is matched to the directory's
entries without ASCII case, so `FILES` and `files` are the same folder on
every machine. Two entries that fold to one name refuse the mutator. A
folder that is not there adds nothing and records that the folder is
missing. That is what a plan written as `files/<mod id>` relies on when
that mod has no files of its own.

Inside a folder that exists:

- Only the top of the folder is searched for archives, and only `.ccx` and
  `.ufo` are opened. An archive further down is an ordinary loose file, as
  is any other kind of file.
- `.ccx` archives are opened before `.ufo` archives. Within each group the
  names follow NTFS order: compared byte by byte after ASCII upper-casing,
  with the raw bytes breaking a tie. The earlier name keeps a path both
  archives hold.
- Loose files are walked after the archives. A loose file keeps a path over
  that folder's archives. Listings are sorted by the folded path, so the
  machine's own directory order never decides a file.
- A path this mutator already added from an earlier folder is replaced, with
  no note. A path an earlier mutator added is replaced in the layer's
  one-file table, and the earlier file is kept so the pipeline can still
  read it. A unit file (one level under the units folder, with the
  layout's unit extension), a weapon file (a `.tdf` one level under the
  weapons folder) and a download file (a `.tdf` one level under the
  download folder) are also snapshotted, one copy for each mutator that
  added the path. The pipeline reads those copies as records. A nested
  path, and every other file, stays one whole file, and the later mutator
  wins that path. A mask that hides a path drops its snapshots and the
  files it displaced.

The layer lists paths in mutator order and, within a mutator, in the order
the paths were first added. Each folder's new paths are sorted by the
folded path. Replacing a path does not move it. The archives a kept entry
still reads are listed in the order they were opened, and an archive whose
files were all replaced is omitted. The layer's label is `mutators`
followed by the ids that were kept, separated by spaces.

A mutator that fails leaves nothing behind. Its masks, paths, archives and
notes are dropped, and the mutators after it are built as though it were
not in the list. One mutator failing does not fail the others while the
layer is built. The match still does not start, unless the only failure
is a refused hack. Two machines with the same packages and the same list
build the same paths, owners, archive order, masks and label.

A mask hides a path an earlier mutator added. It does not hide a path a
later mutator adds: a file the layer's table holds wins over every mask.

### 5.1 Masks

A mask matches a resource path. ASCII case is ignored, and `\` matches `/`.
`*` matches any run of characters inside one segment. `?` matches one
character inside a segment. A segment that is exactly `**` matches any
number of whole segments, including none. `**` anywhere but as a whole
segment is refused.

A pattern is refused when it is empty, longer than 256 bytes, absolute,
holds `:`, holds a NUL, holds an empty, `.` or `..` segment, has a first
segment that is exactly `.backup` in any case, or holds more than 16
`*` and `?` counted together. A refused pattern matches nothing.

| Limit | Value | What it counts |
| --- | --- | --- |
| Masks in one plan | 64 | `mask` patterns one mutator lists |
| Add folders in one plan | 16 | `add` folders one mutator lists |
| Archives in one add folder | 16 | `.ccx` and `.ufo` files at the top of that folder |
| Loose files under one add folder | 16,384 | Files walked under that folder |
| Nested folders under one add folder | 32 | How deep the walk goes |
| Masks in the whole layer | 2,048 | Every kept mutator's masks |
| Paths in the whole layer | 65,536 | Served and hidden paths together |
| Archives in the whole layer | 512 | Archives a kept entry still reads |
| Bytes held in memory | 256 MiB | Content the layer holds in memory. File bytes read from disk or from an archive are not counted here. |

Crossing a plan or folder limit fails that mutator. Crossing the mask,
path or archive limit of the whole layer fails the mutator that would
cross it. The 256 MiB bound is applied when the layer is mounted, and
only to content held in memory. Crossing it refuses the mount.

### 5.2 The allow-list

Every path an add contributes, and every mask, is judged against one
table. The default is to refuse. A path longer than 4,096 bytes is
over-long. A path of 4,096 bytes is not. An empty path, a path that holds
a NUL, an absolute path, a path that holds `:`, and a path with an empty,
`.` or `..` segment, are refused.

Names are compared without ASCII case. `/` and `\` both separate segments.
A first segment `<folder>-<variant>` follows that folder's row, for every
folder in the table. The seven directory names a mod may rename come from
the active layout. In the base game they are `units`, `Weapons`,
`gamedata`, `ai`, `guis`, `unitpics` and `download`. A renamed directory
replaces the base name, and the base name is then an unknown folder.

A mask names a folder before any wildcard. A first segment that holds `*`
or `?` is refused with `a mask must name a folder before a wildcard`. A
folder the table allows as a whole may use a wildcard after that folder.
A folder the table allows file by file, `gamedata/` and `anims/` among
them, refuses a wildcard: the refusal is `a mask here must name one file`.
A mask with no wildcard is judged as that one path.

| Path | Verdict | Why |
| --- | --- | --- |
| `units/` | allowed, the whole tree | Read again for every match. The layout's units folder. |
| `Weapons/` | allowed, the whole tree | Read again for every match. The layout's weapons folder. |
| `ai/` | allowed, the whole tree | Read again for every match. |
| `unitpics/` | allowed, the whole tree | Read again for every match. |
| `download/` | allowed, the whole tree | Read again for every match. |
| `features/` | allowed, the whole tree | Read again for every match. |
| `objects3d/` | allowed, the whole tree | Read again for every match. |
| `scripts/` | allowed, the whole tree | Read again for every match. |
| `gamedata/moveinfo.tdf` | allowed | Read again for every match. |
| `gamedata/sidedata.tdf` | allowed | Read again for every match. |
| `gamedata/los.tdf` | allowed | Read again for every match. |
| `gamedata/meteor.tdf` | allowed | Read again for every match. |
| `gamedata/sound.tdf` | read once | Loaded once when the game starts. |
| Any other file under `gamedata/` | not confirmed | Not yet shown to be read again for every match. A wildcard mask under this folder is refused. |
| `guis/` | menus only | Build pages and screens change only through a mutator's menus. |
| `maps/` | maps only | Maps change only through a mutator's maps hook. |
| `sounds/` | read once | Loaded once when the game starts. |
| `music/` | read once | Loaded once when the game starts. |
| `palettes/` | read once | Loaded once when the game starts. |
| `fonts/` | read once | Loaded once when the game starts. |
| `bitmaps/` | read once | Loaded once when the game starts. |
| `camps/` | read once | Loaded once when the game starts. |
| `textures/` | not confirmed | Not yet shown to be read again for every match. |
| `anims/<unit><digits>.gaf` | allowed | One level under `anims/`, when the unit name is a unit this game already has, or a unit this mutator or an earlier one still adds. Read again for every match. |
| `anims/<unit>_gadget.gaf` | allowed | The same, for a unit's download-button art. The suffix matches without regard to case. |
| Any other file under `anims/` | interface art | Interface art, which changes with the screens. A wildcard mask under this folder is refused. |
| Anything else, including a file at the top | unknown folder | This folder is not one a mutator may change. |

The rule names are `allowed`, `invalid-path`, `menus-only`, `maps-only`,
`read-once`, `not-confirmed`, `interface-art` and `unknown-folder`.

A path whose rule is anything other than `allowed` fails that mutator
when the layer is built, and that failure refuses the start. The failure
text is `<id>: '<path>': <path>: <sentence>`. The allow-list's own
message is the part that starts with the path. The same shape names a
mask, an add folder or an archive when that is what was refused.

A link, a file that is not a regular file, a file name that holds `:` or
`\`, and an add-folder string that is empty or holds a NUL, fail that
mutator as well. An `sm` path is matched to the package listing letter
for letter, so the case the folder stores is the case the plan writes.

`oa-tool mutator import` does not fail the import for a protected path
inside a TA: Mutation archive. An archive with no such entry is copied
unchanged. An entry on a protected path is left out, and the warning is
`left out <archive>: <path> (<rule>)`. The archive is written again from
the entries that remain. An archive with nothing left is not written.

### 5.3 Tracing lookups

`--trace-lookups FILE` writes every game file and listing the run looks
up, one a line. For a path the mutator layer serves or hides it writes a
second line, `path`, a tab, then `mutator <id>` or
`hidden by mutator <id>`. The option is
[the command line](../command-line.md).

## 6. Rules

A rule selects records and writes their fields. The same records are
written two ways: a Lua block, and a `.sm` file. The two spellings compare
equal when they are the same rules. Mutators apply one after another, in
list order. Each one is applied to the data the mutators above it left.
Within one mutator, its rules apply in file order. How one change meets
the next, inside a mutator and across the list, is
[6.8 The combination algebra](#68-the-combination-algebra).

A plan's rules list is a sequence of finished blocks and `sm` paths.
Consecutive blocks become one source. Each `sm` path is its own source.
The bytes come from the package. A path that is absolute, has a drive, or
has a `..` segment is outside the package and is not read. A file that
cannot be read, or that the rule parser reports an error in, refuses the
plan. A warning is kept with the source. A path is read and parsed once,
however many times the list names it.

The blocks of every source together stay within 4,096 blocks, and the plan
holds at most 256 sources. A statement, a name, a field and a quoted text
stay within the rule file's own limits, below. Passing a limit refuses
the plan.

`where` and `:change` copy the table they are given. A loop that reuses
one table builds a different block each time, and changing the table later
changes no block already built. Field names in one block are stored in the
order of those names after ASCII lower-casing, with the raw bytes breaking
a tie. `MaxDamage` and `maxdamage` in one table are two changes, and they
are applied in that order. Two keys that trim to the same text are
refused. An unfinished block refuses the plan.

### 6.1 The Lua forms

The engine publishes `where`, `all`, `named`, `on`, `set`, `scale`,
`lock`, `remove`, `append`, `sm` and `empty` for the changes, and `range`,
`is`, `has`, `absent`, `not_`, `glob` and `pattern` for the needs. `not`
is a Lua keyword, so the need is `not_`. `not_` takes only a `range`, an
`is` or a `has`. `not_(absent())` and `not_(not_(...))` are refused with
`not_ of a not_`, because `absent()` is already a negated need. `glob` and
`pattern` match maps only. Using either in any other need is refused with
`glob() and pattern() match maps only`.

`on` takes a folder and an extension together. It stores both in lower
case, turns `\` into `/`, trims spaces, and strips one leading `.` from
the extension. `on` by itself selects every record of that target. The
default target, when a block never calls `on`, is the units folder with
the `fbi` extension. `:change` finishes a block. A second `:change` is an
error. A later `where`, `all` or `named` on a block that already has a
selector is refused. Calling `on` on a block that already has a selector
keeps the line of the call that started the block and replaces the folder
and the extension.

| Lua | Rule file |
| --- | --- |
| `where { field = n }` | `field=n,n` in `[Needs]` |
| `where { field = true }` | `field=1,1` |
| `where { field = false }` | `field=!1,1` |
| `where { field = "text" }` | `field="text"` |
| `where { field = range(lo, hi) }` | `field=lo,hi` |
| `where { field = is("text") }` | `field="text"` |
| `where { field = has("text") }` | `field="text"*` |
| `where { field = has("") }` | `field=""*` |
| `where { field = absent() }` | `field=!""*` |
| `where { field = not_(...) }` | the same need with a leading `!` |
| `where { ... }:change { ... }` | `[*Group*]` with `[Needs]` and `[Changes]` |
| `all():change { ... }` | `[*All*]` |
| `named("NAME"):change { ... }` | `[NAME]` |
| `on(folder, extension):change { ... }` | `folder=` and `extension=`, then `[*All*]` |
| `on(folder, extension):all():change { ... }` | the same folder and extension, then `[*All*]` |
| `on(folder, extension):named("NAME"):change { ... }` | the same folder and extension, then `[NAME]` |
| `on(folder, extension):where { ... }:change { ... }` | the same folder and extension, then `[*Group*]` |
| `change { field = n }` or `set(n)` | `field=n` |
| `change { field = "text" }` or `set("text")` | `field="text"` |
| `change { field = "" }` or `set("")` or `remove()` | `field=""` |
| `change { field = empty }` | `field=;` |
| `change { field = true }` | `field=1` |
| `change { field = false }` | `field=0` |
| `change { field = scale(k) }` | `field=*k` |
| `change { field = scale(k, lo, hi) }` | `field=*k(lo,hi)` |
| `change { field = lock(n) }` | `field=^n` |
| `change { field = append("text") }` | `field="text"+` |
| `change { ["<AllDepth2>"] = scale(k) }` | `<AllDepth2>=*k`, in any ASCII case |
| `sm "path"` | that rule file, as its own source |

`empty` assigns empty text, the record a rule file writes with `field=;`.
`""`, `set("")` and `remove()` remove the field.

### 6.2 A field the record leaves out

When the field is written, a range holds when the text's number is inside
the bounds, inclusive. Text that is not a number reads as zero, the way
the game reads it. An equality holds when the text matches without ASCII
case. A substring holds when the text contains the needle without ASCII
case. An empty needle is contained.

When the field is not written, `has("")` and `absent()` do not read a
default. `has("")` holds exactly when the key is in the file.
`absent()` holds exactly when the key is not. In the rule file those two
are `field=""*` and `field=!""*`. Every other need is read against the
value the loader uses for that key. For a unit, a weapon, or a movement
class that value is the field's default. A key the schema does not hold,
and any other record, reads 0 for a range and empty text for an equality
or a substring. The defaults that are not zero or empty are listed in
[the field schema](../../src/data/field-schema/README.md#defaults-that-are-not-zero-or-empty).

A negated need holds exactly when the positive one does not.

### 6.3 The .sm file

A rule file is text of at most 1 MiB. A longer text is refused before it
is read. Keywords, section names and `<AllDepth2>` match without ASCII
case. A `//` outside quotes runs to the end of the line. `/*` is not a
comment, and it is an error. A line ends at LF, CRLF or a lone CR.
Whitespace is free around `=`, around braces and at the start of a line.
A number is `-` optionally, then digits with an optional fraction, or a
point and digits. An exponent, or a leading `+`, is not a number. The
number is then read by the same reader the game uses for the value.

```ebnf
file        = { directive | block | stray_brace } ;
directive   = ( "folder" | "extension" ) "=" text [ ";" ] ;
block       = "[" header "]" "{" body "}" ;
header      = "*all*" | "*group*" | name ;
body        = { change }                         (* *all* and a named record *)
          | { needs | changes } ;                (* a group, either order *)
needs       = "[Needs]" "{" { need } "}" ;
changes     = "[Changes]" "{" { change } "}" ;
need        = field "=" [ "!" ] ( number "," number
                                | string [ "*" ]
                                | '""' ) [ ";" ] ;
change      = ( field | "<AllDepth2>" ) "=" value [ ";" ] ;
value       = number
          | "*" number [ "(" number "," number ")" ]
          | "^" number
          | string
          | string "+"
          | bare ;
bare        = (* empty, or words with their inner spaces *) ;
string      = '"' { byte - '"' } '"' ;
stray_brace = "}" ;                                  (* lenient, outside a block *)
```

`folder` and `extension` apply to every block after them, and each may be
set more than once. `\` in either becomes `/`, and both are stored in
lower case. The extension is a filename suffix with no leading dot, so
`sidedata.tdf` names one file. Until a file sets them, the target is
`units` and `fbi`. A directive inside a block is an error.

`<AllDepth2>` writes every key of every block directly inside the record,
each key its own field. A weapon's `[DAMAGE]` entries are reached that
way. Keys of the record itself are left alone. A record that has no such
keys still matched. Two spellings of one key inside a block, differing
only in ASCII case, are one field: the reader keeps the newest, and the
change is applied once.

A group's needs are all required. There is no or between them. A group
with no needs matches every record of its target. A statement outside
`[Needs]` and `[Changes]` in a group is a `stray_text` error. The
statement is dropped, and the error refuses the plan and
`oa-tool mutator check`.

### 6.4 Lenient forms

Only a form that produces one of the twelve warning codes in
[6.5 Limits and diagnostics](#65-limits-and-diagnostics) is warned about.
The rest of the file is still read. Some layouts produce no diagnostic
at all: a `{` on a later line than its header, several statements on one
line, and a header after `}` on the same line.

- A statement or directive may end at the end of the line, or at the `}`
  that closes it, with no `;`. That is `missing_semicolon`.
- A header whose body holds `[Needs]` or `[Changes]` is a group even when
  the header is not `*Group*`.
- A number written in quotes is a text change. The bytes inside the quotes
  are kept. An empty quoted value with no `+` removes the field.
- Spaces inside an unquoted value, such as `*7 (2,15)`, are ignored.
- A need whose low bound is above its high bound is kept. It matches
  nothing, or everything when it is negated.
- A group with no `[Needs]` is kept. It changes every record of its target.
- A second `[Needs]` or `[Changes]` in one group is merged.
- A leading byte-order mark is skipped.
- A change whose value is a bare word or words is a text assignment. The
  words are trimmed at both ends and their inner spaces stay. A value that
  starts like a number, a scale, a clamp, a meta value or a quote keeps
  the stricter reading, including a malformed one.
- A change with nothing after `=` assigns empty text. That is the Lua
  value `empty`. It is distinct from `""`, which removes the field.
- A need written `""` or `!""` is equality with empty text, negated when
  it is written `!""`. `""*` and `!""*` stay the presence tests
  `has("")` and `absent()`.
- A `}` outside any block is ignored, including one after a middle block
  and one after the last block. A `}` that ends a block still ends it.

A bad statement is dropped, and reading continues. A block whose header or
braces are broken is dropped. A `[` inside `[Needs]` or `[Changes]` is an
unknown section: a section cannot hold a section. That section's braces
are skipped.

### 6.5 Limits and diagnostics

| Limit | Value |
| --- | --- |
| The file | 1 MiB |
| Blocks kept | 4,096 |
| Statements in one block | 4,096 |
| One name, field, quoted text or bare text | 1,024 bytes |
| Diagnostics stored | 1,000. The last one stored is the notice that collection stopped. |

The twelve lenient codes are warnings. A warning keeps the statement or
the block. Every other code is an error. An error drops the statement, or
the block when its braces or header are broken, and it refuses the plan
and `oa-tool mutator check`. `too_many_diagnostics` is an error, so a file
with more than 1,000 findings is refused even when each earlier finding
is a warning.

| Code | Severity | Meaning |
| --- | --- | --- |
| `missing_semicolon` | warning | a kept statement was not closed with `;` |
| `group_without_asterisks` | warning | a header without `*Group*` holds a group body |
| `quoted_number` | warning | a change's quoted text is a number |
| `spaces_in_value` | warning | spaces inside an unquoted value were ignored |
| `empty_range` | warning | a need's low bound is above its high bound |
| `group_without_needs` | warning | a group has no needs, so it matches every record |
| `repeated_section` | warning | a second needs or changes section was merged |
| `byte_order_mark` | warning | a leading byte-order mark was skipped |
| `unquoted_text` | warning | bare words read as a text assignment |
| `empty_value` | warning | a change with nothing after `=`, read as empty text |
| `empty_text_need` | warning | `""` or `!""` in a need, equality with empty text |
| `stray_closing_brace` | warning | a `}` outside any block, ignored |
| `unknown_section` | error | a section other than needs or changes, or a section inside a section |
| `unclosed_block` | error | a header or a brace was never closed |
| `unexpected_brace` | error | a brace where a block did not call for one |
| `directive_in_block` | error | `folder` or `extension` inside a block |
| `bad_number` | error | a number the grammar does not allow. A number token of 511 characters or more is this code. |
| `bad_need` | error | a need that matches no need form |
| `bad_change` | error | a change that matches no change form |
| `need_form_in_changes` | error | a need written where a change belongs |
| `change_form_in_needs` | error | a change written where a need belongs |
| `clamp_low_above_high` | error | a scale's low clamp is above its high clamp |
| `unterminated_string` | error | a quote was never closed |
| `block_comment` | error | `/*`, which is not part of the language |
| `stray_text` | error | text that is not a directive, a block or a statement, or a statement outside `[Needs]` and `[Changes]` in a group |
| `too_large` | error | the text is longer than 1 MiB |
| `too_many_blocks` | error | more than 4,096 blocks |
| `too_many_statements` | error | more than 4,096 statements in one block |
| `text_too_long` | error | a name, field, quoted text or bare text is longer than 1,024 bytes |
| `too_many_diagnostics` | error | the list stopped at 1,000 diagnostics |

The tool reads a rule file up to 4 MiB and then this reader refuses a text
longer than 1 MiB, so either bound refuses the file.

### 6.6 How a number is written

A number is written only with the TDF number spelling, and only when it is
finite and different from the field's current number. The current text
counts as that number when, after spaces or tabs, it has an optional sign
and then a digit, or a point and at least one digit (`.8`, `-.5`,
`+.25`). A lone `.` or `-.` is not a number. A factor does not treat a
word as zero.

### 6.7 Which records a rule reaches

A target is a folder and a filename suffix. The suffix matches the end of
the file name, without ASCII case, so `sidedata.tdf` reaches that file and
`tdf` reaches every file of that ending. The folder is not recursive:
`features/corpses` reaches `features/corpses/a.tdf` and not a file in a
folder below it.

A folder whose first segment is one of the seven base directory names
(`units`, `weapons`, `gamedata`, `ai`, `guis`, `unitpics`, `download`)
means the active layout's folder of that kind. The suffix `fbi` means the
layout's unit extension. Any other folder and suffix are kept as the rule
wrote them. Until a block sets a target, the target is the units folder
and the unit extension.

Unit files hold one record, the first `[UNITINFO]` section. A named block
matches `UnitName` without ASCII case, and the record is reported under
that name when the key is present, otherwise under the section name. Every
other folder treats each top-level section as a record, and a named block
matches the section name without ASCII case.

The layout's `gamedata/moveinfo.tdf` is read as movement classes. Units
and weapons win over that path when the same file could be read both ways.
A weapon file's `[DAMAGE]` block is reached by `<AllDepth2>`, not as a
record of its own.

### 6.8 The combination algebra

There is no separate combining step. Rules are applied in sequence, and
each change is written before the next change is read.

One mutator's sources run in plan order. Each source's blocks run in
file order. Inside a Lua block the needs and the changes run in the
order of their field names after ASCII lower-casing, with the raw bytes
breaking a tie, not in the order the table was written. A block's
needs read the records as the rules before that block left them. A
later block of the same mutator sees an earlier block's write. The next
mutator is another step on those documents, after this mutator's hooks.

| Change | What it does to the current value |
| --- | --- |
| A factor | Multiplies the current number. A clamp bounds only that factor. It does not bound a later factor. |
| An assignment | Replaces the current value. |
| A text change or a removal | Acts on the current text. |
| An append | Extends the current text, or the empty text when the field is absent. |
| A lock | Sets the value and marks the field for the rest of the run. |

A later mutator's change to a locked field is not written. The note
names the mutator that locked the field and the mutator that tried to
change it. A later change of the mutator that locked the field still
applies, and the lock stays for the mutators after it. A lock of a
number the field already holds writes nothing and still marks the field.

A later assignment, text change, removal or lock that replaces a value
an earlier mutator wrote is noted, and the note names both mutators. A
factor or an append builds on the current value and is not that note.
The first write, over the documents as they were loaded, is not a
replacement. A removal of a field that is already absent does nothing
and is not noted.

A factor on a field that is absent does nothing, and the step notes it.
A factor on text that is not a number is noted and does not write. A
result that is not finite is not written, and a clamp is not allowed to
pull that result back into range.

A key the record does not hold is written with the spelling of the
change that creates it. A later change of that field keeps the spelling
the reader already has. Two spellings of one key that differ only in
ASCII case are one field. The order of the mutators matters, and so
does the order of the changes.

## 7. Hooks and collections

After a mutator's own files have been read as records, and after its
rules, its hooks run, and only the ones the plan returned. The order is
`units`, `weapons`, `features`, `sides`, `tables`, then `menus`. The
maps entry runs later, after every mutator's records, rules and hooks,
and after weapons have been given ids, and only when the match has
chosen a map. One mutator's maps entry is a transaction of its own.

A hook may write any collection whose layer the manifest declares,
whichever hook it is. Reading any collection on `ctx` is allowed. With
no mutator begun, every write and every removal is refused.

One mutator may journal at most 65,536 field writes and 65,536 notes.
The write tally is one number for that mutator: the units, weapons,
features, sides, tables and menus hooks share it with the maps phase.
Each field a clone's table sets counts. The copy itself does not.
Passing the write limit fails that mutator even when the script catches
the error. The messages are `this mutator has changed too many fields`
and `this mutator has made too many notes`.

### 7.1 Units, weapons and features

A collection is indexed by record name, ignoring ASCII case. A missing
name is nil. `#` is how many records it holds. `pairs` and `each` walk
in one order: the name compared byte by byte after ASCII lower-casing,
then the document path the same way, then the ordinal of that name
within its document. Nothing in the walk follows Lua's table order.

`where { field = form, ... }` returns the records that meet every need,
judged on the records as they stand, so a hook sees earlier writes. The
needs are the same forms as [6.1 The Lua forms](#61-the-lua-forms). A
field the record leaves out is read as the value the loader uses. The
defaults that are not zero or empty are listed in
[the field schema](../../src/data/field-schema/README.md#defaults-that-are-not-zero-or-empty).
`absent()` asks whether the key is written, not what the default would
be. A set has `each`, `set`, `scale`, `remove`, a length and `pairs`.
A set keeps the records `where` selected. A later edit does not add a
record to it, and it still yields a member removed after `where`. Using
that member raises.

A record has `remove`, `get`, `set`, `keys` and `fixed`. Any other name
is a field. A field whose name is one of those five is reached only with
`get` and `set`. A weapon's `damage` is its `[DAMAGE]` block, created on
the first write into it. `get("damage")` reads a field of that name and
does not open the block.

A read of a known unit or weapon field returns the schema's integer,
number, boolean or string, and nil when the key is absent. An integer is
the whole number written, not cast to the width the game stores it in, as
rules and `scale` read it. A number past that width when the match loads
is held at the end of its range, once, after every mutator. A feature
field the game reads in fewer bits is held the same way, with the same
note, at the range the game reads: `metal` and `energy` 0 to 65535 (`metal`
0 to 255 on an indestructible feature that cannot be reclaimed, whose
metal is painted on the map as a byte), `damage` 0 to 65535, `height`,
`spreadchance`, `reproduce` and `reproducearea` 0 to 255, `sparktime` 0 to
8.57 seconds, and `footprintx` and `footprintz` at most 256. An unknown
field, and every feature field, comes back as a string. A write of a
known unit or weapon field is checked first, against the widths and the
limits in
[the field schema](../../src/data/field-schema/README.md). A refusal is
a Lua error that names the collection, the record, the field and the
reason. A warning is stored and noted, and the value is kept. A number
is stored only through the TDF number spelling. A value that is not
finite is an error. A boolean is stored as `1` or `0`. A string is
stored without the spaces, tabs and line breaks at either end. Nil
removes the key. Feature fields skip the schema
and are stored as text, numbers included. A field name loses spaces,
tabs and line breaks at either end. A write that would store the text
already there changes nothing.

`record:fixed(key)` reads that field as a fixed value, as
[10.9 The fixed library](#109-the-fixed-library) defines it. Text that
is present is parsed. A key the record leaves out uses the loader's
default, so an absent `bankscale` is 1 and an absent `moverate1` is
twice `maxvelocity`. A key the schema does not hold, and a key a
feature leaves out, is nil. Storing a fixed value, by assignment, by
`:set` or by a set's `:set { }`, is checked as the number `count / 65536`
and written as the text `f:text()` gives.

`scale` multiplies each field that is present and numeric. An absent
field is left absent. Text that is not a number is left as it is and
noted. `set` and `scale` visit the table's fields in the sandbox's
`pairs` order, and the records of one field in collection order.

`remove` on a unit marks that unit's document removed and drops every
download section whose `UNITNAME` or `UNITMENU` names the unit, ignoring
ASCII case. A download file left with no section is marked removed.
At the end of the mutator's turn, the engine greys each unit removed in
that turn on every builder's authored pages under `guis/`, as
`menus:grey` does, whether or not the manifest declares `menus`.
`remove` on a weapon or a feature
drops that section, and marks the document removed when it was the last
section. A later read or write of a removed handle raises
`<name> was removed`. The collection no longer returns a removed record
by name.

`units:clone(source, name, fields)` copies the source unit into
`units/<name>.<extension>` at once and sets `UnitName` to the new name.
`Objectname` is the source's model, or the source's own name when the
source leaves the model out, so the clone shares that model. The layer
also serves `scripts/<name>.COB`, `unitpics/<name>.PCX`, the build pages
`guis/<name><n>.GUI` from page 0 and then 1, 2, … until one is missing,
and `anims/<name><n>.GAF` for each page it served, each with the
source's bytes. A field in `fields` is a typed write, in sorted key
order, and a bad field adds nothing. A path the clone would serve that
already exists refuses the clone, naming the path, except while clash
mode is active: `replace` puts the clone in place of the existing
record, and `add` stores it under a private name. While clash mode is
not active, a clone onto a name that exists is refused with
`the new name already exists`. A clone's new name is 1 to 31 letters,
digits, `_` or `-`. `weapons:clone` with no free id from 1 to 255 fails
at once, before the integrity pass, with
`no free weapon id: 1 to 255 are all used`.

`weapons:clone(source, name, fields)` copies the source section,
including `[DAMAGE]`, into `Weapons/<name>.tdf`, or
`Weapons/<name>-<k>.tdf` when that path is taken, `k` from 2. It writes
`ID` to the lowest free id from 1 to 255. That id is a request, not the
id the file is published with. A field named `ID` in `fields` is
refused. When several sections share a name, the copy is the first in
collection order, which may not be the lowest slot the game fires.

After the last hook, every weapon a mutator brought, by a record file or
by `weapons:clone`, is given an id. The game's weapons and the mod's
weapons keep theirs. A rule or a hook that writes `id` on one of those
weapons does not change it, and the note names the mutator. A write of
`id` on a weapon that mutator brought is a request. A section's `ID=`
is a request when it is a number from 1 to 255. Numbering takes each
mutator in list order. That mutator's requests are kept when the ids
are still free, and its other weapons then take free ids from 1 to 255,
by name. A later mutator's request for an id an earlier weapon already
holds takes another free id. Slot 0 is never given out. When one weapon
has no free id, nothing of that pass is written, the mutator fails in
the integrity phase, and the message names the mutator and the weapon.
The run is repeated without that mutator.

### 7.2 Sides

`ctx.sides` is `gamedata/sidedata.tdf`, or the language variant of that
file when the variant is the one the game reads for side data. Build
lists are read from the plain file, because that is the file the
build-list loader opens. When the two paths differ, each document is
served at the path its own reader opens.

Sides are `SIDE0`, `SIDE1` and so on, until the first number that is
absent, and at most five. `#sides` is that count. `sides[n]` is the
side at that place, counting from 1, and nil past the end.
`sides.<name>` finds a side by its `name`, ignoring ASCII case.
`sides:each(function(side) ... end)` walks them in that order. A
mutator cannot add a side and cannot remove one. Both raise.

A side's fields are text, stored trimmed. Nil is refused. The nested
blocks, such as a logo, are not fields. These names are checked:

| Field | Rule |
| --- | --- |
| `name` | 1 to 29 characters, and no other side already has it. |
| `nameprefix` | 1 to 3 letters or digits, no slash. Only `guis/<prefix>GEN.GUI`, `guis/<prefix>MAIN2.GUI` and `guis/<prefix>DL.GUI` are checked, and only a page the current prefix already has. A missing one is `the prefix has no guis/ZZGEN.GUI page`, with that prefix's own letters. |
| `commander` | 1 to 31 characters, and a unit the units collection holds. |
| `intgaf` | 1 to 29 characters, no slash, and `anims/<name>.GAF` has to be in the store. |
| `font` | 1 to 255 characters, no slash, and `fonts/<name>.FNT` has to be in the store. |
| Any other field | Text of at most 65,536 bytes. |

A side value that is not text is refused. Writing needs the `sides`
layer.

`sides:canbuild(unit)` returns a new array of the unit names on that
builder's list, in order, or an empty array when the unit is not a
builder. `sides:add_canbuild(builder, unit)` appends one name, and a
name already on the list adds nothing. `sides:remove_canbuild(builder, unit)`
drops that name from one builder. `sides:remove_canbuild(unit)` drops it
from every list. `sides:set_canbuild(builder, names)` replaces the list
with that array. Every name has to be a unit. A unit that is not a
builder is refused with `the unit is not a builder`. The list is stored
as `canbuild1`,
`canbuild2` and so on, with no gaps. A list longer than 1,024 names is
refused. A list longer than 30 is written, and the note is
`the AI keeps only the profile's limits.build-list-entries count (30 by default)`.

### 7.3 Tables

`ctx.tables` opens only `moveinfo`, `los` and `meteor`. `tables.moveinfo`
and `tables["gamedata/moveinfo.tdf"]` are the same document when that
file is loaded, and the same shape reaches `los` and `meteor`. A file
that is not there is nil. `sidedata` is the sides collection, not a
table, and naming it says `use ctx.sides`. `tables.sound` is refused
with `gamedata/sound.tdf: this is loaded once when the game starts`.
Another file directly in `gamedata` is refused with
`gamedata/<file>: not yet shown to be read again for every match`. A
name that is not a file directly in `gamedata` is refused with
`<name> is not a file directly in gamedata`.

`document:sections()` returns the section names in file order, duplicates
included. `document[name]` is the first section of that name, ignoring
ASCII case. A section's fields are text. `section:keys()` lists the
spellings that are present. `document:add_section(name)` adds a section.
`document:remove_section(name)` removes the first section of that name.
A value is text of at most 65,536 bytes. Nil is refused with
`nil does not remove a table key`. A number, and any other value that
is not text, is refused with `a table value is text`. A value that
holds `;`, a brace, a bracket or a line break is refused with
`a value cannot hold ';', braces, brackets or a line break`. A value
that holds `//` or `/*` is refused with `a value cannot hold a comment mark`.
Writing needs the `tables` layer.

### 7.4 Menus

`menus` is the build menus. It is on `ctx` in every hook, and a `menus`
function is called after the other hooks. Writes need the `menus` layer.
`builds` does not. Every name is matched without ASCII case and has to
be a unit the units collection holds when the call runs.

| Call | What it does |
| --- | --- |
| `menus:add(builder, unit)` | Adds a download entry in the first free slot. Returns `{ page = p, slot = s }`. A unit the builder already offers is not added twice: the call returns the place it already has. |
| `menus:add(builder, unit, { page = p, slot = s })` | Asks for that slot. An occupied slot is not overwritten. The entry moves to the next free slot, and the report says where it moved and who held the slot. |
| `menus:add(builder, unit, { page = "new" })` | Slot 1 of the page after the builder's last page. |
| `menus:move(builder, unit, { page = p, slot = s })` | Moves that builder's download entry. It does not move a button of an authored page. |
| `menus:remove(unit)` | Drops the download entries that name the unit, on every builder, and greys its buttons. |
| `menus:remove(builder, unit)` | The same, for one builder. A unit the builder does not offer is not an error. |
| `menus:grey(builder, unit)` | Sets `grayedout=1` on that unit's buttons and clears bit `0x04` of `commonattribs`. It does not change a download entry. `menus:grey("ARMCOM","ARMMEX")` writes `guis/armcom1.gui [gadget8] grayedout: (absent) -> 1`. |
| `menus:hide(builder, unit)` | Sets `active=0` on that unit's buttons. |
| `menus:builds(builder)` | A new array of unit names, each once: the authored buttons that are neither greyed nor hidden, in page and button order, then the download entries. A unit the units collection no longer holds is left out. |

A page is 1 to 7 and a slot is 1 to 6. The page a script calls `p` is
the file `guis/<unit><p>.GUI` and the download field `MENU=p+1`.
`BUTTON` is the slot minus one. Every slot of an authored page is taken.
A builder a hook cloned has the pages the clone serves, so their slots
are taken too. A slot is decided when the request is made, in list
order. A later mutator cannot move an earlier mutator's entry, and an
earlier request is not moved into a slot a later mutator empties.

The loader reads only the first five sections of a download file. A new
file for a unit that is not downloadable starts with a `PLACEHOLDER`
section whose `UNITMENU` is empty, so it marks nobody. A file left with
no entry is hidden. A button with no `anims/<unit>_gadget.gaf` is still
added, with a note. An add that the AI's list cannot hold is still
added, with a note that the list is full.

An authored page stays a file a mutator may not replace. Grey and hide
edit keys on the authored page itself and serve that changed page, or
edit the copy a clone serves. They never move a button. A unit with no
button is refused. A download entry says
`<unit> is a download entry on <builder>; use menus:remove`. Anything
else says `<builder> does not build <unit>`.

### 7.5 Maps

The maps entry runs only once a match has chosen a map, and only for
that match. The setup screens keep the installed scenario. The rewritten
text is served from the mutator layer at `maps/<key>.ota` when a mutator
changed the bytes. The battle room's map check reads the installed file
before the match starts, so that check is unchanged. Every machine that
runs the same loadout writes the same bytes.

`map.name` is the scenario's `missionname`, or the map key when that key
is absent. `map.file` is the key. `map.planet` is the header's planet.
`map.pack` is the id after the last `@`, or nil for a base map.
`map.header` reads every GlobalHeader key, ignoring case. These keys can
be written:

| Key | What it accepts |
| --- | --- |
| `minwindspeed`, `maxwindspeed`, `gravity`, `waterdamage` | Whole numbers from 0 to 32,767. |
| `lavaworld`, `waterdoesdamage`, `nosealeveltrigger` | 0 or 1. |
| `tidalstrength`, `killmul`, `timemul` | Finite numbers from 0 to 1,000. |

Any other header key is refused. `map.schemas` is the `[Schema N]`
blocks from 0, with `:each`, `:where`, `:set` and `:scale`. A schema's
`index` is N. `schema.index`, `type` and `players` are read-only. Writing
`index` is refused with `schema.index cannot change`. `aiprofile` is 1
to 31 letters, digits, `_` or `-`, and `ai/<name>.txt` has to already be
in the store, including a file this mutator added. `humanmetal`,
`humanenergy`, `computermetal` and `computerenergy` are whole numbers
from 0 to 1,000,000. `surfacemetal` is a whole number from 0 to 255.
`:scale` multiplies a stored number. It cannot scale `aiprofile`.

`schema.units` and `schema.features` are the placed entries, in order. A
unit's `unitname` may name a unit that is in `units` as the mutators
above left it. Its `xpos` and `zpos` are whole numbers from 0 to 32,767,
and its `player` is a whole number from 1 to 255. A feature's
`featurename` may name a feature that is in `features` the same way.
`:remove()` drops that one entry. Nothing in the view adds a placed
unit, a placed feature or a schema, and start positions are never shown
or written.

In the list form, `where` reads `name`, `file`, `planet`, `pack` and
header keys. A header key in `change` writes the header. A schema key
writes every schema. A key that is neither is refused when the plan is
read, and so is a read-only key. `glob` matches `*` and `?` only, ASCII
case ignored, at most 256 bytes, with `/` ordinary. `pattern` is a Lua
pattern, matched with the sandbox's `string.find`, so its case matters.
Both are refused in every need outside this list form, with
`glob() and pattern() match maps only`. The list is applied in order.

A rule block whose folder is `maps` and whose extension is `ota` is
taken out of the rules phase and run at the start of the maps phase,
before the first maps hook, and only on the chosen map. Any other
extension in that folder refuses the plan. The header limits are the
ones a maps hook uses. A whole-number header key may be left fractional
when the result is finite and from 0 to 32,767. A key that is not on
`[GlobalHeader]` is refused. A header `SurfaceMetal` is not written
into `[GlobalHeader]`. A kept number is copied onto every schema, as a
whole number from 0 to 255, and the note says so. A header `MohoMetal`
changes nothing and is noted with
`nothing in OA reads MohoMetal; skipped`. With no chosen map the rules
change nothing and leave one note per mutator,
`applies to the chosen map only`.

A refused write is an error the script can catch with `pcall`, and it
names the field or the rule it broke. Map writes count on the same
tally as that mutator's other hooks.

## 8. Hacks

A mutator's `hacks` table sets parameters of registry entries the
registry marks `mutator: true`. The mark is refused on an entry that is
scoped to install, on an entry that touches the interface, and on an
entry whose kind is not a hack or a limit. The entries that carry it
today are `limits.units-per-player` and `console.game-speed-range`.

Only a parameter the registry marks adjustable per match can be set.
Today those are `limits.units-per-player.default`,
`console.game-speed-range.min` and `console.game-speed-range.max`. The
unit limit accepts an integer from 1 to 6,553. The value that plays is
clamped to the profile's `min` and `max`, 20 and 1,500 unless the
profile sets others. Each speed bound is an integer from 0 to 20, and
`min` stays at or below `max`. A scalar, such as
`["limits.units-per-player"] = 500`, sets that entry's single match
parameter. Every other parameter, and every other entry, is refused.
Only a problem inside the `hacks` table fails that mutator alone. The
mutators that require it are dropped with it, and the match starts
without them. When `hacks` is not a table, or the `hacks` table has a
metatable, the fault is an invalid plan and the start is refused.
`true` and `false` are not a way to turn an entry on or off: a mutator
sets a parameter.

```lua
hacks = {
  ["limits.units-per-player"] = { default = 500 },
  ["console.game-speed-range"] = { min = 0, max = 20 },
}
```

The value is kept in the registry's normal form. The manifest must list
the `hacks` layer. A table that is not plain is an invalid plan, and
that refuses the start. A refusal of a value inside a plain table is
the hack refusal above.

Developer Mode is the baseline under mutators. A mutator sets its
parameters over that baseline, for the one game. When Developer Mode
turns the entry off, the report says
`Developer Mode turns <entry> off; <mutator> turns it on for this game`.
When the mod leaves the entry off, it says
`the mod leaves <entry> off; <mutator> turns it on for this game`.
When Developer Mode also sets the entry, it says
`Developer Mode also sets <entry>`. Two mutators that set the same
parameter both stay in the report, and the later value is the one that
plays:
`<earlier> and <later> both set <path>; <later>'s value plays`.
The same facts are stated for profiles in
[the OAMOD standard](../mods/oamod-standard.md#55-the-hosts-choices).

## 9. Stacking

The player lists mutators in an order. That order is the order they
apply. Prerequisites named in `requires.mutators` are switched on with
the mutator and apply before it, in the order that list writes them.
The list is checked before anything runs. More than 32 mutators, an id
listed twice, a requirement that no mutator above it meets, or two
mutators that conflict, refuses the whole list, and nothing runs. Each
manifest is then checked against the mod and the engine version. A
mutator that does not fit is incompatible. That refuses the start, the
same way any other failure does, unless the only failure is a refused
hack.

A pass plans every mutator, builds the files layer, then takes each
kept mutator in list order: its weapon files, its unit files, its
download files, its rules, then its hooks. The maps phase follows, in
the same list order. A pass is a transaction. A failure that depends
only on the mutator itself is kept. A later failure can depend on the
mutators above it, so the pass keeps only the failure nearest the top
of the list and stops at the first hook that fails. No later mutator
runs on what a failed mutator left. The mutator that failed, and every
mutator that requires it, is dropped, and the next pass starts again
from the store. A mutator dropped for what it requires is excluded.
Each pass drops at least one mutator. A mutator that failed leaves no
file, rule, journal entry or note.

The journal keeps every change of the last pass: each rule write, each
hook write and removal, each file a mutator added, each record a file
added or replaced, and each file of the mod a mask hides. It is sorted
by the first mutator, then by phase, so one mutator's rules come before
its hooks, then by path, record, block and field.

The conflict report holds the stacking notes. Each of these names both
mutators, the earlier first, except where the line says otherwise.

| Situation | What the player is told |
| --- | --- |
| A later mutator wins a path or a field an earlier one wrote | `<later> wins <path> <field> over <earlier>`. The field is left out when the note is a path. |
| A mask hides a file an earlier mutator added | `<later> hides <path> added by <earlier>`. This one is a warning. |
| An add folder is not in the package | `<id> adds a folder that is not in the package: <path>` |
| A later rule replaces an earlier mutator's value | `<later> replaced <earlier>'s value on <path> <field>` |
| A later rule replaces a record an earlier mutator brought | `<later> replaced <earlier>'s <record> on <path>` |
| A record the match already had is replaced | `<later> replaced <record> from <where the record came from> on <path>` |
| Add mode shares a record | `<id> shares <record> with <earlier>`, or `on <path>` when no earlier mutator is named |
| Add mode stores a record under a private name | `<id> renamed <record> to <name>` |
| A field was locked | `<earlier> locked <path> <field>, so <later> left it alone` |
| A factor finds no number | `<id> found no value to scale on <path> <field>` |
| A text change is dropped | `<id> dropped a text change on <path> <field>`. A menu note keeps its own sentence when that sentence says the entry moved from a page, has no picture, does not see a unit, or that a placeholder now holds a unit. |
| A rule block matches nothing | `<id> matched nothing`. A hook that names a missing record does not write this note. |
| A value is kept past the field's limit | `<id> kept <path> <field> past its limit`. This one is a warning. |
| A write is refused | `<id> refused a write on <path> <field>: <detail>` |
| Two mutators set one hack parameter | `<earlier> and <later> both set <parameter>; <later>'s value plays` |
| The documents are not a game the engine can load, and the match can still start | The check's own sentence. This one is a warning. |

One field note is kept for each field name and pair, so a stack that
rewrites the same field does not flood the report. The game prints each
line as `open-annihilation: mutators: <line>` when the match is prepared.
A script stopped by an allowance, the real-time limit or the string cap
is reported as that cause. It is not reported as a script error.

## 10. The sandbox

A mutator script runs as Lua 5.4.9 in one sandbox. The sandbox owns one
state. Two sandboxes share no modules, no globals and no budgets. The
mutator profile gives that state the default instruction allowance and
the default memory cap, and it adds no globals of its own.

A script has no file, no clock, no environment, no process and no
network. Every chunk is loaded as text. A failure comes back as a value
that names the package, the phase, the chunk and the line:
`<package>: <phase>: <chunk>:<line>: <message>`. The chunk and the line
are left out when the chunk is unknown. A failed phase sticks. Later
phases return that same error and run nothing.

`pcall` and `xpcall` cannot keep a spent phase alive. Once the
instruction allowance is spent, the real-time limit is past, or the
sandbox has failed, `xpcall` returns the error and does not run the
handler the script passed. An instruction error, a real-time error or a
memory error is reported even when the script's own `pcall` caught it.

The host's own account of these rules, including the measurement behind
the time limit, is
[the script host](../../src/data/script-host/README.md).

### 10.1 Libraries

The libraries a script can call are the base library (`_G`), `string`,
`table`, `utf8`, `math` and `fixed`
([10.9 The fixed library](#109-the-fixed-library)). `io`, `os`, `debug`, `package` and
`coroutine` are not opened.

These names are nil: `load`, `loadfile`, `dofile`, `collectgarbage`,
`string.dump`, `string.pack`, `string.unpack` and `string.packsize`.
Pack sizes and alignment differ between the machines the engine builds
for, so a script cannot pack bytes itself.

`setmetatable` installs a copy of the metatable. The copy keeps every
field the caller's table had at that moment, including `__index`, so a
methods table that `__index` points at still sees fields added later. A
later write to the caller's own table does not change the copy.
`getmetatable` returns the copy, or the `__metatable` field when the
caller set one. Making the copy counts 64 instruction units, and one
unit per field copied.

A metatable that already holds `__gc` or `__mode` is refused:
`setmetatable: a metatable with __gc is refused` and
`setmetatable: a metatable with __mode is refused`. A protected
metatable is refused with `cannot change a protected metatable`.
`rawset` of `__gc` or `__mode` is refused on every table, with
`rawset: the key __gc is refused` and `rawset: the key __mode is refused`.
A later write of `__mode` on the copy is refused with
`the key __mode is refused`. A script cannot make a weak table. A later
write of `__gc` on the copy is not refused.

`print` writes nothing to a file. The host keeps at most 256 lines and
16,384 bytes of that text. A line is cut only when it is longer than
16,384 bytes, and then to 16,384. A line that does not fit in the bytes
still left is dropped whole, and that drop closes `print`. Every later
line is dropped too. Kept lines may be dropped so the note
`1 line left out` or `<n> lines left out` fits.

### 10.2 Loading code

The entry script and every module are source. An empty file is source.
A file whose first byte is `0x1B` is precompiled and is refused before
the loader sees it, with
`<path> is precompiled Lua; packages hold Lua source only`.
A file longer than 1,048,576 bytes is refused with
`<path> is <n> bytes of source; the most is 1048576`.

Packing and checking load a chunk in a throwaway state and do not run
it. A chunk that would fail or print when run is still accepted there.
Precompiled bytes and a file past the source cap are refused first.

### 10.3 require

`require` takes one name of at most 64 bytes: lower-case letters,
digits and `_`, in parts joined by dots, with no empty part. `util.grid`
reads `lua/util/grid.lua` through the package reader. The path is matched
byte for byte, so a file system that folds case cannot change which file
loads. A module that has already been loaded in this state is returned
again and is not read again.

| What went wrong | Message |
| --- | --- |
| The name is not a module name | `require: "<name>" is not a module name: lower-case letters, digits and _ in parts joined by dots`. A name longer than 96 bytes is shown as its first 96. |
| The package has no such file | `require: the package holds no lua/<parts>.lua` |
| The file is longer than 1,048,576 bytes | `lua/<parts>.lua is <n> bytes of source; the most is 1048576` |
| The file is precompiled | `lua/<parts>.lua is precompiled Lua; packages hold Lua source only` |
| Modules are nested more than 32 deep | `require: modules are nested more than 32 deep` |
| The file could not be read | `require: the package file could not be read` |

A cycle names the chain, for example
`require: lua/a.lua needs lua/b.lua, which needs lua/a.lua`.
The first module is joined with ` needs `, and
each later one with `, which needs `. `require` counts one instruction
unit for every frame it walks, on every call.

### 10.4 Budgets

Each phase starts again from the full instruction allowance and the full
real-time limit. The memory cap is the Lua allocation of the state, and
it is not handed out again for the next phase. Text the host has copied
out, including the lines `print` kept, is not part of that allocation.

| Budget | Value |
| --- | --- |
| Instructions in one phase | 50,000,000 |
| Memory | 64 MiB (67,108,864 bytes) |
| Real time in one phase | 10,000 milliseconds (10 seconds) |
| One string | 65,536 bytes. `string.rep` uses the same cap, separators included. |
| One source file | 1,048,576 bytes |
| A `require` name | 64 bytes |
| `require` nesting | 32 modules loading in one chain |
| Lines `print` keeps | 256 |
| Bytes `print` keeps | 16,384 |
| One error's stack listing | 2,048 bytes |

The instruction count is read every 1,000 instructions. A short phase
can report 0. Passing the allowance stops the phase with
`the script ran past its budget of <n> instructions`.
Passing the memory cap stops
it with `the script used more than its <n> MiB of memory`. For the
default cap that number is 64.

### 10.5 Charged work

Library work that would otherwise run in C is counted against the same
instruction allowance. A unit counts one-for-one against that allowance.
A comparison that costs 2 units is about the time of one or two
instructions of the interpreter. A new string longer than 40 bytes
counts one unit per 32 bytes of its length, rounded up. A string of 40
bytes or fewer is not counted: making one allocates only when no live
copy exists, which depends on when the collector last ran, and the
instruction or call that makes it bounds that work. The header in front
of the bytes is not counted. Freeing a string does not give the units
back. The same script spends the same count on every build and every run.

Pattern matching counts its own steps. A step is one entry into the
matcher, one character a `*`, `+` or `-` repetition tries, or one
character a `%b` pair scans. `string.find`, `string.match`,
`string.gmatch` and `string.gsub` stop with the instruction-budget error
when that count reaches the allowance, and a `pcall` around the call
does not let the phase continue. Within the allowance they return the
same values, of the same types, and raise the same errors, as Lua
5.4.9. A call on a string value, such as `("x"):find`, uses the same
functions.

| Call | Units |
| --- | --- |
| `string.byte` | One per byte returned. |
| `utf8.codepoint` | One per code point returned. |
| `utf8.char` | One per code point. |
| `utf8.codes` | One per 32 bytes the iterator walks, continuation bytes included, on every call, including the call that finds the end. |
| `utf8.len`, `utf8.offset` | One per 32 bytes of the interval they walk. |
| A plain `string.find` | One per 32 bytes each scan and each comparison examines. |
| `require` | One per frame walked, on every call. |
| `table.move`, `table.insert`, `table.remove`, `table.concat`, `table.pack`, `table.unpack` | One per element moved or visited. |
| `table.sort` | Every comparison. Two strings cost one unit per byte of the two lengths, and at least 2. Any other comparison costs 2. A comparison function written in C is counted too. |
| `next` | 4,096 per call. |
| `pairs` | 4,096 for the walk that collects the keys, and each ordering of two keys costs the same as a `table.sort` comparison. |
| `rawequal` of two strings | One per 32 bytes of the two lengths. |
| `tonumber` of a string | One per 32 bytes of that text. |
| `string.format` | One per format byte, 2 per value it converts, and one per byte of each text `%s` or `%q` copies. |
| `math.max`, `math.min`, `fixed.min`, `fixed.max` | One comparison, as `table.sort` counts it, for each value after the first. `fixed.min` and `fixed.max` charge that comparison before the value is converted. |
| `warn` | 2 per argument. |
| `setmetatable` | 64, plus one per field copied. |

A table call whose range is longer than the default memory cap could
hold, one element per stored value and its tag, is refused before it
loops. The message is
`<function>: a range of <n> elements is past the most of <limit>`.

`next`, `rawequal`, `tonumber`, `warn`, `string.byte`, `string.format`,
the charged `table` and `utf8` calls and `utf8.char` are copies of Lua
5.4.9's with the count inside. The script calls them directly, so an
error names the function and starts with the script's chunk and line.
A method call such as `("%d"):format(x)` numbers its arguments as Lua
does.

Comparing two strings with `<` or `==` is one instruction of the
interpreter, and these charges do not see it. Equality of two 65,536-byte
strings takes about 1.3 microseconds on the machine where this was
measured. Ordering two such strings of ordinary bytes takes about 3.3
microseconds. Ordering two strings of that length filled with zero bytes
takes about 480 microseconds. `table.sort` and `pairs` charge that
comparison by length, so a sort of those zero-filled strings stops
inside the default allowance. A bare `<` of two zero-filled strings does
not. The real-time limit is what stops a phase that hides its work in a
step the charges do not see.

### 10.6 Strings

No string may be longer than 65,536 bytes. The cap is applied in the
allocator, because the interpreter has no switch that stops a longer
string. Each state measures the header in front of the bytes once, from
one 200-byte string, before the libraries open, so the cap compares the
string's length. A longer string ends the phase with
`the script made a string of <n> bytes; the most is 65536`,
at the line that made it.

`string.rep` refuses earlier with
`string.rep would make a string of <n> bytes; the most is 65536`.
`table.concat` refuses with
`table.concat would make a string of more than 65536 bytes`.
`string.gsub` refuses with
`string.gsub would make a string of more than 65536 bytes`
when it can see the size coming. A `pcall` that
catches the refusal does not let the phase continue. A value the host
pushes counts as a string too, so text longer than 65,536 bytes fails
the script that receives it. A string literal that long fails the chunk
that contains it. `print` cuts a long value instead of refusing it.

### 10.7 Text, order and numbers

`tostring` of a float is the same characters on every machine. A NaN of
either sign is `nan`, the infinities are `inf` and `-inf`, and every
finite value is `%.14g`, with `.0` added only when that text has no
point and no exponent. Joining a float to a string uses that same text.

A table, a function or any other value without `__tostring` prints as
its kind, a colon and `hidden`, for example `table: hidden`. It does not
print where the value sits. `__tostring` is used when the metatable has
one, and `__name` is used as the kind when it has no `__tostring`.

`string.format` refuses `%p`, because it would show where a value sits,
with `string.format: %p would show a memory address`. Lua 5.4.9 has no
`%F`. `%a` and `%A`, `%e` and `%E`, `%f`, and `%g` and `%G` are refused
when the number is a NaN or an infinity, with
`string.format: cannot write nan or an infinity with %<letter>; test the number first, or use tostring`.
A finite number,
`%q`, `%s`, `%d`, `%i`, `%u`, `%o`, `%x`, `%X` and `%c` are unchanged. `%q` of an
infinity is `1e9999` or `-1e9999`, and `%q` of a NaN is `(0/0)`.

A sandbox opens only when the C library's collation, numeric and
character-type locales are each `C` or `POSIX`. Any other setting is
refused at open, naming the category and the locale, for example
`the C library's numeric locale is de_DE.UTF-8; scripts need C`. String
comparison and `table.sort` on strings then follow byte order, number
text is read and written with `.`, and `upper`, `lower` and the pattern
classes such as `%a` treat a byte past 127 the same way on every
machine. Hexadecimal numbers use Lua's own conversion.

Every string hash uses one constant seed, `0x4F414C55`. `table.sort`
does not draw a pivot from the clock. Each basic floating-point
operation is rounded as written. A Visual Studio build uses `/fp:strict`.
Every other build, including a MinGW build, uses `-fno-fast-math` and
`-ffp-contract=off`. There is no fused multiply-add and no
value-changing optimisation.

`pairs` visits number keys in ascending order, then string keys in byte
order, then `false` and then `true`. The key list is taken when `pairs`
is called. A value changed before the walk reaches it is the value the
walk sees, and a key whose value became nil is skipped. Keys added
during the walk are not visited. A table whose keys include a table, a
function or any other type with no fixed order raises
`pairs: this table has keys of type <type>, which have no fixed order; keep such values in a sequence and walk it with ipairs`.
A table with a `__pairs`
metamethod keeps that metamethod. `ipairs` is unchanged. `next` returns
the key the interpreter would, so `next(t) == nil` is still the
emptiness test. The hash seed makes the same history of a table walk
the same way on every machine, and `next` follows that order. A key
that is a table has no order two machines share, and `pairs` refuses a
table that holds such a key. A mutator that lets the last write in a
loop win, or that joins text in loop order, uses `pairs` or a sequence.

`math` is Lua 5.4's library, with `pi`, `huge`, `maxinteger` and
`mininteger`. The compatibility names `cosh`, `sinh`, `tanh`, `pow`,
`frexp`, `ldexp` and `log10` are absent. `sin`, `cos`, `tan`, `asin`,
`acos`, `atan`, `exp` and `log`, and the `^` operator, are
computed by the engine, so a script gets the same bits on every machine.
`math.log(x, 2)` and `math.log(x, 10)` are the exact cases. Any other
base is `log(x) / log(base)`.
Sine, cosine and tangent are rounded once to a double. An angle at or
above 2,147,483,648 in magnitude, infinity included, is refused:
`sin: the angle must be below 2147483648 in magnitude`, and the same
sentence for `cos` and `tan`. A NaN comes back as a NaN. `log` of a
power of two is exact for `math.log(x, 2)`, and `10^k` for k from 0 to
22 is exact for `math.log(x, 10)`.
An exponent of 2 is an IEEE multiply, and an exponent of 0.5 is an IEEE
square root. An integer exponent of magnitude at most 256 is repeated
squaring. A larger integer exponent is `exp(y · log x)`, and a negative
base keeps the sign an odd integer calls for. `sqrt`, `floor`, `ceil`,
`fmod` and the four basic operations are the IEEE results.

`math.random` is Lua 5.4's generator. Each mutator's generator starts
from a seed the engine derives from the match's seed and the mutator's
id, so every machine draws the same numbers. The match's seed is a hash
of the simulation hash and of each listed mutator's id and version.
`math.randomseed()` with no argument returns to the mutator's seed. It
does not read a clock. A call with one integer, or two, sets a new seed
from those integers.

### 10.8 The time limit

Each phase stops when it has run longer than its real-time limit. The
default is 10 seconds. A steady clock is read when the phase starts and
checked in the instruction hook, every 100 instructions, and in every
library charge. The hook counts the allowance on every tenth of those
calls, so the count still steps by 1,000 instructions. There is no
thread and no signal.

The default message is
`<phase> ran for more than 10 seconds; its work is too large or never ends`.
A limit of one second says `second`. A
limit that is not a whole number of seconds names that length in
milliseconds, and one millisecond says `millisecond`. A `pcall` or
`xpcall` cannot keep the phase going. A phase that finishes inside the
limit has the same results, the same charges and the same messages as
it would with no clock. The limit only stops the script. A result that
comes back is unchanged.

The check lands at the next hook or the next charge, so a phase can run
past the limit by the work of 100 instructions, or of one charge. Most
instructions take well under a microsecond. The slowest step found is
an assignment that makes a table rebuild itself: 10 to 30 milliseconds
of processor time for a table of a million keys, about the largest
whose rebuild fits the default memory cap. A loop can do that every 4
instructions, 25 times between two reads of the clock, so it can run a
quarter to three quarters of a second of processor time past the limit,
and longer in real time on a machine busy with other work. On the
machine where this was measured (10 cores, load averages of 20 to 80),
loops that rebuild such a table stopped 0.01 to 0.9 seconds after a 2
second limit, and in three runs at the busiest 2.4 to 2.9 seconds after
it. A loop of 64 KiB zero-filled comparisons stopped within 60
milliseconds of a 300 millisecond limit, and a tight loop within the
same millisecond.

A script near the limit can pass on one machine and fail on another.
Every machine must produce the same result.

### 10.9 The fixed library

`fixed` is the engine's 16.16 number, the same scale the game uses for a
speed, a range stored that way, and the other fixed fields. It counts
65,536ths in a signed 64-bit integer. A whole number is that number
times 65,536. The whole part runs from −2,147,483,648 to 2,147,483,647,
so a count runs from −2^47 to 2^47 − 1. A result past either end raises
and names the operation. Nothing wraps.

`fixed(v)` takes a fixed value (that same value), an integer (times
65,536), a float or a string. `fixed.parse(text)` and `fixed(text)` charge one unit per 32 bytes of
that text before the scan, the same charge as `tonumber` of a text.
`fixed.parse(text)` reads text the way the
loader reads a fixed field: the number, times 65,536, truncated toward
zero. It keeps the full count rather than the low 32 bits. Text with no
digits reads as 0, and characters after the number are ignored, so
`fixed("abc")` is 0 and `fixed("1,5")` is 1. `fixed.raw(n)` builds a
value from an integer count. A fractional count is refused.
`fixed.min` and `fixed.max` take one or more values, each converted as
`fixed(v)` is.

`f:raw()` is the count. `f:text()` and `tostring(f)` are the decimal the
game writes, so the decimal is exact. `f:tonumber()` is that quotient
as a float. `f:floor()` shifts the count right by 16, toward minus
infinity, and returns an integer. `f:ceil()` moves the other way.
`f:round()` takes a half away from zero. Those three return integers,
not fixed values.

`+`, `-`, `*` and `/` convert a number the way `fixed(v)` does and
refuse a string. `*` builds the product and shifts it right by 16, so a
negative product rounds toward minus infinity. `/` is the dividend times
65,536, divided in 64 bits and truncated toward zero. Dividing by zero
raises. Unary minus negates the count. `%`, `^` and `//` raise on a
fixed value: those operators have no method. `..` joins the value's
text. Use `(a / b):floor()` where an integer division is wanted.

Every operation is integer arithmetic. A float times 65,536 is truncated
toward zero. Text goes through the loader's own reader. Text and
`tonumber` divide the count by 65,536, which is exact for every count in
range. The same counts come out on every build, the 32-bit ones included.

`fixed(1) == 1` is false. `<` and `<=` accept a number on either side.
A fixed value is a poor table key: two equal values are two keys, and
`pairs` refuses a userdata key. Use `f:raw()` as the key.

A rule still computes on the field's text. `scale(1.5)` on a field
written `1.6` stores `2.4`.
`u:fixed("maxvelocity") * fixed("1.5")` multiplies the counts the game
holds. On a field written `1.6` the hook stores `2.3999786376953125`
and the rule stores `2.4`. A factor of 1 still rewrites `7.6` as
`7.5999908447265625`, so a script skips the write when the factor is 1.
Compute a stat in a hook with `fixed`. The text a rule writes is the
text TA: Mutation wrote.

## 11. Checks

A package is checked three times, and the three checks do not do the
same work.

The manifest check is what `oa-tool mutator check` and the installer
share. It reads `oamut.yaml` and the package's files, and it does not
run a script or apply a rule. The command and the report are
[2.2 The .oamut file](#22-the-oamut-file). A rule-file warning is not
shown, and it does not refuse the package. An error does. `mutator check` also compiles the entry
script and every `.lua` file under `lua/`, and parses every `.sm` file
under `rules/`. It refuses a script that does not compile or a rule
file that has an error, which the installer would still accept.

The plan check is the plan phase. It is the refusals in
[4. The entry point](#4-the-entry-point): the shape of the table, the
declared layers, the rule files, and the hacks. A plan that fails stops
that mutator. The match does not start, unless the only failure is a
refused hack. Building the files layer still continues for the mutators
that remain, as [5. The Files layer](#5-the-files-layer) says.

The dry run is the pipeline on a store of its own, with no live match.
The Library runs it and remembers the answer. The verdict for a list is
the verdict of the last mutator. A prerequisite earlier in the list is
not that verdict.

| Verdict | When |
| --- | --- |
| `compatible` | The last mutator applied, and its journal shows an effect: a field whose text changed, or a record or file added, removed or hidden. The twelve counts, in order, are units changed, units added, units removed, weapons changed, weapons added, weapons removed, features changed, features added, features removed, menu entries changed, files added and files hidden. A sides or tables write is an effect, and it is not one of those counts. |
| `no-effect` | The last mutator applied and its journal shows none of that. The file spells it `no-effect`. |
| `incompatible` | The last mutator called `ctx.incompatible`, hit the field-write cap (`this mutator has changed too many fields`), a hook raised a script error, or the integrity phase failed. |
| `failed` | The list was refused or empty, the mutator was excluded, a script did not load, `mutate` raised a script error, the plan did not apply, a rule file has an error, the files layer or a protected path refused the mutator, the instruction allowance, the real-time limit or the string cap was spent, an engine error was raised, or the check was cancelled. A cancelled check says `the check was cancelled` and is not remembered. |

The file is `Cache/mutator-verdicts.tsv` in the player's own folder. It
is not in the Mutators folder. Each line is tab-separated: `1`, the
loadout digest, the mod key, the engine version, the verdict word,
twelve counts joined by commas, the mod label and the reason. The label
and the reason are each one line of at most 200 bytes. A line is at
most 2,048 bytes. A dry run sets no map, so maps entries do not run.
The file is at most 4 MiB. A longer file reads as no verdicts. At most
the newest 1,024 verdicts are kept. The key is the loadout digest of the
whole checked list, the mod key and the engine version.

Before a pass is published, the documents are checked the way the
loaders read them. A problem the base game or the mod already had is
not charged to a mutator. A problem that cannot stop the match is a
warning. Any other integrity failure refuses the start, unless the only
failure in the list is a refused hack. The pass is not published.

| Limit | Value | Outcome |
| --- | --- | --- |
| Unit files | 65,535 | More than this fails the mutator. |
| Category masks | 512, unless the running limits set another width | As many unit files as this width, or more, is a warning. The next type id loses its categories. |
| A shared game | 1,024 unit files | Past this fails the mutator when the match is shared, and is a warning when it is not. |
| Download files | 4,096 | More than this fails the mutator. The count includes files the menus added. |
| Sides | 5 | A side section past the five the game reads fails the mutator. Each side from `SIDE0` up to the first gap must name a live commander. |
| Movement classes | 32 | A class past `CLASS31` fails the mutator. `movementclass` has to name one of those. |
| Weapon id | 0 to 255 | An id outside that range, and a weapon section with no `ID`, is the integrity failure `weapon_id_out_of_range`. A weapon a mutator brought that cannot be given an id from 1 to 255 fails that mutator. Slot 0 is never given out. |

A weapon name, an explosion and a self-destruct have to name a weapon
that still holds its slot. `corpse` has to name a feature. `objectname`,
or the unit's name when that field is empty, has to be a model the store
holds. A missing script is a warning: the unit can still play. A unit
file's name has to be its `UnitName`, and two unit files cannot share
one `UnitName`. A download file's first five sections have to name live
units when `UNITMENU` or `UNITNAME` is not empty. A build page a mutator
changed has to already be a file the store holds. A greyed button may
name a removed unit. A legacy `Copyright=""` does not remove the file,
so a menu entry that names that unit is not, by itself, a missing unit.

## 12. Multiplayer, replays and saves

A playable match that applied at least one mutator keeps the applied
loadout and a digest. The loadout lists the mutators that applied, in
list order. A mutator that failed, was excluded or was incompatible is
not in it. Each entry is the id, the version, the package's SHA-256,
whether the package was a folder, the clash mode in force, and every
option with its default filled. For a folder, `sha256` is the digest of
that folder. The canonical text is JSON,
`{"mutators":[...],"oaloadout":1}`, at most 32 entries and at most
65,529 bytes. The loadout digest is the SHA-256 of that text.

The mutator digest hashes `oa-mutators-1`, a line feed, the canonical
loadout and the layer digest. The simulation hash a room compares is
the profile's simulation hash folded with that digest:
`oa-sim-hash-mutators-1`, a line feed, the profile's hash and the
mutator digest. With no mutator digest the simulation hash is the
profile's hash alone. Both digests are forgotten when the match
releases its mutators, including when the match ends. Nothing in the
loadout runs during a match.

A launch file carries the list under `content.mutators`, at most 32,
each an installed id or a group of `id`, `version` and `options`. The
order is the order they apply. `--mutator` beside the file replaces the
list. `--no-mutators` plays with none. The spellings are
[the command line](../command-line.md).

A recording carries the loadout text in its mutators sector when the
text is not empty and is at most 65,529 bytes. An empty loadout writes
no sector. A longer text is left out, and the recording is still
written. A recording without the sector plays no mutators.

A save carries the same text in an account named `Mutators`, in one blob
named `Loadout`, when the text is not empty and is at most 65,536 bytes.
A save written without that account is unchanged. The text is stored as
given. A campaign save keeps it the same way.

A save or a recording also keeps a content note beside the file: the
file's name with `.oa-content`. A film keeps `oa-content.txt` in the
film's folder. After the base game, the expansions and the mod, the note
lists each mutator: a `mutator` line of its SHA-256 and its id, then
`mutator-version` and `mutator-name`. The note keeps at most 32. A value
that holds a line break refuses the note. It does not carry the options
or the clash mode. Those are the loadout.

A room with mutators is for Open Annihilation players. A player on
Total Annihilation 3.1c who tries to join is told
`*** {player} plays Total Annihilation 3.1c and cannot join a game with mutators.`
A player already seated when mutators come on is told
`*** {player} plays Total Annihilation 3.1c, which cannot play with mutators.`

Loading a save or a replay that names mutators is refused with one of
these sentences. `{detail}` is what the loadout's own refusal named:

- `This needs a mutator you do not have: {detail}`
- `The mutators this was made with cannot be read.`
- `This was made with mutators, and this run plays none.`

The Library's rules line for a mutator is
`Changes the game: unranked on Core Prime`.

## 13. Versioning

`oamut` is the format version. The only value the reader accepts is the
integer `1`, written `oamut: 1`. `1.0` and `"1"` are refused, and so is
any other number, with `oamut must be 1`. A package of another format
is not read with the keys of this one left out.

`version` is the mutator's own version, not the format. Its grammar,
and the grammar of a condition's range, are
[3.1 Versions, ranges and conditions](#31-versions-ranges-and-conditions).

`requires.engine` is optional. It is a string of at most 64 bytes, one
to four comparisons of a three-part release `MAJOR.MINOR.PATCH`. Each
part is a decimal from 0 to 65535 with no leading zero. The comparisons
are `>=`, `>`, `<=`, `<` and `=`, separated by commas that spaces may
surround. Every comparison has to hold. A value that starts with `>`
is a block scalar unless it is quoted, and the manifest reader refuses
that before the range is read, so the value is written in quotes:

```yaml
requires:
  engine: ">= 0.8.0"
```

Reading the manifest does not ask whether this build meets the range.
Opening the package does, which is what the installer does and what
`oa-tool mutator pack` and `oa-tool mutator check` do. The build's
version is the three-part release that build was made with.

When the range is read and this build misses it, the player sees
`needs Open Annihilation <range>; this is Open Annihilation <build>`.
The range is one phrase for each comparison, joined with `, `:

| Comparison | Phrase |
| --- | --- |
| `>= 0.8.0` | `0.8.0 or later` |
| `> 0.8.0` | `later than 0.8.0` |
| `<= 0.9.0` | `0.9.0 or earlier` |
| `< 0.9.0` | `before 0.9.0` |
| `= 0.8.0` | `exactly 0.8.0` |

A stored requirement the range reader cannot read is refused with
`Its engine requirement cannot be read.`

## 14. Glossary

| Term | Meaning |
| --- | --- |
| Plan | The table `mutate` returns. It names the files, rules, hooks and hacks the later phases run. It does not change the game by itself. |
| Layer | One of the nine names in `layers`. A plan key, a hook and a rule write only what their layer allows. |
| Mask | A pattern on the Files layer that hides a path already in the game. |
| Add | A folder of the package the Files layer contributes, or the clash mode that keeps an existing record and stores a different one under a private name. |
| Hook | A function the plan returns for one collection. It runs after that mutator's rules. |
| Rule | A finished block, or a `.sm` file, that selects records and writes fields. The two spellings are the same rules. |
| Loadout | The mutators that applied, in list order, with the clash mode and the options that played. Its canonical text is what a room, a recording and a save share. |
| Digest | The SHA-256 of the canonical loadout, or that text folded into the simulation hash a room compares. |
| Verdict | What a dry run remembers for one list against one mod: `compatible`, `no-effect`, `incompatible` or `failed`. |
| Sandbox | The Lua state a mutator runs in. It has no file, network, clock or environment of its own, and every phase has a budget. |
| `fixed` | The engine's 16.16 number. A hook uses it so a computed stat is the same count on every machine. |
| Clash | What happens when a mutator brings a unit or weapon whose name is already in the game: `add` or `replace`. |
| Journal | The record of the last pass: each rule and hook write, each file added, and each path a mask hides. The dry run's verdict counts and the integrity blame are read from it. The conflict report is built separately, from the notes. |
| Allow-list | The paths a mutator may add. Any other path fails that mutator when the game runs, and an import leaves the file out. |
