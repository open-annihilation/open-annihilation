# Game files

On a phone or a tablet (see [the installation guide](installation/ios.md)),
Open Annihilation keeps its own copy of your Total Annihilation files, in
its own storage. When it starts without a game folder it can play, it opens
the **Game files** screen, which brings the files in for you. On macOS,
Windows and Linux the screen never opens: the game folder is asked for with
the system's folder chooser, and the built-in locator is for Steam Deck
Game Mode and for a Linux desktop whose folder dialog is not there. The
phone's **Choose folder** and **Choose installer** use the system's document
picker. The first start you are there
for, when you have not named a folder with `--game-dir`, copies the known
game files from the installation already in use into `Games` and
`Expansions` inside your Open Annihilation folder. That installation is
only read. A later start copies only what is missing. `--game-dir` does
not copy: the named folder is used as it is. A run that nobody is watching
does not copy either: it uses a copy already in your folder, or it stops
and tells you to start the game so the files can be copied.

## The screen

The screen fills the game's window, inside the parts of the screen the
device keeps clear (the camera housing, the rounded corners, the home
indicator). Its header carries the app icon, the plates alone, "Open Annihilation · Game
files", the version and, on the first start, an **OA · Aa** button that
opens the settings limited to **Language**, the one section that
needs no game data. On a tablet the ways in are cards side by side; on a
phone (a screen whose shorter side is under 460 points) they are rows, each
with its button at the right.

Every button is at least 44 points high, and a finger takes the nearest
button within 22 points of it. A mouse, a trackpad and a keyboard work too:
Tab and Shift+Tab move between buttons, Return or Space presses the one
with the ring, Escape (or Command and full stop) goes back or closes a
sheet, and the arrow and page keys or the wheel scroll the list of parts.
Dragging the list scrolls it.

## The three routes

The first-run screen offers up to three ways in, as the device allows:

