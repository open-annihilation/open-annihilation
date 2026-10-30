# Packages

A registry serves seven kinds of package. Each is a zip archive. OA installs
a mod into `Mods/<id>`, a map pack into `Maps/<id>`, a language pack into
`Languages/<tag>`, an expansion into `Expansions/<game>/<id>`, a mutator
into `Mutators/<id>`, a brain into `Brains/<id>`, and a key layout into
`Keys/<id>`, using the id or tag inside the manifest, whatever the file is
called. `<game>` is the first base game the expansion extends.
`Expansions` is the folder a managed copy uses. That folder is the
expansion, not a mod.

| Kind | Manifest | Standard |
|---|---|---|
| `oamod` | `oamod.yaml`, at most 256 KiB | [OAMOD standard](../mods/oamod-standard.md) |
| `oamap` | `oamap.yaml`, at most 256 KiB | [OAMAP standard](../maps/oamap-standard.md) |
| `oalang` | `language.yaml`, at most 256 KiB | [Languages](../languages.md) |
| `.oaexp` | `oaexp.yaml`, at most 256 KiB | [OAEXP standard](../expansions/oaexp-standard.md) |
| `oamut` | `oamut.yaml`, at most 256 KiB | [oamut.yaml](../../src/formats/oamut/README.md) |
| `oabrain` | `oabrain.yaml`, at most 256 KiB | [oabrain.yaml](../../src/formats/oabrain/README.md) |
| `oakeys` | `oakeys.yaml`, at most 256 KiB | [OAKEYS standard](../keys/oakeys-standard.md) |

The catalogue entry for a package is [catalogue.md](catalogue.md). A mutator's
entry may name `clashes` as `add` or `replace`; absent leaves the game's
default. The file the catalogue names is the zip.

A registry MUST NOT serve the original game's data. OA does not check a
package for it.

## Addressing

A registry SHOULD place each package at `/v1/p/<sha256>.<kind>` and each
picture at `/v1/i/<sha256>.png`, on the host that serves the catalogue.
`<sha256>` is 64 lower-case hex digits, the SHA-256 of the package file or
of the picture.

OA requires the catalogue's `file` to be a reference whose last segment is
`<sha256>.<kind>`. An expansion's last segment is `<sha256>.oaexp`. A
relative reference with that last segment is also accepted. A picture in the catalogue only has to be a reference ending
`.png`. OA does not check that a picture's file name is a hash. It does
check that the package file's last segment is the digest.

The reference is resolved against whichever host served the catalogue, so a
mirror that serves the same tree serves the same paths.

## What the archive may hold

OA reads the zip before it writes anything. It refuses an archive that is
not a zip, a damaged record, an encrypted entry, or an entry packed any way
other than stored or deflated. It refuses a name that is not safe on every
system, two names that differ only in case, a link or a special file, and an
id that names a reserved device.

The unpacked size MUST NOT exceed 4 GiB. OA keeps 64 MiB free on the disk
that holds the install folder, beyond the package's files. Once the unpacked
size is above 64 MiB, it MUST NOT be more than 200 times the size of the
archive. The archive MUST NOT unpack more than 16,384 folders, counting
folders its paths pass through. Each unpacked path keeps 16 characters spare
below the longest path the system opens.

A map preview inside a pack MUST be a PNG of at most 2 MiB and at most 1024
pixels on a side. A mod whose `requires.engine` this build does not meet is
refused. A map that does not fit the base game is refused. A language pack
is refused when a font it lists is missing or unreadable, its warm-up text
is missing, too large or not UTF-8, or its table does not parse.

Before a package is offered for use, OA has checked those limits, the hacks
the catalogue names, and, for a map, whether it fits.

An expansion is checked against the built-in table before anything is
written. Each file must be that expansion's file by name, size and SHA-256.
The copy lands in `Expansions/<game>/<id>` with a `.oa-content` record. The
source is `registry <id>` when the package came from a registry, and the
package file's path when the player opened the file. The generic installer
does not unpack an expansion, and the folder does not hold an
`.oa-origin.yaml`.

## The origin record

When OA installs a package it writes `.oa-origin.yaml` at the top of the
installed folder. The record is at most 4096 bytes, UTF-8, with LF line
endings. The keys, in order, are `oa-origin` (`1`), `origin` (`file` or
`catalogue`), then `registry`, `catalogue-id` and `release` for a catalogue
origin, then `sha256` and `installed`.

