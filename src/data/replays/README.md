# data/replays

`oa-data-replays` lists the player's replays for the Library: every
`.oademo` and `.tad` below the Recordings folder of the player's Open
Annihilation folder, each read for what its description or its header says.
It keeps what it read in an index, `Recordings/.replays/index.json`, so that a
later scan opens only the files that are new or have changed. The index is a
cache: a folder whose index is gone, damaged or of another version is read
again and gives the same list, and nothing is lost. The replays the player
keeps are not in the index: they are marked in `Recordings/.replays/kept.json`,
by each recording's identity, so that deleting or damaging the index never
forgets one.

Recordings holds `<mod id>/<file>.oademo` or `.tad`, `default/` for games
without a mod, `Imported/` for replays opened from outside, and
`.replays/`, which holds the index, the kept marks and, under `partial/`,
the games being recorded. A game being recorded is never listed.

## Entry points

`oa/data/replays/replays.hpp` (namespace `oa::data::replays`):

- `list_folder` lists the replay files without opening them: their paths
  below Recordings, '/' between folders, in UTF-8, their sizes, modification
  times and readers, sorted by path in byte order.
- `read_file` opens one file and reads it. `read_replay` reads bytes already
  in memory. A `.oademo` gives its description through
  `oa::formats::oademo::read_description`, without its recording being read
  or decompressed: the map, the kind of game and how it was recorded, when,
  by which program, its length and how it ended, whether it was finished
  after a crash, the mod, expansions and mutators, the planet and the sector
  an extension's kind of game names, and the players. What a `.oademo`
  needs is taken from its manifest: each package, and the engine it names.
  The `.oa-content` note beside it is not read. A `.tad` gives its
  map, its players (colour, side as `arm`, `core` or the number, and
  `human`), the recorder-version sector's text, the time of its last packet
  as its length, and the SHA-256 of its whole file as its identity. Its kind
  is `multiplayer`. What it needs is taken from its mutators sector, its map
  and the `.oa-content` note beside the file.
- `compare_needs` (`content.hpp`) compares those needs with the engine, the
  mods, the maps, the mutators and the expansions the caller says are
  installed. A digest the caller has found in a catalogue or a releases list
  can be fetched. The same mod in another release is reported and is not
  fetched. A digest the caller has looked for and not found is not listed,
  or offline, as the caller says.
- `scan` lists the folder and reads each file that the previous index does
  not hold with the same size, modification time and reader. A file it
  holds so is taken from it unopened. `ScanHooks` tells of each replay as it
  is done (`done` of `total`), and stops the scan when asked.
- `index_text` and `parse_index` write and read the index's JSON, and
  `read_index` (or `load_index`, which gives the index alone) and
  `save_index` read and replace `index.json`. `index_path` says where it is.
  `read_index` also says whether the file was there and damaged, and
  `set_aside` moves such a file to `index.json.damaged`, and when that name
  is taken to `index.json.damaged-2` and on, so that no earlier copy is
  overwritten. It dates the copy now, so its time says when it was set
  aside.
- `KeptMarks` are the replays the player keeps, each by the recording's
  SHA-256 (`Replay::sha256`) with its path as a hint, or by its path alone
  when the replay's file did not read and has no identity. `is_kept` is
  true when a mark has the identity or a mark names the path. `set_kept`
  changes them. `apply_kept` sets `Replay::kept` from them. A file a mark
  names by path stays kept even when another file still has the identity,
  and gains a mark of its new identity, or of its path alone when it no
  longer reads. A path whose file is gone moves to a file that still has
  the identity. `release_kept` drops a mark when a replay is deleted, or
  points it at another listed file that still has the identity.
  `salvage_kept` reads every whole mark object left in a damaged file, and
  `spared_by_kept_bytes` is true when a replay's identity or path occurs in
  those bytes. `kept_set_aside_at` says when the newest `kept.json.damaged*`
  copy was set aside, and `spared_by_set_aside` is true for a replay recorded
  by then: one whose mark may have been cut off with the damage. `kept_text` and `parse_kept` write and read the JSON, and
  `read_kept` and `save_kept` read and replace `kept.json` the way the index
  is. A replay the player exported is marked the same way in `exported.json`
  (`set_exported`, `apply_exported`, `read_exported`, `save_exported`).
- `find` finds a replay of an index by its path. `damaged`, `imported` and
  `file_kind_of` answer the Library's small questions.
- `import_replay` (`oa/data/replays/import.hpp`) copies a `.oademo` or a
  `.tad` opened from outside into `Recordings/Imported`, or names the replay
  already under `Recordings` with the same identity. A `.tad` is written
  with sector 6 left out and nothing else changed. A `.oademo` is recognised
  and then read, and a refusal is that read's sentence. One whose recording
  holds sector 6 is rewritten without it. A `.tad` that is not a TA Demo
  recording, or whose version this game does not play, is refused with the
  sentence that says so. It never throws.
- `register_kind` adds a kind of game an extension records, with the English
  title the Library looks up and shows; `registered_kinds` lists them.
- `day_number` counts the days from 1970-01-01 to a date, and `group_by_day`
  gives the runs of a list that fall on one day, for the list's headings.

