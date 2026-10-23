# Mutator manifests

`oa-formats-oamut` reads, checks and writes a mutator's manifest, `oamut.yaml`.
The installer, the catalogue and the Library learn what a mutator is from
this file, without running any of its code. Target `oa-formats-oamut`,
header `oa/formats/oamut/manifest.hpp`, namespace `oa::formats::oamut`.

The text is the strict YAML a mod profile is written in
(`oa/formats/oamod.hpp`). `read_document` reads the bytes;
`read_package_keys` reads `homepage`, `tags` and `requires.engine`.

## Entry points

- `read_manifest` reads one manifest. Every broken rule is reported, and the
  manifest is left unchanged when any rule is broken. A document the strict
  reader refuses is one problem on the key `document`. `describe` prints one
  problem as `oamut.yaml:<line>:<column>: <key>: <message>`.
- `write_manifest` writes a manifest back in the key order below, with
  two-space indentation. Every string is double-quoted (`\n`, `\"` and `\\`
  are escaped). Numbers use their canonical text. A short list is a flow
  sequence. An optional key that was left out is left out again, and so is
  `entry` when it is `mutator.lua`.
- `summary_of` is the description's first line, cut to 200 bytes on a UTF-8
  character boundary. That is the line the Library and the catalogue show.
- `parse_version`, `version_text`, `parse_version_range`, `version_in_range`,
  `parse_condition` and `condition_met` are the version and condition grammar
  below. `layer_name` and `parse_layer` are the nine layer names.
  `clash_mode_name` and `parse_clash_mode` are `add` and `replace`.

`oa/formats/oamut/shared.hpp` is the part a brain manifest shares: the author,
works-with, options and strings, read and written by the same functions this
reader uses. A brain passes its own language keys, such as `levels`, and
those keys are not refused. The mutator reader passes none. Nothing a
mutator accepts changes.

## State

None. Callers pass bytes and a `Manifest` and get values back. Nothing here
is installed for the rest of the process, and nothing opens a file.

## Invariants

- `oamut` is the integer 1. A later format is refused.
- A mutator id is lower-case kebab-case of 1 to 64 bytes, the same rule as a
  catalogue package key. The formats layer does not link the catalogue, so
  the rule is written here.
- A version compares by value. A part that was not written compares as 0, so
  `2`, `2.0` and `2.0.0` are equal. `parts` remembers how many were written,
  and the writer uses that.
- `requires.mutators` is kept in the order written. That order is the order
  the prerequisites apply in, all before this mutator.
- `legacy` is kept in the order written, one record for each original the
  package came from.
- A key the manifest does not define is refused. A profile key is refused
  with its own message, including `hacks` at the top level. `hacks` inside
  `layers` is a layer name.
- `homepage`, `tags` and `requires.engine` follow the shared package-key
  rules. Tags are then limited to four. When the shared rules already report
  a tag problem, the four-tag limit is not reported again.

## Manifest rules

The top-level keys, in the order `write_manifest` writes them. A profile key
(`identity`, `layout`, `limits`, `script-extensions`, `data-keys`, `hacks`,
`settings`, `media`, `packaging`, `supportedGames`, `requiredExpansions`) is
refused with `<key> belongs in a mod's oamod.yaml; a mutator cannot carry it`.
Any other unknown key is refused with `<key> is not a key of oamut.yaml`.

Required:

- `oamut`: the integer 1. `1.0` and `"1"` are refused.
- `id`: lower-case kebab-case, 1 to 64 bytes. No leading, trailing or doubled
  hyphen.
- `name`: 1 to 64 bytes, with no control character.
- `version`: a quoted mutator version. A plain number, and a plain scalar
  such as `2.0.1`, are refused with `write the version in quotes, as "2.0"`.
- `author`: a mapping of `name` (1 to 128 bytes) and an optional `email`.
  The address has one `@`, a dot in the part after it, no space and no
  control character, and is at most 254 bytes.
- `tags`: the shared tag rule (1 to 8, each lower-case kebab-case of 1 to 32
  bytes, none repeated), and then at most 4.
- `works-with`: 1 to 32 conditions. No id twice. `*` is the only entry when
  it appears, and means any mod and the base game played alone.
- `layers`: 1 to 9 of `files`, `units`, `weapons`, `features`, `sides`,
  `tables`, `menus`, `maps`, `hacks`, none repeated. `features` is wrecks
  and features, `sides` is `gamedata/sidedata.tdf`, `tables` is the other
  `gamedata` tables.

Optional:

- `clashes`: `add` or `replace`, compared byte for byte. `add` brings a unit
  or weapon in beside one that already has that name. `replace` puts this
  mutator's record in its place. Absent means the game's default applies;
  the reader does not fill one in. `Add`, `merge` and a number are refused
  with `clashes must be add or replace`.
- `description`: 1 to 2,000 bytes. A line feed is allowed. No other control
  character. The first line is the summary.
