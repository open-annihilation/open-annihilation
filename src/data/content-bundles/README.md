# Base games and expansions

`oa-data-content-bundles` is the built-in description of every base game and
expansion. Copying, checking, mounting and the Library read this one table:
an id, the English name, whether it is a base game or an expansion, the
base games an expansion extends, the expansions it needs, and each file's
copy name, the source names it may have, its size and its known SHA-256
values. The table holds names, sizes and hashes only.

The demo's installer is still recognised where it is today
(`demo_1997` in `oa/app/demo_installer.hpp`), by its size and SHA-256. The
archive it carries is copied into `Games/totala-demo` with that bundle's
record. This table names the unpacked archive, `TADemo.hpi`, with the same
size and SHA-256 that recognition checks. It does not repeat the installer's
size, hash or resource.

## Entry points

`oa/data/content_bundles.hpp` (namespace `oa::data::content_bundles`):

- `bundles` and `find_bundle` read the table.
- `requirement_closure` is what turning a set on needs, including itself.
  `removal_closure` is what turning a set off removes, including itself.
- `after_turning_on` and `after_turning_off` apply that to a set that is
  already on. Turning one on also turns on an expansion whose only missing
  need was that one. Turning one off turns off everything that needs it.
- `match_file` finds a file by a source path and its size, without regard
  to ASCII case. `known_hash` checks a SHA-256 against the file's known
  values.
- `oa/data/content_bundles/content_needs.hpp` reads `supportedGames` and
  `requiredExpansions` from the three package manifests. An id is a
  built-in base game or expansion. An omitted `supportedGames` means
  `totala` for a mod or a map pack, and every base game for a language
  pack, unless `requires.base` is `ta-3.1c`, which then reads as `[totala]`.
- `oa/data/content_bundles/content_choice.hpp` is the player's choice. `present_bundles`
  says which bundles a set of files contains. `resolve_content_choice`
  reads a stored base game and expansion list, or `--content`.
  `turn_content_expansion` and `choose_content_base_game` apply the on/off
  rules. `parse_content_argument` reads `GAME[:EXPANSION,...]`.
- `installers`, `find_installer` and `known_installer_hash` are the three
  Commander Pack installers, recognised by size and SHA-256.
- `oa/data/content_bundles/record.hpp` reads and writes a bundle folder's
  `.oa-content` record. `parse_content_record` and `format_content_record`
  take bytes and return bytes. `read_bundle_folder_record` reads the record
  in one bundle folder. A field the reader does not know is ignored. A
  file the source did not hold is marked `not-in-source`, and one it held
  at another size also keeps that size, `source-size`.
  `files_not_recorded` lists a bundle's files its record does not name yet,
  and `record_complete` says whether it names them all, copied or not in
  the source: only such a bundle is played.
- `oa/data/content_bundles/expansion_package.hpp` reads `oaexp.yaml` and
  checks that the files offered with it are the expansion's files, by
  name, size and SHA-256. `expansion_fixture.hpp` is the small table the
  tests install against. The game does not.
- `table_fault` is empty when every dependency names a real bundle and
  following the needs reaches no cycle.

## State

None. The table is fixed for the process. Pointers from `find_bundle`,
`match_file` and `find_installer` address it.

## Invariants

- The ids are `totala`, `totala-demo`, `campaign`, `core-contingency`,
  `battle-tactics`, `music`, `cavedog-units`, `cavedog-tilesets` and
  `boneyards`.
- Campaign, music, the tilesets and `boneyards` extend `totala` and need no
  expansion. Core Contingency and Battle Tactics need the campaign. Cavedog
  units need Core Contingency. Nothing extends the demo.
- A file's copy name, or a source name, plus its size, belongs to one file.
- Results that list bundles follow the table's order.
- The installer archive `totala3.hpi` and the sample `Example.ufo` are not
  files of any bundle.

## Tests

`data-content-bundles` looks bundles up, follows both closures, turns a
bundle on and off from a set, matches a file by name and case and size,
checks a known hash and a different one, and checks that the table's own
dependencies hold. The demo archive's size and hash are the ones
`demo_1997` checks. The `boneyards` file list is compared with names, sizes
and hash prefixes held in the test, and a file of that archive's name at
another size does not count as the expansion.

`data-content-record` writes a record and reads it back, refuses a record
it does not understand, keeps a record that carries a field it skips, and
tells a record that names every file of its bundle, in any case, from one
a stopped copy left.

`data-content-choice` turns an expansion on and off, chooses a base game,
clears a mod whose required expansion went off, reads a player with no
stored base game as totala with every copied expansion that can be on, and
reads `--content`.

## Limitations

A file whose hash is not one listed here is still a copy of that file when
its name and size match; callers decide what to say about it. The table
does not say where a bundle is installed, and it does not read an
installer. Music is an expansion like the others; callers that compare
rules leave it out themselves. The choice is not mounted here, and the
note token `required-expansion` is not the sentence the player reads.