`registry` is a registry id. `catalogue-id` is the package key and MUST NOT
contain `@`. `release` is a whole number from 1 to 2147483647. `sha256` is
64 lower-case hex digits of the package file. `installed` is a real UTC day,
`YYYY-MM-DD`. A folder with no record was installed by an earlier version or
made by hand; that is not an error.

## `oa-tool pack` and `oa-tool check`

`oa-tool pack FOLDER` writes a `.oamod`, a `.oalang`, a `.oamap`, a
`.oaexp`, a `.oamut`, a `.oabrain` or a `.oakeys`. The folder's top holds
one manifest, `oamod.yaml`, `language.yaml`, `oamap.yaml`, `oaexp.yaml`,
`oamut.yaml`, `oabrain.yaml` or `oakeys.yaml`. The manifest is written first, then every other file in
byte order of its path, with fixed times. A file whose extension, without
regard to case, is `png`, `jpg`, `jpeg`, `gif`, `ogg`, `mp3`, `zip`, `gz`,
`bz2`, `xz`, `7z`, `oamod`, `oalang` or `oamap`, and an empty file, is
stored. Every other file is deflated. An expansion is different: every file
is stored. The same files give the same bytes on any machine with the same
zlib.

`--out FILE` names the package. Without it, the name comes from the manifest
and is written in the current folder. A mutator's default name is
`<id>-<version>.oamut`. A brain's default name is `<id>-<version>.oabrain`.
A key layout's default name is `<id>-<version>.oakeys`.
`--force` replaces an existing file. A folder the installer would refuse is
not packed, and a mutator whose manifest, Lua source or rule file is broken
is not packed: every problem is listed, a manifest problem as
`oamut.yaml:<line>:<column>`, and a script or rule problem as
`<path>:<line>: <message>`. A brain whose manifest, layout, Lua source,
network SHA-256 or icon is broken is not packed either: a manifest problem
is `oabrain.yaml:<line>:<column>`, a script problem is
`<path>:<line>: <message>`, a file outside the layout is
`<path>: outside the files a brain may hold`, and a network whose bytes are
not the digest the manifest names is
`<file>: its SHA-256 is <hex>, not the <hex> oabrain.yaml gives`. Lua is
compiled and rule files are parsed, and neither is run. A network file is
hashed and not interpreted. A map pack needs `--game-dir DIR` for the
game's palette, which previews use. `--game-dir` on a mod, a language pack,
an expansion, a mutator, a brain or a key layout is a usage error.

`oa-tool check FILE` runs the same checks an install runs before it writes,
and reports whether that install would accept the file. `--json` prints one
JSON object. The keys, in order, are `check`, `ok`, `problems`, `warnings`,
`file`, `kind`, `id`, `name`, `version`, `revision`, `size`, `sha256`,
`unpacked_size`, `files`, then the kind's own facts:

- a mod: `summary`, `homepage`, `tags`, `author`, `requires`, `sim_hash`,
  `full_hash`, `hacks`, `packaging`;
- a language pack: `language` in place of those;
- a map pack: `summary`, `author`, `homepage`, `tags`, `requires`,
  `packaging`, then `maps`;
- an expansion: `extends`, `needs` and `author`;
- a mutator: `summary`, `description`, `homepage`, `tags`, `author`,
  `requires`, `works_with`, `conflicts_with`, `layers`, `options`, `icon`,
  `entry`, `scripts`, `rules` and `legacy`;
- a brain: `summary`, `description`, `homepage`, `tags`, `author`,
  `requires`, `works_with`, `levels`, `full_awareness`, `networks`,
  `network_bytes`, `options`, `icon`, `entry`, `scripts` and `data`;
- a key layout: `summary`, `description`, `homepage`, `tags`, `author`,
  `requires`, `works_with`, `base`, `keyboard`, `pointer`, `helpers`,
  `changes`, `triggers`, `unknown_actions`, `clashes` and `icon`.

