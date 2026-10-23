# Brain manifests

`oa-formats-oabrain` reads, checks and writes a brain's manifest, `oabrain.yaml`.
The installer, the catalogue, the Library and the seat picker learn what a
brain is from this file, without running any of its code. Target
`oa-formats-oabrain`, header `oa/formats/oabrain/manifest.hpp`, namespace
`oa::formats::oabrain`.

The text is the strict YAML a mod profile is written in
(`oa/formats/oamod.hpp`). `read_document` reads the bytes.
`read_package_keys` reads `homepage`, `tags` and `requires.engine`. The
author, works-with, options and strings are the mutator rules, called
through `oa/formats/oamut/shared.hpp`.

## Entry points

- `read_manifest` reads one manifest. Every broken rule is reported, and the
  manifest is left unchanged when any rule is broken. A document the strict
  reader refuses is one problem on the key `document`. `describe` prints one
  problem as `oabrain.yaml:<line>:<column>: <key>: <message>`.
- `write_manifest` writes a manifest back in the key order below, with
  two-space indentation. Every string is double-quoted. `fullAwareness` is
  written only when it is true, as the unquoted boolean `true`. `entry` is
  written only when it is not `brain.lua`. An optional block that was left
  out is left out again.
- `summary_of` is the description's first line, cut to 200 bytes on a UTF-8
  character boundary. That is the line the Library and the catalogue show.
- `brain_id`, `brain_tag`, `level_id`, `difficulty_name` and
  `parse_difficulty` are the id, tag and difficulty checks below.
  `find_level` returns a level of the manifest by id.
- `file_role` says which part of a brain a path is, or that a brain may not
  hold it. The path is the text as written. A path ending in `/` is a folder
  and is never asked.

## State

None. Callers pass bytes, a path and a `Manifest` and get values back.
Nothing here is installed for the rest of the process, and nothing opens a
file.

## Invariants

- `oabrain` is the integer 1. A later format is refused.
- A brain id is a mutator id: lower-case kebab-case of 1 to 64 bytes. The
  formats layer does not copy that grammar; it calls `oamut::mutator_id`.
- A version is a mutator version of one to three parts, written in quotes.
  `parts` remembers how many were written, and the writer uses that.
- `works-with`, `options` and `strings` keep the order written. A brain's
  language block may also hold `levels`. The mutator reader passes no such
  key, and nothing a mutator accepts changes.
- `levels` and `networks` are kept in the order written.
- `fullAwareness` is false when the key is absent. Only a boolean is
  accepted. A string such as `"true"` is refused.
- A difficulty is `easy`, `medium` or `hard`, byte for byte, with values 0,
  1 and 2, so a caller can map it onto a seat's income rules by value.
- A key the manifest does not define is refused. A profile key is refused
  with its own message. A mutator's `layers`, `clashes`, `conflicts-with`
  or `legacy` is refused with its own message.
- `homepage`, `tags` and `requires.engine` follow the shared package-key
  rules. Tags are then limited to four, and each must be one of the nine
  brain tags. When the shared rules already report a tag problem, the
  four-tag limit and the membership check are not reported again. When the
  list is longer than four, membership is not checked either.
- An expansion id is checked for shape only. The installer checks that each
  id is a built-in bundle.

## Manifest rules

The top-level keys, in the order `write_manifest` writes them, are
`oabrain`, `id`, `name`, `version`, `author`, `description`, `homepage`,
`icon`, `tags`, `works-with`, `requires`, `levels`, `fullAwareness`,
`networks`, `options`, `strings` and `entry`.

A profile key (`identity`, `layout`, `limits`, `script-extensions`,
`data-keys`, `hacks`, `settings`, `media`, `packaging`, `supportedGames`,
`requiredExpansions`) is refused with `<key> belongs in a mod's oamod.yaml;
a brain cannot carry it`. `layers`, `clashes`, `conflicts-with` and `legacy`
are refused with `<key> is a mutator's key; a brain cannot carry it`. Any
other unknown key, a key that is not a string included, is refused with
`<key> is not a key of oabrain.yaml`.

Required:

- `oabrain`: the integer 1. `1.0` and `"1"` are refused with `oabrain must
  be 1`.
- `id`: lower-case kebab-case, 1 to 64 bytes. No leading, trailing or
  doubled hyphen.
- `name`: 1 to 64 bytes, with no control character.
- `version`: a quoted mutator version, such as `"2.0"`, of one to three
  parts. A plain number, and an unquoted scalar such as `two`, are refused
  with `write the version in quotes, as "2.0"`. A quoted text that is not a
  mutator version is refused with `version must be a mutator version, such
  as "2.0"`. Anything else is `version must be a string`.
- `author`: the mutator author. A mapping of `name` (1 to 128 bytes) and an
  optional `email`. The address has one `@`, a dot in the part after it, no
  space and no control character, and is at most 254 bytes.
- `description`: 1 to 2,000 bytes. A line feed is allowed. Any other control
  character is refused. A description with line breaks is one double-quoted
  string with `\n`.
- `tags`: the shared tag rule (1 to 8, each lower-case kebab-case of 1 to 32
  bytes, none repeated), and then 1 to 4 of `classic`, `economy`, `rush`,
  `turtle`, `air`, `naval`, `learned`, `teaching`, `experimental`, compared
  byte for byte. A tag that is not one of those is `tags[i] must be one of
  classic, economy, rush, turtle, air, naval, learned, teaching,
  experimental`. More than four, when the shared rule passed, is one
  problem: `tags lists N tags; at most 4`.
- `works-with`: the mutator rule. 1 to 32 conditions, none twice. `*` is the
  only entry when it appears. Paths are `works-with[i]`, indexes from 0.
