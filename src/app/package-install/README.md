# Packages

This module installs packages into a folder of the player's own folder. One
table of kinds says what differs; it holds the mod kind, `oamod`, the
map pack kind, `oamap`, the language pack kind, `oalang`, the expansion
kind, `expansion`, the mutator kind, `oamut`, the brain kind,
`oabrain`, and the key layout kind, `oakeys`. A mod package is a zip archive of a mod's folder, the one that
holds its `oamod.yaml`; it installs into `Mods/<id>`, by the id its profile
gives, whatever the file is called. A map pack installs into `Maps/<id>`.
A language pack installs into `Languages/<tag>`, by the tag its
`language.yaml` gives. An expansion installs into `Expansions/<game>/<id>`
and is copied by `install_oaexp`, which writes the table's files and a
`.oa-content` record. A mutator installs into `Mutators/<id>`. A brain
installs into `Brains/<id>`. A key layout installs into `Keys/<id>`. The module has no SDL: the game's runtime
shows what it asks
and answers (`src/app/runtime_mod_install.cpp`,
"Mod packages" in [src/app](../README.md#mod-packages-oamod) and
"Map packs" in [src/app](../README.md#map-packs-oamap)).

## Entry points

`oa/app/package_install.hpp`, namespace `oa::app::package_install`:

- `open_package` reads a package and checks it before anything is written:
  its directory through the streamed zip reader
  (`oa/formats/zip/stream.hpp`), where its `oamod.yaml` lies, every name it
  unpacks, its sizes, and its profile, resolved twice as a load of the mod
  resolves it (the second time with its packaged INI file, or the game
  folder's, and the registry values).
- `plan_install`, the oamod kind's plan, decides, from what each folder of
  Mods holds (`FolderHooks`), where a package goes and what it asks.
- `Unpacking` unpacks a package's files into a staging folder inside Mods,
  a budget of bytes a step. It first hashes the package file, a piece at a
  time, each byte of the budget, and the prompt reads `Checking {file}...`.
  Then its folders, its files, the origin record, and a sync of each folder
  and, on macOS, one flush of the drive's cache to storage. Each folder or
  file made and each folder synced costs `unpack_entry_cost` (16 KiB) of
  the budget, and a step ends after about 4 ms, so that a package of many
  small files never holds a frame long.
- `commit_change` puts a change in place with renames on one volume.
- `uninstall_package` removes a package: its folder, and the `.backup`
  inside it, moved aside whole in one rename for the `Discarder`.
- `recover_changes` settles what a stop left, and `Discarder` deletes the
  folders a change drops a little at a time.
- `read_installed_mod` and `read_backup`, the oamod kind's, read what a
  folder and its `.backup` hold, for the plan and the Library's Roll back.

`oa/app/package_install/inbox.hpp` keeps what outlives each run of the game in
one process: the packages waiting for the main menu (`post_package_file`,
`take_package_file`, `next_package_file`), the `.oareg` files waiting on the add-registry queue
(`post_registry_file`, `take_registry_file`), `post_opened_file`, which sends
a `.oareg` file there and every other opened file to the package queue, the
change that waits for the run playing its target to end
(`set_pending_change`, `pending_change_waiting`, `finish_pending_change`,
`discard_pending_change`)
and its outcome, and the discard folders the start's recovery found.
`pending_change_target` names the folder the waiting change acts on, so
that the Library can say an update waits until the player leaves the mod
being played. `main()` calls `finish_pending_change` after every run: after
a soft restart, before the next run reads the folder, and on the way out
when the player quits, so that a change held for the mod being played is
made then rather than lost. After a crash, the next start's
`recover_changes` settles the staged files away and the folder keeps what
it held. `discard_pending_change` drops a waiting install, replace or
reinstall the same way, without putting it in place and without reporting
it, and leaves a waiting roll back. A package's outcome is reported once (`report_package_outcome`
drops a second report of the same package): the game reports a mod held
from the Library as installed when it holds it, and the report the change
makes when it is put in place then reaches no one.

`oa/app/package_install/prompts.hpp` builds every prompt the install shows
(`oa::ui::engine_settings::Prompt`): its text through the interface
catalogue, its buttons and what each answers.

`oa/app/package_install/handoff.hpp` has the instance lock and the hand-off
folder a second start writes its files into for the copy already
running. The folder's name stays `opened-mods`.

## Kinds

`kKinds` in `src/kinds.cpp` is the table. It holds `oamod`, then `oamap`,
then `oalang`, then `expansion`, then `oamut`, then `oabrain`, then
`oakeys`. A mod uses the extension `.oamod`, the manifest `oamod.yaml`
(at most 256 KiB), the root folder `Mods` and the prefix `.oamod-` on the
installer's own folders and on the lock. A map pack uses `.oamap`,
`oamap.yaml` (the same limit), the root folder `Maps` and the prefix
`.oamap-`. A language pack uses `.oalang`, `language.yaml` (the same limit),
the root folder `Languages` and the prefix `.oalang-`. An expansion uses
`.oaexp`, `oaexp.yaml` (the same limit), the root folder `expansions` and
the prefix `.oaexp-`. Its `plan` refuses the generic unpack. A mutator uses
`.oamut`, `oamut.yaml` (the same limit), the root folder `Mutators` and the
prefix `.oamut-`. A brain uses `.oabrain`, `oabrain.yaml` (the same limit),
the root folder `Brains` and the prefix `.oabrain-`. A key layout uses
`.oakeys`, `oakeys.yaml` (the same limit), the root folder `Keys` and the
prefix `.oakeys-`.
`package_kinds`, `kind_for_file` and `find_kind` read the table; `mod_kind`
is the mod entry. A file whose extension is none of them is refused as
`unknown_kind`, and nothing is read.

Reading the container, unpacking, the renames, recovery, the lock,
discarding, the inbox and the hand-off are the same for every kind. They
take the kind and build the seven folder names from its prefix
(`folder_names`). A kind supplies those names and the hooks: `read_manifest`
fills what the package installs and the kind's own fields, `read_installed`
and `read_backup` say what a folder holds, `plan` decides where it goes,
`check_staged` checks the unpacked files before they are put in place (null
checks nothing) and `prompts` words every prompt. The oamod hooks are
`src/plan_oamod.cpp` and `src/prompts_oamod.cpp`
(`oa/app/package_install/oamod.hpp`): the profile resolved twice, where a
mod goes, and the words a mod install shows. `Package::profile` is that
resolved profile, and null for every other kind.

### Adding a kind

A new kind is wired in five places, and nowhere in the generic files:

- `src/plan_<kind>.cpp`, implementing the hooks (`read_manifest`,
  `read_installed`, `read_backup`, `plan`, and `check_staged` when the
  staged files need a check of their own).
- Its line in `add_library` in this module's `CMakeLists.txt`.
- Its entry in `kKinds` in `src/kinds.cpp`.
- Its branch in each function of `src/app/runtime_package_kinds.cpp`.
  `package_options` is where the runtime hands the hooks their `context`,
  and `package_changed` is where the app takes in what a change put in place.
- Its test `app-package-install-<kind>`.

An expansion is the exception to the generic unpack. Its `plan` refuses
that unpack, and the runtime copies it with `install_oaexp`, which writes
the table's files and the `.oa-content` record.

A kind's own refusals go at the end of `Refusal`, worded by its
`KindPrompts::refusal_text`.

### Map packs (.oamap)

The hooks are `src/plan_oamap.cpp` and `src/prompts_oamap.cpp`
(`oa/app/package_install/oamap.hpp`). The shared reading, unpacking and
renames are the ones above. Before a pack is put in place the kind checks,
and refuses with every problem named:

- `oamap.yaml` reads in package use. Every diagnostic is kept; the prompt
  shows the first three and the log has them all.
- Every file a map lists, and every preview, is in the package. Names match
  ignoring ASCII case.
- Every other file is one a map lists. A file outside `maps`, `features`,
  `anims`, `objects3d` and `previews` is refused as outside those folders,
  and one inside them that no map lists is refused as unused.
- A preview is a PNG of at most 2 MiB and at most 1024 pixels on a side.
- `requires.engine`, when the manifest writes one, is met by this build.
  The range is read with the shared engine-range rules, and the sentence
  names the range and this build's version.
- Every map fits the game's content. `check_staged` calls the `FitHooks`
  that `PackageOptions::context` points to, once per map, with the
  expansions the manifest's `requiredExpansions` names, and the runtime's
  hook builds the store from the base game, the expansions that are on and
  those the pack needs. No hooks, which only a test gives, skips the check.
  The kind does not build that store itself. A clash with a mod is not a
  reason to refuse.

A file the player opened asks "Install the map pack {name} {version}
({n} maps)?" before a new folder is made, and asks to update, reinstall or
replace as a mod does. Nothing is installed alongside. A catalogue package
whose target's origin record names the same registry replaces that folder
with no question and keeps one `.backup`. The same id from another registry
asks to replace, and the question names both registries. A folder that does
not hold this pack's `oamap.yaml` is left as it is. A folder that holds
nothing but empty folders (no file, link or junction, at most 256 folders)
reads as missing: an install goes there, and `commit_change` removes those
folders, deepest first and each only while it is empty, before the staged
files move in. One that holds anything more by then is refused, and the
log says so. When the `Discarder` has deleted a discard folder, it removes
the folder of that discard's package name if it holds nothing but empty
folders, as a file sync can bring them back after an uninstall.

A catalogue map pack also installs when the main menu is not showing, on
any screen except while a match loads or runs, and it shows no prompt. A
plan that needs a question goes back to the inbox and is asked on the
settled main menu. A refusal is reported, with its sentence, and not shown
off that menu. A file the player opened, of any kind, and a package of
another kind, still wait for the settled main menu.

### Language packs (.oalang)

The hooks are `src/plan_oalang.cpp` and `src/prompts_oalang.cpp`
(`oa/app/package_install/oalang.hpp`). The shared reading, unpacking,
renames, lock, `.backup`, recovery and origin record are the ones above.
Before a pack is put in place the kind checks, and refuses with the problem
named:

- `language.yaml` reads. `no_manifest` says it holds no `language.yaml`.
  A manifest the reader refuses keeps that reader's message.
- `requires.engine`, when the manifest writes one, is met by this build.
  The sentence names the range and this build's version.
- Each listed font is an entry `fonts/<file>` of the package. A missing
  font names that entry.
- The warm-up, when named, is in the package, at most 4,096 bytes, and
  UTF-8.
- Each of `translate.tdf`, `missions.tdf`, `pictures.tdf`, `units.tdf` and
  `interface.tdf` that is present is at most 4 MiB and parses. A failure
  names the file and the parse error. A table that is absent is skipped.
- `check_staged` opens each listed font. One that does not open is refused
  after unpacking, the staging is discarded, and the pack already installed
  is left as it was.

A file the player opened asks "Install the language {endonym} ({English
name})?" with Install and Cancel. The same version at a higher revision, or
a folder that is not this pack, asks to replace, and the old folder is kept
as `.backup`. The same version and the same revision, when that revision is
not zero, asks to reinstall. Nothing is installed alongside, and Play now
is not offered.

A catalogue language pack asks nothing and shows no prompt. It installs
while Settings or a notice show, and in a run nobody watches, and it does
not install while a match loads or runs. The same version and revision is
installed again; any other folder already there is replaced and kept as
`.backup`. What became of it is reported, and not shown.

### Mutators (.oamut)

The hooks are `src/plan_oamut.cpp` and `src/prompts_oamut.cpp`
(`oa/app/package_install/oamut.hpp`). The shared reading, unpacking,
renames, lock, `.backup`, recovery and origin record are the ones above.
Nothing in an install reads a script as Lua or runs one. Before a mutator
is put in place the kind checks, in `read_manifest`, and refuses with the
problem named, so a refusal writes nothing:

- `oamut.yaml` reads with the mutator manifest reader. Every diagnostic is
  kept; the prompt shows the first three and the log has them all.
- The entry script (`entry`, default `mutator.lua`) is in the package.
- Every file is `oamut.yaml`, the entry, the icon named by `icon`, a file
  under `lua/` ending `.lua`, a file under `rules/` ending `.sm`, any file
  under `files/`, or a top-level `*.md` or `*.txt` note. A file outside
  that layout is refused.
- No `.lua` file, the entry included, starts with the byte `0x1B`. Each is
  read through `read_entry` with a bound of 1 MiB. A larger script is
  refused. An empty file is source.
- The icon, when named, is in the package, a PNG, square, at most 256
  pixels on a side and at most 256 KiB. Every icon failure shares one
  sentence.
- `requires.engine`, when the manifest writes one, is met by this build.
  The sentence names the range and this build's version.

`check_staged` is null. The revision is always 0. A file the player opened
asks "Install the mutator {name} {version}?" before a new folder is made.
The same version asks to reinstall. Another version asks to update. Nothing
is installed alongside. A catalogue package whose target's origin record
names the same registry replaces that folder with no question and keeps one
`.backup`, before the versions are compared. The same id from another
registry asks to replace, and the question names both registries. A folder
that does not hold this mutator's `oamut.yaml` is left as it is. A folder
that holds nothing but empty folders reads as missing.

The installed prompt says "{name} is installed. Turn it on with the Mutators
button when you set up a game." and offers Open folder and OK. Play now is never
offered: installing a mutator does not turn it on.

A catalogue mutator also installs when the main menu is not showing, on
any screen except while a match loads or runs, and it shows no prompt, as
a catalogue map pack does. A plan that needs a question goes back to the
inbox and is asked on the settled main menu. A file the player opened, of
any kind, still waits for the settled main menu.

The installed mutators are listed by `oa-data-mutator-library` from each
folder's `oamut.yaml` and `.oa-origin.yaml` alone. The game reads that list
on first use and again after every mutator package change.

### Brains (.oabrain)

The hooks are `src/plan_oabrain.cpp` and `src/prompts_oabrain.cpp`
(`oa/app/package_install/oabrain.hpp`). The shared reading, unpacking,
renames, lock, `.backup`, recovery and origin record are the ones above.
Nothing in an install parses a script or runs one. Before anything is
written the kind checks, in `read_manifest`, and refuses with the problem
named:

- `oabrain.yaml` reads with the brain manifest reader. Every diagnostic is
  kept; the prompt shows the first three and the log has them all.
- Each `requires.expansions` id is an expansion this build knows.
- The id can name a folder on Windows.
- Every file is one `file_role` allows, on the name as the package wrote
  it. A file outside that layout is refused.
- The entry script (`entry`, default `brain.lua`) is in the package.
- No `.lua` file, the entry included, starts with the byte `0x1B`. Each is
  read through `read_entry` with a bound of 1 MiB. A larger script is
  refused. An empty file is source.
- The icon, when named, is in the package, a PNG, square, from 1 to 256
  pixels on a side and at most 256 KiB. Every icon failure shares one
  sentence.
- Every network the manifest names is in the package, and those entries'
  sizes together are at most 64 MiB.
- Each data table is at most 4 MiB.
- `requires.engine`, when the manifest writes one, is met by this build.
  The sentence names the range and this build's version.

After unpacking, and before the files are put in place, `check_staged`
reads the staged `oabrain.yaml` and hashes each network in 64 KiB pieces.
A hash that is not the one the manifest names is refused, the staging is
discarded, and a brain already installed is left as it was.

The revision is always 0. A file the player opened asks "Install the brain
{name} {version}?" before a new folder is made. The same version asks to
reinstall. Another version asks to update. Nothing is installed alongside.
A catalogue package whose target's origin record names the same registry
replaces that folder with no question and keeps one `.backup`, before the
versions are compared. The same id from another registry asks to replace,
and the question names both registries. A folder that does not hold this
brain's `oabrain.yaml` is left as it is. A folder that holds nothing but
empty folders reads as missing.

The installed prompt says "{name} is installed. Choose it for a computer
player when you set up a game." and offers Open folder and OK. Play now is
never offered: installing a brain does not seat it.

A catalogue brain also installs when the main menu is not showing, on any
screen except while a match loads or runs, and it shows no prompt, as a
catalogue map pack does. A plan that needs a question goes back to the
inbox and is asked on the settled main menu. A file the player opened, of
any kind, still waits for the settled main menu.

The installed brains are listed by `oa-data-brain-library` from each
folder's `oabrain.yaml` and `.oa-origin.yaml` alone. A network file the
manifest names is measured, not opened. The game reads that list on first
use and again after every brain package change.

### Key layouts (.oakeys)

The hooks are `src/plan_oakeys.cpp` and `src/prompts_oakeys.cpp`
(`oa/app/package_install/oakeys.hpp`). The shared reading, unpacking,
renames, lock, `.backup`, recovery and origin record are the ones above.
Nothing in an install parses a chord itself: the manifest reader and the
layout reader do that. Before anything is written the kind checks, in
`read_manifest`, and refuses with the problem named:

- `oakeys.yaml` reads with the key-layout manifest reader. Every diagnostic
  is kept; the prompt shows the first three and the log has them all.
- An id that starts `own-` is kept for the player's own changes.
- The id can name a folder on Windows.
- Every file is one `file_role` allows, on the name as the package wrote
  it: `oakeys.yaml`, the icon the manifest names, or a top-level `*.md` or
  `*.txt` note. A file outside that layout is refused.
- The icon, when named, is in the package, a PNG, square, from 1 to 256
  pixels on a side and at most 256 KiB. Every icon failure shares one
  sentence.
- `requires.engine`, when the manifest writes one, is met by this build.
  The sentence names the range and this build's version.
- The layout reader then runs with no keyboard map. An error refuses the
  package, with the chord's line and column. A warning is kept: the
  question for a file the player opened lists up to three under "This
  version cannot use:", then "and {n} more".

`check_staged` is null. The revision is always 0. A file the player opened
asks "Install the key layout {name} {version}?" before a new folder is made.
The same version asks to reinstall. Another version asks to update. Nothing
is installed alongside. A catalogue package whose target's origin record
names the same registry replaces that folder with no question and keeps one
`.backup`, before the versions are compared. The same id from another
registry asks to replace, and the question names both registries. A folder
the key library marks as the player's own changes, or one named `own-…`,
is left as it is, and no package may take an id that starts `own-`. A
folder that holds nothing but empty folders reads as missing.

The installed prompt says "{name} is installed. Choose USE in the Library's
Keys tab to play with it." and offers Open folder and OK. Play now is never
offered: installing a key layout does not make it the one in use, and it
never writes that preference.

A catalogue layout also installs when the main menu is not showing, on any
screen except while a match loads or runs, and it shows no prompt, as a
catalogue map pack does. A plan that needs a question goes back to the
inbox and is asked on the settled main menu. A file the player opened, of
any kind, still waits for the settled main menu.

The installed layouts are listed by `oa-data-key-library` from each
folder's `oakeys.yaml` and `.oa-origin.yaml` alone. The game reads the
layout in use again when a package change replaces that folder. It keeps
no separate listing of layouts.

## What a package holds

A zip archive (`oa/formats/zip/stream.hpp` says which) whose top holds
`oamod.yaml`, matched without case, or which holds one folder at its top
that holds it, as the Finder's Compress makes: that folder is stripped.
`__MACOSX` folders, `.DS_Store` files and AppleDouble files (a last part
starting `._`) are ignored, and so is a `.backup` folder of its own, which
is ours. It is refused, before anything is written, for:

- a file that cannot be read, or is not a zip archive; a damaged directory
  or entry;
- no `oamod.yaml` where it should be, one larger than 256 KiB, or one whose
  profile does not resolve (its diagnostics are kept);
- a name that cannot be unpacked on every system (`portable_name_problem`:
  each part 1 to 255 bytes of UTF-8 without control characters or `< > : "
  | ? * \`, not ending in a dot or a space, and not a name Windows keeps for
  a device), two names that differ only in the case of their ASCII
  letters, as the game matches names, or a file and a folder of one name;
- more than 16,384 folders (`max_package_folders`), those its paths pass
  through included;
- a link or a special file, an encrypted entry, or one packed otherwise
  than stored or deflated;
- more than 4 GiB unpacked, or more than 64 MiB unpacked and more than 200
  times its own size;
- an id that names a device on Windows.

The names are kept as a tree of their parts, each part once under its
folder, so reading a package takes memory that grows with the parts it
names, never with the length of its paths.

Unpacking also refuses too little room on the disk (the files and 64 MiB
more), an unpacked path longer than the system opens less 16 characters,
and a file or folder that exists already when it is made, as a name the
file system folds to another's does: every file and folder is made anew,
never over another.

## Where a package goes

The plan looks at `Mods/<id>` (A), then at the folders Install alongside
makes, `Mods/<id>-<version>` and `-2` to `-99` (B), the version made
folder-safe (`version_folder_part`). The first row that applies wins:

| A | B | Plan |
|---|---|---|
| this mod, the same version, another revision | any | UPDATE MOD on A |
| this mod, the same version and revision | any | ALREADY INSTALLED on A |
| not as above | one holds this mod at the same version, another revision | UPDATE MOD on it |
| not as above | one holds this mod at the same version and revision | ALREADY INSTALLED on it |
| missing | | installed into A with no question |
| this mod, another version | | ANOTHER VERSION: replace A, or install alongside in the first free B |
| anything else | | FOLDER IN USE: install alongside in the first free B; A is never replaced |
| either of the last two with no free B | | refused |

A revision that cannot be read counts as another.

## Changes

Every change is a sequence of renames on the volume Mods is on, each
refusing a name that exists (`rename_exclusively`) and tried again for up to
ten seconds while another program, such as a file sync or a virus scanner,
holds the files. The folders it uses lie in Mods, named after the target
`T`: `.oamod-staging-T` (S), `.oamod-old-T` (O), `.oamod-replaced-T` (X),
`.oamod-restore-T` (R) and `.oamod-discard-T-n` (D).

| Change | Renames |
|---|---|
| install | S to T |
| replace | T to O; S to T; O's own `.backup` to D; O to `T/.backup` |
| reinstall | T to X; S to T; X's `.backup` to `T/.backup`; X to D |
| roll back | `T/.backup` to R; T to O; R to T; O to `T/.backup` |

Every rename moves a folder whole, so the origin record inside it goes with
the folder. A roll back writes no record of its own.

An uninstall (`uninstall_package`) is one rename, T to D, and nothing else:
the package's folder, with its `.backup` and origin record, leaves Mods
whole, and the `Discarder` deletes D. It takes only a target that is one
plain folder of the root (no separator, colon or control character, not
`.`, `..` or a name starting with `.`), and refuses, removing nothing, while
another copy of the game holds the lock (busy), a target that is a link or
junction (link), which is never followed, one that is missing, holds no
package of the kind or not what the Library listed (changed), and while S,
O, X or R of the target lies in Mods (busy), since an unfinished change or
recovery would bring the package back. A refused rename leaves T where it
was (not_placed). After the rename the root is synced and, on macOS, the
drive's cache flushed to storage. A stop at any point leaves T whole, or
only D, whose hidden name nothing reads as a package, and which the next
start's recovery deletes like any other D.

So three installs keep one version back: after revisions 1, 2 and 3 the
folder holds 3, its `.backup` 2, and 1 is deleted. A roll back swaps the
two, so a second undoes the first. A failure before the new files are in
place undoes what was done; after it, the rest is tried once more, and what
it could not settle is renamed to a visible `<T>-left-over` folder.

Before a change, the target must still hold what the plan found, or it is
refused as changed; a roll back is refused as changed when `T/.backup` is
a link or junction. Once the files are in place, T and Mods are synced, and
on macOS the drive's cache is flushed to storage, before the player is
told. A copy of the game holds the lock on `Mods/.oamod-lock`
while it unpacks or changes Mods, so that another copy's recovery never
touches its folders; that copy's installs are refused as busy meanwhile.

Recovery runs once at every start before the mod folder is resolved, and
again after a change that waited for its run. For each target it finds
folders of: X with T missing, X goes back; X with T, X's `.backup` moves
over and X goes. O with T missing, O goes back; O with T, O's own `.backup`
goes and O becomes `T/.backup`. R with T and no `T/.backup`, R becomes it;
R with T missing, R becomes T. S goes. Every crash point of every change
settles so to the state before it or after it. What it cannot settle
becomes `<T>-left-over`. Every D it finds is deleted, whatever it is.

The `Discarder` removes at most 64 entries, or works 4 ms, a step. Links and
junctions, a D that is one included, are removed, never entered, so that a
`.backup` or staging folder someone made a link to a folder outside Mods
drops only the link; read-only files are made writable first.

A link (`is_link_or_junction`) is a symbolic link, or on Windows a reparse
point whose tag is a name surrogate (`reparse_tag_is_link`): a junction,
mount point or symbolic link, which names another place. A folder or file
with any other tag is itself, such as one a file sync keeps in the cloud,
as the sync of Documents does on many Windows machines: the plan reads it
as a mod, recovery settles it, the `Discarder` enters it and Roll back
takes it. A reparse point whose tag cannot be read counts as a link.

## Origin records

An install writes `.oa-origin.yaml` into the staging folder after the
package's last file and before the folders are synced, so the rename that
puts the folder in place carries the record with it. The record says
whether the package came from a file or one release of a registry's
catalogue, and the SHA-256 of the package file, which the unpacking
computes. A folder with no record, such as one installed by an earlier
version or made by hand, is not an error and is never matched to a release.

```yaml
# Written by Open Annihilation when it installed this folder; not part of the package.
oa-origin: 1
origin: catalogue
registry: coreprime
catalogue-id: example-mod
release: 27
sha256: <64 lowercase hex digits>
installed: 2026-11-02
```

A file the player opened is `origin: file` and omits `registry`,
`catalogue-id` and `release`; it still holds the file's SHA-256. When the
origin brings a SHA-256 and the package file's differs, the install stops
as damaged and the target is left as it was. A package's own
`.oa-origin.yaml` at its top is left out, as its `.backup` is.

A replace keeps the old record in `.backup`. A reinstall replaces the
target's record and leaves the kept version's. A roll back swaps the two
folders, records included, and writes nothing. Recovery moves folders
whole and never edits a record. Replacing the record of a folder that
already holds a package refuses while the root folder's lock is held, and
a replace that fails leaves the previous record as it was.

## Tests

- `app-mod-install-package`: a Finder-made package, a package with its
  profile at its top and a `.backup` of its own, a packaged INI file the
  second resolution reads, the folders a package makes, each refusal of a
  package once (more folders than a mod may take as paths of 254 folders
  each), the name rule and the folder-safe version.
- `app-mod-install-plan`: each row of the table over a Mods folder kept in a
  map, and each prompt's title, buttons, first mark and sentences, each
  fitting its height with a long path.
- `app-mod-install-change`: an install, three replaces, a reinstall, two roll
  backs and an install alongside in a scratch Mods folder; a rename refused
  at each step of each change; every crash point settled; the folders
  recovery cannot settle; the retries and waits; a rename never replacing;
  links in a discard folder; a `.backup` linked to a folder outside Mods,
  dropped by a replace and refused by a roll back, and a discard folder
  that is such a link, each leaving that folder whole; the Windows reparse
  tags taken as links, as values, so that it runs everywhere; files made
  anew only; a target that changed; the lock; the room and path checks;
  the files a step makes held to its budget.
- `app-package-install-uninstall`: a mod and its `.backup` moved aside
  whole and deleted a step at a time, a later install finding the folder
  free, and the next discard name taken; links never followed, as the
  package's folder (refused), its `.backup` and a file inside it, what each
  names left whole; names that are not one plain folder refused; a folder
  of the player's own, a file, a missing folder and a changed one refused;
  each unfinished change's folder in the way and the lock another copy
  holds refused; a refused rename leaving the mod and its `.backup` where
  they were; and a deletion stopped before its first step and after one,
  leaving only D, which a plan never takes for the mod and recovery
  deletes.
- `app-package-install-oamap`: a map pack installed, replaced and asked
  about in a scratch Maps folder, each refusal of a pack, and the fit hook,
  told the expansions the pack needs; an install, an uninstall that a file
  sync follows with the pack's empty folders and an install again, three
  times, leaving no folder in Maps; a folder of empty folders installed
  into; and a folder holding the player's own file refused by the plan,
  then by the change with a log line, the file kept.
- `app-mod-install-handoff`: the instance lock and the hand-off folder,
  including a `.oareg` and a `.oalang` taken back in order.
- `app-package-install-inbox`: `post_opened_file` routes each extension in
  any case, the registry queue keeps order and drops a repeat, and a
  `.oareg` file leaves the package queue untouched.
- `app-package-install-origin`: the origin record's text and the texts it
  refuses; an install, replace, roll back, reinstall and recovery in a
  scratch Mods folder, each record moving with its folder; a package's own
  origin file left out; a catalogue hash that differs refused; the hash
  phase held to its budget; `write_origin` replacing a record and refusing
  a missing folder, a link and a lock that is held; the folder hooks'
  records; catalogue outcomes kept, capped and taken once, including one a
  pending change reports; and an update held for the mod being played,
  named by `pending_change_target` while it waits and put in place by
  `finish_pending_change` as on the way out, the new version in the folder
  and the old in `.backup`, then a staged update a stop left settled away
  whole by `recover_changes`; and a held update dropped by
  `discard_pending_change`, leaving the installed version, its record and
  `.backup`, with no staged files, while a waiting roll back is left.
- `app-package-install-kinds`: the kind table holds `oamod`, `oamap`,
  `oalang`, `expansion`, `oamut`, `oabrain` and `oakeys`, a file is a package by
  those extensions, the seven folder names of each, and a made-up kind
  built in the test installs, replaces, rolls back, recovers and discards
  through the generic code, including a staged check that refuses.
- `app-package-install-oamut`: a mutator installed into a scratch Mutators
  folder, a reinstall question for the same package, an update that keeps
  `.backup` and a roll back that swaps them, a catalogue replace from the
  same registry with no question and a question that names another
  registry, a folder that holds something else left as it was, an uninstall
  the discarder deletes, a package inside one top folder, and each refusal,
  including a script, an icon, an unmet engine and more folders than a
  mutator may take. The installed prompt offers no Play now.
- `app-package-install-oabrain`: a brain installed into a scratch Brains
  folder, `check_staged` accepting its network, a reinstall question for
  the same package, an update that keeps `.backup` and a roll back that
  swaps them, a catalogue replace from the same registry with no question
  and a question that names another registry, a folder that holds something
  else left as it was, an uninstall the discarder deletes, a package inside
  one top folder, and each refusal, including a script, an icon, a network,
  a table, an unmet engine and a staged network whose bytes differ from the
  manifest's SHA-256. The installed prompt offers no Play now.
- `app-package-install-oakeys`: a key layout installed into a scratch Keys
  folder, a reinstall question for the same package, an update that keeps
  `.backup` and a roll back that swaps them, a catalogue replace from the
  same registry with no question and a question that names another
  registry, a package inside one top folder, a binding this build does not
  know listed on the question, and each refusal, including a missing
  manifest, a malformed chord, the ids `own-oa-default`, `classic` and
  `con`, a note inside a folder, an icon, an unmet engine, and an own
  folder left as it was. The installed prompt offers no Play now.
- `app-package-install-oalang`: the pseudo pack installed into a scratch
  Languages folder, a Reinstall question for the same package, a Replace
  that keeps the old revision in `.backup`, a manifest inside one top
  folder, and each refusal, including a font that does not open after
  unpacking and an entry the shared names check refuses.
- `app-package-install-oaexp`: the fixture package is refused by the
  built-in table with nothing written; the same package, checked against
  the test table, lands in `Expansions/<game>/<id>` with `.oa-content`; a
  reinstall drops a file the table does not name; uninstall removes that
  folder and leaves a sibling; a link in the expansion's place is left
  alone.

The native checks `native-mod-install`, `native-library-install` and
`native-language-install` (`docs/development/testing.md`,
[src/app](../README.md#the-library)) install packages in the game itself.

## Limitations

A package's names are checked by their ASCII letters for case, as the game's
own lookups match names (`AssetStore` in `oa/formats/hpi.hpp`); two names
that only a file system takes for one, by the case of other letters or by
another Unicode form, are refused where it does, when the second cannot be
made anew, and both unpack where it does not. Archives that span several
disks or carry data before their first entry are not read. The stock game
data has no unit texts in a pack's word, so a pack's `units.tdf` is checked
and read, and those names stay as the game data gives them. Hashing a
staged brain network is one call of at most 64 MiB on the frame that runs
it.