- `homepage`: the shared rule, an `http` or `https` address of 1 to 256 bytes.
- `icon`: a package path ending in `.png` in any case. Parts are separated
  by `/`. No part is empty, `.` or `..`. No leading `/` or `\`, and no `\`
  anywhere. The pixels are not read here.
- `requires`: a mapping of only `engine` and `mutators`. `engine` is the
  shared engine requirement (three-part releases). `mutators` is 1 to 16
  conditions on mutator ids, none repeated, and not this mutator's own id.
- `conflicts-with`: 1 to 32 conditions on mutator ids, none repeated, not
  this mutator's own id, and not an id `requires.mutators` names.
- `options`: a mapping of at most 16 option ids. An id is a Lua name,
  `[a-z][a-z0-9_]*`, of 1 to 32 bytes, so a script reads
  `ctx.options.<id>`. Lua 5.4's reserved words are refused (`and`, `break`,
  `do`, `else`, `elseif`, `end`, `false`, `for`, `function`, `goto`, `if`,
  `in`, `local`, `nil`, `not`, `or`, `repeat`, `return`, `then`, `true`,
  `until`, `while`). The message is `is a word Lua reserves`, on the
  option's path, for example `options.for`. A word that only contains a
  reserved word (`format`, `ends`, `for_`) is a name. A different case
  (`For`) is not a name. Write `true` and `false` in quotes: an unquoted
  `true:` is a boolean key, which the document reader refuses.
  Each option is a mapping of `type` (`toggle`, `choice` or `number`),
  `label` (1 to 64 bytes, no control character) and `default`, plus the
  keys of its type. Any other key is refused.
  - A toggle's `default` is `true` or `false`.
  - A choice has `values` (2 to 16, all numbers or all identifiers, none
    repeated) and `default` equal to one of them. An identifier is 1 to 32
    bytes of ASCII letters, digits, `_`, `-` and `.`. The words a player
    reads are the label, not the value. `labels`, when written, lists one
    label of 1 to 64 bytes for each value, in the same order.
  - A number has `min`, `max` and `step`, exact numbers. `min` is below
    `max`, `step` is above 0, and `(max - min) / step` is a whole number of
    at most 1,000 steps. `default` lies on a step, including an endpoint.
- `strings`: a mapping of at most 16 language tags. A tag is 2 to 35 bytes:
  a run of letters, then hyphen-joined parts of letters and digits, in either
  case. Each language is a mapping of optional `name`, `description`,
  `options` (option id to a label) and `labels` (option id to a list of value
  labels, as many as that choice has values). An option id the manifest does
  not define is refused, and so are value labels for an option that is not a
  choice. A translated name or label is 1 to 64 bytes. A translated
  description follows the description rule.
- `entry`: a package path ending in `.lua`, not under `files/`, `rules/` or
  `lua/`. The default is `mutator.lua`.
- `legacy`: a list of 1 to 16 records, in order. A mapping in place of the
  list is refused with `legacy is a list of records, one for each original`.
  Each record is a mapping of `name` (1 to 128 bytes, no control character),
  `prefix` (`"{}"` or `"}{"`), `compatibility` (1 to 8 codes of 1 to 8
  letters and digits, or `*`) and `files` (1 to 16 file names of 1 to 255
  bytes, with no `/` or `\`).

A problem names the value's path, such as `options.factor.default`,
`requires.mutators[0]` or `strings.de.options.factor`. Indexes count from 0.
The line and column are the value's, 1-based, in bytes.

## Grammar

A mutator version is one to three parts joined by `.`, each a decimal from
0 to 65535 with no leading zero, at most 32 bytes. It is written in quotes,
because a plain `2.10` is a YAML number equal to `2.1`.

A version range is one to four comparisons (`>=`, `>`, `<=`, `<` or `=`),
separated by commas that spaces may surround, at most 64 bytes. It is the
engine requirement's grammar with mutator versions in place of three-part
releases, so `>= 4.0` and `>= 2` are ranges. `parse_version_range` reports
the 1-based byte where a broken range fails.

A condition is an id alone, or an id, one or more spaces and a range. An id
is lower-case kebab-case of 1 to 64 bytes, `*` or `ta-3.1c`. `*` and
`ta-3.1c` take no range. In `works-with`, `*` means any mod and the base
game, `ta-3.1c` means the base game with no mod, and any other id is a mod.
In `requires.mutators` and `conflicts-with` an id is a mutator id.

`condition_met` compares ids byte for byte. A condition with no range is met
by its id at any version text. A range is met only when the version text is
a mutator version inside the range, so a mod version such as `5b` misses.
A range that holds a comma is quoted in YAML.

## Tests

`formats-oamut` reads `tests/version_cases.txt`: versions, ranges (which
versions meet and miss) and conditions (which id and version meet and miss,
including a mod version `5b`). It reads the example manifest, reads a
manifest that uses every key, writes it and reads it back, and refuses each
rule above once, including a profile key, an unknown key, `layers: [sounds]`,
`clashes` set to `Add`, `merge` or a number, and a bare `legacy` mapping.
An absent `clashes` stays absent through a round trip. `summary_of` is
checked on a character boundary.
The test does not open a package.

Examples use the made-up mutators `steady-hands` and `long-shadows` and the
made-up mod `ridge`.

## Limitations

No file is opened. The icon's pixels, and the size the Library accepts, are
checked by the installer and `oa-tool`. So are the scripts, the package
layout and whether this build meets `requires.engine`. This module does not
read a mutator's files, and it does not decide the order prerequisites run
in beyond keeping `requires.mutators` as written.