- **Choose folder**: pick your Total Annihilation folder (the one that holds
  `totala1.hpi`) with the system's file picker, wherever it is: on the
  device, in a cloud drive, on a USB drive or a shared folder. The game
  looks at the folder, shows what it found on **Ready to copy** (the game's
  archives, the 3.1c update, Core Contingency, Battle Tactics, extra maps,
  music, the movies and any mods, each optional part with its own switch),
  says what is left out (Windows programs, help files, the uninstaller and
  system clutter, which no game data uses) and how much space the copy
  needs, then copies, checks the copy with the same check the game runs at
  every start, and puts it in place. A folder that holds no game archives
  at its top (or only the demo's installer beside a game folder) but has a
  game folder one or two levels down is offered that folder instead.
- **Copy it yourself**: copy the folder into the game's own folder with the
  device's file manager, or from a computer, then press **I have copied
  it**. A folder under another name, or archives copied loose, are found
  and offered to be put in place.
- **Choose installer**: pick the installer of the free Total Annihilation
  demo (1997); its game data is unpacked from it, as on the desktop.

## Copying, checking and playing

**Copying** shows the bytes copied of the total, the time left (after the
first five seconds, from the pace of the last ten), the file being copied,
and each part ticking off as its files finish. **Stop** asks what to do with
what was copied: keep it for later, discard it, or keep copying.

After the last file the copy is checked with the game's own check, the one
every start runs, and only a copy that passes is put in place, in one step.
**Ready to play** then says what the game will play ("Total Annihilation
3.1c with Core Contingency and Battle Tactics, music and movies."), the
space it uses, any archive that could not be opened, and that the files are
not in the device's backups unless you include them. **Play** goes on with
the normal start: the intro (skipped with `--skip-intro`) and the main
menu.

## Problems

Every problem says what happened and what to do, with a button for it, and
none of them closes the game: a folder that holds no installation, files
the game's check refuses, a file that is not the demo's installer, too
little space (with the parts whose switches would make it fit), the disk
filling during the copy, a source that went away or a download that
failed, access to the folder withdrawn, a folder that changed since the
copy began, a folder far larger than an installation, the file picker that
did not open, and the game's own folder that cannot be written. What was
copied is kept unless you discard it; **Back** returns to the first screen
(or to the management state).

## Stopping, leaving and continuing

A copy can be stopped and kept: the next start offers to continue it, and
choosing the same folder copies only what is missing, skipping each file
already copied with the same size and time. A copy goes on for a while when
you leave the game; when the device ends that time, it pauses and continues
when you come back, and the screen says which happened. A copy cut short by
the game being closed is offered to continue at the next start, the same
way. Nothing you had is deleted without your say: a game folder that is
replaced is set aside as "Total Annihilation (old)" until you remove it.

## Managing the game files

**Settings › Game files** (in the main menu's settings) shows what is
installed, its size and the free space, where the files are, and **Include
in device backups**, which is off by default: the game files are large, and
you can always copy them again. **Manage…** opens the screen's management
state: check the files again, add an expansion, maps or a mod, remove a
part, replace the game files or remove them all. Additions are moved in at
once and used from the next start; a folder whose top holds a mod profile
(`oamod.yaml`) goes into `mods/` under the profile's id. Removals and
replacements take effect from the next start of Open Annihilation, and
**Done** says so.

## Where the files are

- The game folder, `Total Annihilation`, in the folder the platform names
  (its documents folder), which the device's file manager reaches.
- The managed copy of a base game, `Games/<game>` in your Open Annihilation
  folder (in Documents), and of an expansion,
  `Expansions/<game>/<expansion>`. Each of those folders holds a
  `.oa-content` record: where the files came from, and each file's size,
  modification time and SHA-256.
- The copy being made, in `import/Total Annihilation` of the game's data
  folder (`--data-dir`, else the per-user data folder), with the import's
  state in `import/state`.
- The demo, in `Games/totala-demo` of the player's Open Annihilation folder.
  An earlier unpack in `demo-1997` of the data folder is copied from once
  and left there.

## For developers

The import core (`src/app/game_files_import.cpp`, `game_files_scan.cpp`,
`game_files_run.cpp`, `game_files_state.cpp`) plans, copies, checks and
commits with no SDL. The screen's model, texts, layout, hit test and focus
are the UI module `src/ui/game-files`. The screen's loop
(`src/app/game_files_screen.cpp`) runs on the game's window: it maps
fingers, the mouse and keys to presses, polls the import's two worker
threads into the model, paints the layout with the touch controls' painter
(`src/app/game_files_paint.cpp`) into one streaming texture, repainting only
when something changed and at most four times a second for progress alone,
and puts the renderer's state back when it ends. The **OA · Aa** dialog
(`src/app/game_files_dialog.cpp`) draws the settings dialog in the bundled
fonts through its own game text hooks while it is up.

The platform's side is the `GameFilesHooks` seam
(`src/app/include/oa/app/game_files_hooks.hpp`): the file picker, the
listing, the reading of each file, the free space, time away from the
screen, the backup setting and the platform's own words in the texts. The
screen is offered only where a platform installs the hooks. The desktop
installs none, so the screen never opens there.

An attended start that was not given `--game-dir` copies the known files of
the installation already in use into `Games/<game>` and
`Expansions/<game>/<expansion>` under the player's Open Annihilation folder.
The installation is only read. A file already recorded, with the same size
and modification time, is left in place, and a copy that stopped continues
from the bytes already written. `--game-dir` does not copy. A run that
nobody is watching does not copy: it uses a managed base game already
there, or it stops with a message. `--check-content-copy PATH` checks that
copy against a small fixture, using PATH as the Documents folder, and does
nothing else. `--check-installer FILE` reads a recognised Windows installer,
macOS installer or disk image for older Macs, lists the bundles it holds and
every file's SHA-256, and copies nothing. A start that was not given
`--game-dir` then plays that managed copy. The folders, highest first, are
the mod folder when a mod is playing, then each expansion that is on, then
the base game. An expansion's folder is the expansion, not the mod.
`--game-dir` and `--archive` keep the scan of the named folder. A bundle
the setup has just checked is not read again on that same start. The next
start checks it as usual.

### The check

The screen and its routes are checked on the desktop, headless, by
`--check-game-files`, which installs scripted platform hooks in place of the
device's file picker and drives the screen by taps and keys:

| Option | What it does |
|---|---|
| `--check-game-files` | runs the check; the run is unattended and ends at the main menu |
| `--game-files-route folder\|demo\|copy-yourself\|manage` | the route the check takes (folder by default) |
| `--game-files-expect main-menu\|stopped-kept\|resumed\|not-a-game\|short-space\|next-start` | where the route should end (the main menu by default) |
| `--game-files-source PATH` | the folder, or the installer, the scripted picker answers with |
| `--game-files-free-bytes N` | the free space the scripted hooks report |
| `--game-files-copy-rate BYTES` | the bytes a second the scripted copy is held to |
| `--game-files-stop-after BYTES` | Stop is pressed once this much was copied; the scripted copy waits there for it |

The scripted hooks use the working directory as the device: its
`Documents/Total Annihilation` is the game folder, and the platform's
default folder when it exists. They offer every way in, answer the picker
with `--game-files-source` (a held-back answer, delivered on a later pass
of the screen's loop as a platform's arrives), copy through the engine's
own copy, and record each request for time away and each backup setting,
which the verdict checks. While the screen shows Looking, Copying or
Checking, the check also fails if a player's loop would wait for an event
alone instead of polling the import, which would leave a stale step on the
screen until the next touch. Each route waits for the steps it expects, taps
the buttons through SDL events as a finger would, and fails with the step
it waited for when one does not come within three minutes or a problem it
does not expect appears. The game prints one line,
`game-files check: <variant>: passed` or `game-files check: <variant>:
failed: <why>`, and its status follows it.

| Variant | Route |
|---|---|
| `folder` | S1, Tab and its focus ring, OA · Aa and Escape, Choose folder, Ready to copy (the rows dragged when they scroll, Show and its sheet closed by Escape, a switch off and on), Copy, Ready to play, Play, the main menu; the game folder then holds the subset without the files left out, kept out of the backups |
| `phone` | the same at 852x393, the phone form, where the rows scroll; the dialog is left with Return (OK) |
| `demo` | Choose installer, the demo's row, Copy, the demo unpacked, Ready to play, the main menu |
| `copy-yourself` | I have copied it, No game folder yet, the files copied in under another name, Check again, Found your files, Use it, the main menu |
| `stop` | a copy held to 4 MB a second; after 8 MB the game leaves the screen and comes back (Copying went on), leaves again and its time away runs out (the copy pauses, then starts again on return); Stop, Keep what was copied, the continue banner |
| `resume` | the next start in the same folder: the banner, the same folder chosen, files skipped, Ready to play, the main menu |
| `not-game` | Choose folder clicked with the mouse on a folder with no installation: the problem, Escape, S1 |
| `short-space` | 50 MB free: the shortage shown with Copy off, Check again, still short |
| `manage` | the management state over an installed folder: Add files…, a mod added, Check again, Remove Music, Done and its note |
| `manage-next-start` | the next start in the same folder: the removal applied, the mod in `mods/`, Done |

At every state a route waits for, the check writes three pictures in the
working directory, named
`game-files-<variant>-<nn>-<step>[-<problem>][-<sheet>]-<window|tablet|phone>.png`:
the window's own frame, and the same state laid out and painted at a
tablet's size (1194x834 points) and a phone's (852x393 points, with 59
points kept clear at each side and 21 at the bottom), both at two pixels a
point, so one run shows both forms. The steps that only pass by (Looking,
Copying, Checking) are pictured once when the check sees them.

`tools/check_native_game_files.py` prepares each variant's work folder and
runs the game in it:

```text
python3 tools/check_native_game_files.py --game PATH --work DIR --variant NAME
    --game-dir OA_GAME_DIR [--demo-installer FILE] [--resolution WxH] [--keep-work]
    [-- more game options]
```

It links the subset (`totala1.hpi`, `rev31.gp3`, `tactics1.hpi`, two music
tracks, the smallest movie, a mod with a usable profile) from the
installation into `DIR/source`, copying where the volume refuses links,
writes the files the copy should leave out beside them, and runs the game
with `--skip-intro --mute --check-game-files --data-dir DIR/Library
--preferences-file DIR/preferences.conf --resolution 1194x834` (or the
`--resolution` given) and the variant's options, on the dummy video and
audio drivers and the software renderer. `resume` and `manage-next-start`
reuse the folder their first halves left. Without the installation (or the
demo's installer, for `demo`) it exits with the game-data skip code.

`--no-game-files-screen` is a player's switch: with no usable game folder,
a build that offers the screen shows the missing-folder notice with its
**Check again** button instead.

The `native-game-files*` tests run every variant; see
[testing.md](development/testing.md).
