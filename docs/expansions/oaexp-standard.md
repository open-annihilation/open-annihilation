# The OAEXP standard

An expansion package, `oaexp.yaml` inside a `.oaexp` file, delivers one
built-in expansion. The expansion is one the game already knows: its id,
its English name, the base games it extends and the expansions it needs.
Every file in the package is that expansion's file, by name, size and
SHA-256. A package can only deliver those known bytes.

This document is the format and its rules as the engine implements them.
The archive is a zip, as a mod package's archive is
([4.6 The .oamod package](../mods/oamod-standard.md#46-the-oamod-package)).
This standard names what is different for an expansion.

## Contents

- [1. Goals](#1-goals)
- [2. The package](#2-the-package)
- [3. The manifest](#3-the-manifest)
- [4. The files](#4-the-files)
- [5. Where it installs](#5-where-it-installs)
- [6. The catalogue](#6-the-catalogue)
- [7. A full example](#7-a-full-example)

## 1. Goals

| Goal | What it means in practice |
| --- | --- |
| Known bytes only | Each file is the built-in table's file. A different size, a different SHA-256, or a file the table does not name is refused, and nothing is written. |
| One manifest | `oaexp.yaml` is what the packer writes and what the installer checks. A key this standard does not name is refused. |
| The same record | The installed folder holds a `.oa-content` record, the record a copied game already uses. |

## 2. The package

An expansion is one `.oaexp` file. The manifest `oaexp.yaml` sits at the
top of the archive, or in the one folder the top holds. The manifest is at
most 256 KiB. It is the strict YAML a mod profile uses
([3.3 Strict YAML rules](../mods/oamod-standard.md#33-strict-yaml-rules)).
One document, one top-level mapping, no anchors and no block scalars.

`oa-tool pack` stores every file, including the manifest. The manifest is
written first, then the other files in byte order of their paths. `--out`
names the package. Without it, the name is `<id>.oaexp` in the current
folder. `--force` replaces an existing file. `--game-dir` on an expansion
is a usage error.

`oa-tool check` reports whether the package would be installed. It does
not take `--game-dir`. `--json` adds `extends`, `needs` and `author` after
the shared facts.

## 3. The manifest

`oaexp` is `1`. A key that is not in this table is refused. `version` is a
string: an unquoted `1` is refused, so write `"1"`.

| Key | Required | Rule |
| --- | --- | --- |
| `oaexp` | yes | The integer `1` |
| `id` | yes | A built-in expansion id, compared exactly |
| `name` | yes | That expansion's English name, 1 to 64 bytes |
| `version` | yes | A string of 1 to 32 bytes. Nothing orders it |
| `extends` | yes | The expansion's base games, in the table's order. The list is not empty |
| `needs` | yes | The expansions it needs, in the table's order. `[]` when it needs none |
| `author.name` | yes | 1 to 64 bytes |
| `author.email` | no | 1 to 64 bytes, with one `@` that is not the first or last byte |
| `packaging.revision` | yes | An integer from 1 to 65535 |
| `packaging.date` | no | `YYYY-MM-DD`, a real Gregorian day, written as a string |
| `packaging.packager` | no | 1 to 64 bytes |

`author` may hold `name` and `email`. `packaging` may hold `revision`,
`date` and `packager`. Any other key under either of them is refused.

## 4. The files

The other files are the expansion's files. Each copy name, size and
SHA-256 is the built-in table's. A name is compared exactly, so a different
case is a different file. A name with a `..` part, a leading or trailing
slash, or a backslash is refused. The same copy name twice is refused. The
manifest itself is not one of those files.

The check happens before any byte of the expansion is written into the
player's folder. A package the table does not know is refused whole.

## 5. Where it installs

The files are copied into `Expansions/<game>/<id>`. `<id>` is the
manifest's id. `<game>` is the first base game in `extends`. `Expansions`
is the folder a managed copy uses, beside `Games`. That folder is the
expansion. It is not a mod folder, and the Mods tab does not list it.

The folder holds the expansion's files and a `.oa-content` record. The
record's bundle id is the expansion's id. Its source is `registry <id>`
when the package came from a registry, using that registry's id. When the
player opened the file, the source is the file's path. A registry id that
holds a newline is refused. After the source, the line `package <sha256>
<release>` names the package the files came from: the whole `.oaexp`'s
SHA-256, as a catalogue lists it, and the catalogue's release, or 0 for a
file the player opened. A reader before 0.9 skips the line, and no other
bundle's record holds one. A catalogue entry fetches the package again
only when the catalogue's SHA-256 is another one, the copy is missing, or
the record has no package line.

Installing again over a folder whose record already names this expansion
replaces its files where they are. A file that already holds the
package's bytes is left as it is, and the folders stay, so a file sync
sees no folder go and come back. A file the table does not name, such as
one the game wrote beside the package's own, is kept. A file the old
record named that the package no longer holds is removed, and so is an
empty numbered copy of one of the package's folders ("Help 2") a file sync
left. The record is written without its package line first, and with the
new one last, so an install that stops part way is fetched again. A link
in the expansion's place is left where it is, and so is a folder whose
record names something else.

Uninstalling removes `Expansions/<game>/<id>` when the record's bundle id
is that expansion. Another expansion's folder is left in place. A link is
left in place.

The generic package installer does not unpack an expansion, and the folder
does not hold an `.oa-origin.yaml`.

## 6. The catalogue

A catalogue entry's `kind` is `expansion`. The file's last segment is
`<sha256>.oaexp`. `version` is required. `extends` is the expansion's base
games, the same list as the manifest, in order. `id` is the built-in
expansion id. `name` is the ordinary catalogue name, 1 to 64 bytes. The
manifest requires the English name; the catalogue entry does not.

## 7. A full example

```yaml
oaexp: 1
id: cavedog-units
name: Cavedog units
version: "1"
extends:
  - totala
needs:
  - core-contingency
author:
  name: Ridge
  email: ridge@example.com
packaging:
  revision: 1
  date: "2026-10-13"
  packager: Core Prime
```

The archive also holds each file the built-in table names for
`cavedog-units`, each at that file's size and SHA-256. A catalogue entry
for the same package names `kind`, `id`, `version`, `file`, `sha256`,
`size` and `extends`, and the file's last segment is the digest plus
`.oaexp`.

The built-in expansion `boneyards` ("The Boneyards") extends `totala` and
needs nothing else. It holds `by.ccx` and the game-data files of the
install's `Boneyards/` folder: the help pages, the profiles and ladders,
the images and the map. A copy writes them under
`Expansions/totala/boneyards/`, keeping those folders, so the archive is
`Expansions/totala/boneyards/by.ccx`. An install that already holds those
files, including the known `by.ccx`, counts as having the expansion, the
way a Core Contingency install counts as having that expansion's archives.
The beta agreement, programs and libraries are not part of it.
