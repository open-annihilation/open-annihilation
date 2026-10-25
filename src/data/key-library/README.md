# data/key-library

`oa-data-key-library` lists the key layouts installed in the player's Keys
folder, and writes the player's own changes for one layout whole or not at
all. Settings, the editor and the Library share one description of what is
installed: each layout's manifest, and whether its origin record says the
folder was made by hand, came from a catalogue, or holds the player's own
changes. It also reads the catalogue facts a listing shows, and packs a
folder into a `.oakeys` whose bytes are the same on every machine. Listing
never opens a package zip. The packer writes one.

## Entry points

`oa/data/key_library/library.hpp` (namespace `oa::data::key_library`):

- `keys_folder_name` is `Keys`, the folder of the player's own folder that
  holds installed layouts.
- `own_folder_name` is `own-` and a layout id, the folder of that layout's
  own changes.
- `list_installed` reads a Keys folder. A missing folder is an empty
  listing. Each real directory is read for `oakeys.yaml`, at most 256 KiB,
  and for `.oa-origin.yaml`, at most 4,096 bytes, with the strict YAML
  reader. The result is sorted by id. A catalogue record fills
  `package_sha256`, `registry`, `catalogue_id` and `release`. A file record
  keeps the SHA-256 only. An own layout, a damaged record and a missing
  record leave those facts empty.
- `find` returns the listed layout of an id, or null.
- `write_own` creates `Keys/own-<base id>/` and replaces `oakeys.yaml` and
  the origin record `origin: own` by writing temporary names in that folder
  and renaming them over the old files.
- `file_role` says what one name in a package is: `oakeys.yaml` (without
  ASCII case), the icon named exactly, a top-level name ending `.md` or
  `.txt` as written, or outside. A name that holds a slash is outside.
- `read_folder` reads the manifest, the icon it names and the top-level
  notes. A name that starts with `.` is skipped. Any other file, and any
  subdirectory, is a problem.
- `package_bytes` writes the archive: `oakeys.yaml` first, then the other
  files in byte order of name. A name ending `.png` is stored. Every other
  file is deflated. A name `file_role` refuses is a problem. The same files
  give the same bytes.
- `write_package` writes those bytes to `<out>.part` and renames that over
  `out`.

A folder with no readable manifest, or a manifest with problems, is left
out. `problems` names it: `oakeys.yaml: it cannot be read`, `oakeys.yaml:
it is larger than 256 KiB`, or the manifest's `describe` line after the
folder's name. Two folders with one id keep the folder whose name is the
id, or else the first by name, and `problems` names the other.

## State

None. Callers pass a folder and get a `Listing` back. Nothing here is
installed for the rest of the process.

## Invariants

- A folder whose name starts with `.` is skipped. Nothing inside a layout
  is walked.
- A symbolic link is not followed. A plain file at the top of Keys is
  neither listed nor opened.
- `oakeys.yaml` and `.oa-origin.yaml` are the only files opened, matched
  without ASCII case.
- More than 256 KiB of a manifest, or more than 4,096 bytes of an origin
  record, is not given to the YAML reader.
- An origin record that does not read leaves the layout listed with origin
  `file` and empty catalogue facts. `origin: own` is the player's own
  changes, and `origin: catalogue` is a catalogue install. A file origin
  keeps `package_sha256` and leaves `registry`, `catalogue_id` and
  `release` empty.
- `write_own` refuses an empty base id, `..`, and any base id that holds a
  path separator. It writes only inside `Keys/own-<base id>/`, and it does
  not follow a symbolic link. A crash mid-write leaves the previous file
  whole: the new bytes are renamed over it, never written in place.

## Tests

`data-key-library` lists a scratch Keys folder: two installed layouts, an
own layout, a broken manifest, an oversized manifest, a `.staging` folder,
a symbolic link where the system makes one, two folders with one id, and a
missing folder. The catalogue facts of a catalogue record, a file record
and a damaged record are checked. `write_own` makes the folder, replaces a
file whole and leaves no temporary file. `package_bytes` of the same files
twice is the same bytes, with `oakeys.yaml` first, the PNG stored and a
note deflated, and `extra.lua` refused. A package `write_package` writes
opens through the installer and plans an install.

## Limitations

Whether a layout is the one in use is not known here. The manifest is not
checked against this build's chords or actions: the installer does that.
Listing never opens a package zip. The packer writes one, and it does not
open one to list it. The origin record is read when it is strict YAML of
the known keys; any other record contributes no origin and no catalogue
facts, and the layout stays listed.