The sentences a damaged file carries are the container's
(`oa/formats/oademo/interface_words.hpp`) for a `.oademo`, and this
module's (`oa/data/replays/interface_words.hpp`) otherwise: "This recording
cannot be read." for a `.tad` that does not parse, "This file cannot be
read." for a file that does not open, and "This file is too large to be a
replay." for one past the size cap.

## State

The kinds extensions register are one list for the process, guarded by a
lock, in the order each id was first registered. An extension registers its
kinds as it starts; `forget_kinds` empties the list for tests.

The index file is a cache of what the files say, and nothing else. Nothing is
kept between scans: callers pass the folder and the last index, and get the
new one back. A scan does not save the index, and does not mark any replay
kept; the caller does, with `apply_kept`. The same is true of `exported.json`.

`kept.json` holds the only record of what the player keeps, and
`exported.json` of what the player exported. A mark outlives
its file: nothing here removes a mark for a replay the folder no longer
holds, so a folder that is briefly missing (an unplugged drive) forgets
nothing. Deleting a replay removes its mark only when no other listed file
has that recording.

## Invariants

- A file or folder whose name starts with `.` is skipped, so `.replays` and
  the games being recorded in it are never listed. Only names ending
  `.oademo` or `.tad`, without ASCII case, are listed, so a content note
  (`.oa-content`) is not.
- At most three folders below Recordings are walked.
- A symbolic link is neither followed nor listed. Recordings itself may be
  one.
- An error of the file system never throws: the entry is skipped, a folder
  that cannot be listed is left, and a file that cannot be read is listed
  damaged.
- A damaged file is listed with `damage` set and its time as its recorded
  time, and never stops the scan. Bytes that do not read never throw.
- A file larger than 512 MiB is listed damaged and never read. An index
  larger than 64 MiB is not read.
- `read_file` reads only below Recordings: a path with an empty part, `.`,
  `..`, a drive or a root is damaged, unread.
- A name or a sector of a `.tad` that is not UTF-8 is read as
  Windows-1252, up to its first NUL; the five bytes Windows-1252 leaves
  undefined stand for the characters of the same value.
- `index.json` and `kept.json` are each replaced by writing a temporary file
  beside it and renaming it over (`oa::platform::replace_file`), so a crash
  part-way leaves the old file whole. The same index, or the same marks,
  always give the same text.
- A file that is there and does not read is damaged, which a missing file is
  not. The caller sets a damaged file aside (`set_aside`) before it saves, so
  that the damage is kept for the player to look at, never overwritten.
  Each copy is kept: `name.damaged`, then `name.damaged-2` and on.
- While a `kept.json.damaged*` copy is there, the retention rule leaves
  every replay recorded by the second the newest copy was set aside
  (`spared_by_set_aside`), and nobody is asked again: a mark cut off with
  the damage cannot be known. A copy whose time cannot be read spares every
  replay.
- Two files of one recording are one replay: keeping one keeps both, and one
  mark holds them.
- An index entry that has no path, or a known member of the wrong type, is
  dropped and its file read again by the next scan.

## Tests

`data-replays` builds made-up recordings in a scratch folder; it reads no
game data. A scan of a Recordings folder holding a replay with a mod and an
extension's kind object, a `.tad`, an imported replay, a game being
recorded, a hidden folder, a content note, a link and a replay four folders
down lists exactly the three replays, sorted, with their sizes, readers and
every member they read; the hooks see each replay done of three; a scan
told to stop returns what it did. The index's text reads back to the same
replays, member for member, is the same text again, and survives a save and
a load; unknown members are ignored and bad entries dropped; text that is
not JSON, of another version or of the wrong shape is refused. A second
scan takes an unchanged file from the index unopened, even after its bytes
were replaced at the same size and time; with the index deleted the folder is
read again into the same replays. The kept marks are written, read back and
read again after the index is deleted, truncated or filled with noise, and
every replay is still kept; a damaged index is set aside and a save leaves it
there; a replay that was moved or renamed stays kept and its mark's path
follows; a damaged replay is kept by its path; two copies of a recording are
one mark; a kept file that is damaged is told apart from a missing one; a
kept file set aside is dated as it is set aside, the newest of several
copies counts, and a replay recorded by then is spared, a later one not. A
`.tad`'s Windows-1252 names come out in UTF-8, with its recorder's text,
length and digest. A cut replay, a zip that is no replay, noise named `.tad`
and empty files are listed with their sentences while the good file still
reads, and nothing throws; a missing file and paths that leave Recordings
read as files that cannot be read. Kinds register by id, a later one
replacing the first in its place. Day numbers of known dates, and the groups
of a list by day. An exported mark keeps a replay past the span, and a replay
of the same age with no mark still goes. A file that is not those marks is
damaged, and reading it leaves the bytes. An import copies a `.oademo` and a
`.tad` into `Imported`, writes a `.tad` with sector 6 left out and nothing
else changed, selects the same identity again without copying, and refuses a
file that is not a TA Demo recording and one whose version this game does not
play.

## Limitations

A `.tad`'s players come from its player chunks, not from the status
datagrams playback binds: each is a human, with the side its chunk records
and no team. A `.tad` names no kind of game, no mod and no time of
its own beyond its file's. A mark of a replay whose file is gone stays in
`kept.json` until the Library deletes the replay (a later ticket's). A file
rewritten within the same second at the same size is taken from the index
unread. The index holds no results of the replay index pass
(`timeline.json`) yet.