`requires.engine` is the requirement as written. `requires.mutators` and
`conflicts_with` are objects with `id` and, when a range was written,
`version` holding that range. `works_with` and `layers` are the manifest's
strings. `options` lists `id`, `label`, `type` and `default`, and a choice's
`values` and `labels` or a number's `min`, `max` and `step`. A string choice
value is an identifier of 1 to 32 bytes of letters, digits, `_`, `-` and `.`;
its label stays 1 to 64 bytes. `icon` gives
the icon's `path`, `sha256`, `size`, `width` and `height`. `scripts` is how
many Lua files compiled. `rules` lists the `.sm` paths in byte order.
`legacy` lists each original's `name`, `prefix`, `compatibility` and
`files`. A mutator's Lua is compiled and its rule files are parsed, and
neither is run. `oa-tool mutator pack` and `oa-tool mutator check` are the
same two commands limited to a mutator: a folder without `oamut.yaml`, or a
file of another kind, is a usage error.

A brain's `summary` is the description's first line. `author` is the name.
`requires` is an object: `engine` as written, and `expansions` an array of
ids, left out when the manifest names neither. `works_with` is the
conditions as written. `levels` lists each level's `id`, `name` and
`difficulty`. `full_awareness` is always a boolean. `networks` lists each
network's `id`, `file`, `sha256` and `size`, and is left out when the brain
ships none. `network_bytes` is always a whole number, those sizes added
together. `options` and `icon` have the mutator's shape. `scripts` is how
many Lua files compiled. `data` lists the table paths in byte order. A
brain's Lua is compiled and not run, and a `.oann` file is hashed and not
interpreted. `oa-tool brain pack` is `pack` limited to a brain: a folder
without `oabrain.yaml` is a usage error.

A key layout's `summary` is the description's first line, cut at 200 bytes.
`author` is the name. `requires` is an object whose only member is `engine`,
as written, and is left out when the manifest names none. `works_with` is
the conditions as written. `base` is `oa-default` or `none`. `keyboard` is
`any`, `ansi`, `iso` or `jis`, and `any` when the manifest names none.
`pointer` is `right-click` or `left-click`, left out when the manifest names
none. `helpers` lists the names the manifest sets, in the order toggle,
sticky-modifiers, hold-to-confirm, pointer-keys, radial-orders and
edge-scroll, including one set to false or to an empty list, and is left
out when none are set. `changes` is how many actions the layout binds.
`triggers` lists each trigger the layout's own bindings name, once, in the
order of first use. `unknown_actions` and `clashes` are whole numbers, and
`keyboard`, `base`, `changes`, `triggers`, `unknown_actions` and `clashes`
are always written. `icon` has the mutator's shape. A warning is a layout
problem reported with no keyboard map, written as
`oakeys.yaml:<line>:<column>: <key>: <message>`. `ok` is false for an error,
and, with `--strict`, for any warning. `--strict` changes no other kind.
The package is the one the key-layout packer writes: `oakeys.yaml` first,
then every other file in byte order of its path, with no clock time. A
`.png` is stored and every other file is deflated. `oa-tool keys pack` is
`pack` limited to a key layout: a folder without `oakeys.yaml` is a usage
error. `oa-tool keys check` is `check` limited to a `.oakeys` or a folder
whose top holds `oakeys.yaml`. A folder check names the folder, and the size
and hash are the package the folder would pack to. `oa-tool keys picture
FILE --out PNG` draws the keyboard picture of what the layout changes, 1200
pixels wide unless `--width` says otherwise, with the fonts beside the
program unless `--fonts` names a folder. `any` draws an ANSI keyboard.
`oa-tool keys show FILE` prints each action with its context and chords
after the base and the layout. `oa-default` is the base with no layout.
`--json` prints an array of `action`, `context` and `chords`.

An expansion does not take `--game-dir`. A brain does not take `--game-dir`.
A key layout does not take `--game-dir`.

A key the package does not have is left out. `revision` is null when a
language pack has no packaging revision, and for every mutator, every brain
and every key layout. A map pack
needs `--game-dir`, the
game folder. Each `--mod FOLDER[=KEY]` is one more target after the base
game `ta-3.1c`. `maps` lists each map. `compatible` holds `ta-3.1c` and then
each mod, and `failures` holds the targets that do not fit.
`--accept-unimplemented-hacks` accepts a hack this build does not carry out
yet. The command exits 0 when the package would be installed, and 1 when it
would be refused or a map does not fit `ta-3.1c`.

The `hacks` object `check --json` prints is the object a catalogue entry
holds: `sim`, `view` and `ids`.
