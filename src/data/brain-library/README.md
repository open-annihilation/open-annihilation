# data/brain-library

`oa-data-brain-library` lists the brains installed in the player's Brains
folder. The Library and the game share one description of what is installed:
each brain's manifest, the SHA-256, registry, catalogue id and release its
origin record holds when that record reads, and the sizes of the network
files that manifest names.

## Entry points

`oa/data/brain_library/library.hpp` (namespace `oa::data::brain_library`):

- `brains_folder_name` is `Brains`, the folder of the player's own folder
  that holds installed brains.
- `list_installed` reads that folder. A missing folder is an empty listing.
  Each real directory is read for `oabrain.yaml`, at most 256 KiB, with P01's
  reader, and for `.oa-origin.yaml`, at most 4,096 bytes, with the strict
  YAML reader. The result is sorted by id. `network_bytes` is the sizes of
  the network files the manifest names that exist, added together.
- `find` returns the listed brain of an id, or null.

A folder with no readable manifest, or a manifest with problems, is left
out. `problems` names it: `oabrain.yaml: it cannot be read`, `oabrain.yaml:
it is larger than 256 KiB`, or P01's `describe` line after the folder's name.
Two folders with one id keep the folder whose name is the id, or else the
first by name, and `problems` names the other.

## State

None. Callers pass a folder and get a `Listing` back. Nothing here is
installed for the rest of the process.

## Invariants

- A folder whose name starts with `.` is the installer's own (staging, old,
  discard, `.backup`) and is skipped. Nothing inside a brain is walked,
  so a `.backup` is never a second brain.
- A symbolic link is not followed. A plain file at the top of Brains is
  neither listed nor opened.
- `oabrain.yaml` and `.oa-origin.yaml` are the only files opened, matched
  without ASCII case. A script, a network and a table are never read. A
  network file is measured with its size only when it is a regular file
  reached without leaving the brain's folder.
- More than 256 KiB of a manifest, or more than 4,096 bytes of an origin
  record, is not given to the YAML reader.
- An origin record that does not read leaves the brain listed and its
  origin facts empty. A file origin keeps the SHA-256 only.
- A network file the manifest names that is missing, a link, or outside the
  folder adds nothing to `network_bytes`.

## Tests

`data-brain-library` lists a scratch Brains folder: six good brains, a
broken manifest, a folder with none, an oversized manifest, the installer's
staging folder, a `.backup` holding another id, a symbolic link where the
system makes one, two folders with one id, a plain file left untouched, a
catalogue origin, a file origin, a damaged record and an oversized record,
the order and the problems, `find`, a missing folder, the network sizes of
a brain with two networks, Lantern Scout's 12,345-byte network, and a brain
whose network file is missing.

`data-brain-library-seats` reads and writes a setup's seats: a round trip,
the exact bytes of two seats, members out of order, an unknown member left
out of the reading, each broken rule refused, and `default_level` for each
difficulty over Steady Commander's three levels, Ridge Raider's one level
and a brain with none.

## Seats

`oa/data/brain_library/seats.hpp` is the record a setup remembers for its
computer seats, and the one text that record is stored as.

- `Seat` is one place, 0 to 9. An empty brain is the built-in computer
  player. The version and the package SHA-256 stay empty while the setup
  holds the seat and are filled when a game is saved. The level is a
  brain's level id, or `easy`, `medium` or `hard` for the built-in player,
  and empty when the seat takes the setup's own difficulty. Options stay in
  the brain's order, each value in that option's canonical text.
- `seats_text` writes compact JSON. Members are in byte order and seats are
  in order of place. An empty member is left out, and `place` is always
  written: `{"seats":[{"brain":"steady-commander","level":"firm","options":{"aggression":"high"},"place":1}]}`.
- `parse_seats` reads that shape in any member order, ignores a member it
  does not know, and returns nothing when a rule is broken. It accepts at
  most ten seats, distinct places 0 to 9, a kebab-case brain and level of
  at most 64 bytes, a version of at most 32 bytes, a package SHA-256 of 64
  lower-case hex digits, and at most 16 options whose ids are kebab-case of
  at most 64 bytes and whose values are at most 64 bytes. The whole text is
  at most 16 KiB. It does not throw, and it does not trust a count.
- `default_level` is the first level of a difficulty, or the last level
  when the brain has none of that difficulty, or 0 when it has no levels.

## Limitations

Whether a brain is chosen for a seat is not known here. The manifest is not
checked against this build's engine, and no script, network or table is
parsed or run. A network's SHA-256 is not checked. The origin record's
registry id and key order are the installer's rules; a record that is
strict YAML of the known keys is read, and any other record contributes no
facts.