- `levels`: a list of 1 to 6 mappings. Each mapping is exactly `id`, `name`
  and `difficulty`, all required. `id` is lower-case kebab-case of 1 to 32
  bytes, and no id is repeated. `name` is 1 to 64 bytes with no control
  character. `difficulty` is `easy`, `medium` or `hard`. A missing
  difficulty and a wrong one are the same problem: `levels[0].difficulty
  must be easy, medium or hard`. An empty list is `levels must list 1 to 6
  levels`. A value that is not a list is `levels must be a list`.

Optional:

- `homepage`: the shared address rule, an `http` or `https` address.
- `icon`: when written, exactly `icon.png`. Anything else is `icon must be
  icon.png, the picture at the package's top`.
- `requires`: a mapping of only `engine` and `expansions`. Any other key,
  `mutators` included, is `<key> is not a key of requires`. `engine` is the
  shared engine requirement. `expansions` is a list of 1 to 16 lower-case
  kebab-case ids of 1 to 64 bytes, none twice.
- `fullAwareness`: `true` or `false`. Absent means false. A string is
  `fullAwareness must be true or false`.
- `networks`: a list of 1 to 16 mappings of exactly `id`, `file` and
  `sha256`. `id` is an option id: a Lua name of 1 to 32 bytes, `[a-z]` then
  `[a-z0-9_]*`, and not a word Lua reserves (`is a word Lua reserves`). No
  id is repeated. `file` is `nets/<stem>.oann`. The stem is 1 to 64 bytes of
  `a`-`z`, `0`-`9` and `_`, and no file is repeated. `sha256` is a quoted
  string of 64 lower-case hex digits. A number, an unquoted scalar, or any
  other text is `networks[0].sha256 must be 64 lower-case hex digits in
  quotes`. An empty list is `networks must list 1 to 16 networks`.
- `options`: the mutator options block, at most 16. Each id is a Lua name.
  A number option's `min`, `max`, `step` and `default` are the mutator
  arithmetic, called, not copied.
- `strings`: the mutator strings block, at most 16 language tags. Each
  language may hold `name`, `description`, `options`, `labels` and `levels`.
  `levels` is a mapping from a level id this brain defines to a name of 1
  to 64 bytes with no control character. An unknown id is
  `strings.de.levels.brutal is not a level of this brain`. A language tag
  that is refused is not kept, and its `levels` are not read again. A
  language that is not a mapping is already reported by the mutator reader;
  its `levels` are not read again.
- `entry`: `<stem>.lua` at the package's top. The stem is 1 to 64 bytes of
  `a`-`z`, `0`-`9` and `_`. Anything else, a folder such as `lua/brain.lua`
  included, is `entry must be a Lua file at the package's top, named with
  lower-case letters, digits and _`. Absent means `brain.lua`.

Indexes in a path start at 0. A problem is reported at the value: the first
byte of a scalar, including its opening quote, the `[` of a flow sequence,
or the first `-` of a block sequence. A missing required key is reported at
the document's start.

## File roles

`file_role` decides in this order:

- `oabrain.yaml`, compared without regard to ASCII case, is the manifest.
- The manifest's `entry`, compared byte for byte, is the entry.
- `lua/` then one or more parts of `[a-z0-9_]+` joined by `/`, ending
  `.lua`, is a module when the dotted module name (`a.b` for `lua/a/b.lua`)
  is at most 64 bytes. A capital letter, a hyphen, an empty part, or a name
  longer than 64 bytes is outside. A file under `lua/` that is not a module
  is outside.
- A `file` a network of the manifest names, byte for byte, is a network.
  Any other `nets/` path is outside.
- `data/<stem>.csv`, `data/<stem>.tdf`, `data/<stem>.json` or
  `data/<stem>.txt`, one folder deep, is a data table. The stem is 1 to 64
  bytes of `[a-z0-9_]`. A capital, a deeper path, or another suffix is
  outside.
- `icon.png` is the icon only when the manifest's `icon` is `icon.png`.
  Without that key the file is outside.
- `README.md` at the package's top, compared without regard to ASCII case,
  is the readme. `LICENSE` at the top, the same way, is the licence.
- Anything else is outside, including `licence`, `docs/README.md`, and
  `brain.lua` when `entry` is some other file.

## Tests

`formats-oabrain` reads a made-up brain `steady-commander` that uses every
key (two levels, one network, a number option, a choice option, German
strings with level names, `requires.expansions` of `core-contingency`, and
`works-with` of `ta-3.1c` and `ridge >= 28`), writes it and reads it back.
It reads a minimal `quick-raider` of the required keys only, with
`fullAwareness` false and `entry` `brain.lua`. It refuses each rule above
once, including a profile key, a mutator key, an unknown key,
`requires.mutators`, seven levels, a repeated level id, `difficulty:
brutal`, `fullAwareness: "true"`, a numeric `sha256`, `nets/Threat.oann`,
`icon: badge.png`, five tags, the tag `aim`, a missing description and
`entry: lua/brain.lua`. It checks `file_role` for each role and for the
paths a brain may not hold, and `summary_of` on a character boundary.
The test does not open a package.

## Limitations

No file is opened. The bytes of a script (at most 1 MiB), of every network
together (at most 64 MiB) and of one data table (at most 4 MiB) are recorded
here as `most_script_bytes`, `most_network_bytes` and `most_data_bytes`. The
installer and `oa-tool` apply them, and they check the scripts, the icon's
pixels, the sizes and the hashes. The installer checks each expansion id
against the built-in bundles. This module does not read a network's bytes
and does not decide whether a brain may be seated.
