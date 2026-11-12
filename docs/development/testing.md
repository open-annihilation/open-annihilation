# Testing

How to run the engine's tests and checks, and how to write a new test. The
rules themselves are in [conventions.md](conventions.md#tests); this page
explains them and shows how to follow them.

## Running the tests

Build the pinned SDL, FreeType, fonts and AccessKit once
(`python3 tools/bootstrap_sdl.py`, `python3 tools/bootstrap_text_fonts.py`,
`python3 tools/bootstrap_page_fonts.py --deps local/deps` and
`python3 tools/bootstrap_accesskit.py`,
see [CONTRIBUTING.md](../../CONTRIBUTING.md#build-and-test)). A Linux arm64
or armhf machine then builds its library from source. What that build
needs is in [platform.md](platform.md). `-DOA_ACCESSKIT_DIR=` names a
prepared tree other than `local/deps`. Then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$PWD/local/deps/sdl-install" \
    -DOA_GAME_DIR="/path/to/Total Annihilation" \
  && cmake --build build --parallel 8 \
  && ctest --test-dir build --output-on-failure
```

That one command runs every test and every source check.

Validate in the Check build: `-DCMAKE_BUILD_TYPE=Check` (in a build
directory such as `build-check`) compiles with optimisation but keeps every
assertion, with line tables for backtraces. It computes the same results as
Debug bit for bit, which the pinned-digest tests check, and runs the suite
several times faster, since most of a Debug run is unoptimised library code.
Keep Debug for stepping through code in a debugger. `CMakePresets.json`
names both, and a sanitizer build (`cmake --preset check`, then
`cmake --build --preset check` and `ctest --preset check`); put your
`OA_GAME_DIR`, compiler launcher and job count in a `CMakeUserPresets.json`
that inherits them, which Git ignores. The Clang check
of the documentation blocks in public headers is the build target
`oa-doc-check`, which the test `doc-check` builds (see
[conventions.md](conventions.md#checks)). Some useful variations:

| To | Run |
|---|---|
| run tests in parallel | `ctest --test-dir build -j 8 --output-on-failure` |
| run the tests whose names match a pattern | `ctest --test-dir build -R 'sim-detection'` |
| see one test's full output | `ctest --test-dir build -R '^match-determinism$' -V` |
| list the tests without running them | `ctest --test-dir build -N` |
| run only the source checks | `ctest --test-dir build -R 'doc-links\|runtime-surface\|style-ratchet\|format-check\|licensing-check'` |

Before asking for review, run the whole suite once in a build configured
with `OA_GAME_DIR`, and say in the pull request whether the game-data tests
ran.

### The installed game

`OA_GAME_DIR` names an ordinary Total Annihilation 3.1c installation: the
absolute path of the folder that holds `totala1.hpi`. Set it in the
environment or pass it to CMake as `-DOA_GAME_DIR=PATH`. A build tree keeps
the value it was configured with; an empty entry takes the environment
variable on the next configure. The tests read the installation's archives
as the game does, so nothing needs to be extracted.

Without an installation, each game-data test prints one line saying what it
skipped and exits with code 77, which ctest reports as skipped, not passed.
Configure with `-DOA_REQUIRE_GAME_DATA=ON` to make a missing installation a
failure instead; a test that skips because the installation lacks optional
content (a music folder, say) still reports skipped.

The native checks, which run the game headless over the installation, are
added only when `OA_GAME_DIR` names one at configure time. The game is
`open-annihilation` (`open-annihilation.exe` on Windows, and on macOS the
executable inside the application bundle `open-annihilation.app`), the file
the `oa-game` target builds; tests start it through `$<TARGET_FILE:oa-game>`.

`native-library-map-previews` (`--check-library-map-previews`) opens the
Library on Maps with three packs and waits until each preview is drawn: one
installed pack's own picture, one installed pack's terrain minimap, and one
picture from the check's registry for a pack that is not installed. It then
requires the same two installed pictures on the Active tab's map tiles, each
at the size that source produces rather than the empty well shown while a
picture is still missing. Its catalogue is served by the test fixture, and
its Maps folder, data folder and preferences stay in the build tree.

`native-library-content` (`--check-library-content`) turns Core Contingency
off, closes the Library, and requires its maps out of the skirmish list.
It then chooses the Total Annihilation demo and, on the next close,
requires that demo mounted. `OA_DEMO_INSTALLER` names the demo's installer
for that half. Without it the check prints that the demo was not checked
and still passes the Core Contingency half; it does not skip with code 77.
`--game-dir` is only the source the copy is made from. The check plays the
player's copy, and its preferences and player folder stay in the build tree.

`native-library-update-question` (`--check-library-update-question`) installs
made-up Ridge from an added registry and offers an update whose rules change.
At 640x480 it opens that row's details page, where the tabs are not shown.
Not now leaves the page. UPDATE, fifty times, leaves the Library on top with
its tabs on the next frames, including once the update is no longer offered.
Its preferences, data and player folder stay in the build tree.

`native-library-replays` (`--check-library-replays`) walks the Library's
Replays section over made-up replays written from one recorded game, which
`native-library-replays-source`, a headless loopback game with
`--record-game`, records into its working folder first. Replays is second
with no replay and with some; the section says it reads while the folder is
read, and never "No replays yet."; each filter, the kind menu, the search
(a damaged file by the name its row shows) and the sort give their rows;
Keep is in `kept.json` at once and is read back with the index whole,
deleted, truncated or full of noise, a damaged index being set aside; a
ten-player replay's card scrolls to Kept until; a campaign replay is titled
by its mission name, trailing spaces trimmed; WATCH plays 300 ticks and
leaving comes back to the same row and scroll, and Watch from starts at the
tick that Watch left, printed from the match; a replay watched to its end
offers Watch; on a build that can start a
process, the pass's own unread rows say Reading… and closing the Library leaves no
index process, and a watcher's card shows no result for that player and,
scrolled, the timeline strip; a Ctrl key the automation endpoint holds marks a second row;
a recording whose map is not
installed is refused in the card, and one whose unit table differs, refused
only after its match began loading, leaves the game on the main menu with
the Library on the same row, at the same scroll, with the refusal in its
card; Turn on recording writes the setting; and Export writes one replay as
a `.oademo` and three as `.tad` files into `Exported replays`. Exporting a
replay older than the keep span marks it, and the next consideration leaves
it while one of the same age goes. A cut `exported.json` is set aside, and
while that file is there an export writes nothing and old replays stay.
Deleting a replay while `exported.json` is damaged writes no new file, and
the next start leaves the exported replays. The check removes what it wrote as it
ends, and `library-replays-clean` clears its folders before each run.

`native-replays-content` (`--check-replays-content`,
`tools/check_native_replays_content.py`) records one loopback game, then
serves Ridge, an older map pack, a newer one and a mutator on 127.0.0.1.
The older pack is only in the catalogue's releases list. One press fetches
that pack and the mutator and the replay plays; the mutator is no longer
selected afterwards. A release neither list holds reads as not in Core
Prime's catalogue. A refused release names its registry. With the registry stopped, the same replay reads "You're
offline." `replays-content-clean` clears the working folder before each run.
The check never calls a live registry.

`native-replays-core-prime` (`--check-replays-core-prime`,
`tools/check_native_replays_core_prime.py`) records one loopback game, then
serves a made-up replay feed and its download keys on 127.0.0.1. The list
fills and scrolling asks for the next page. Those pages stay after the
cache's minute, and the scroll stays with them. Choosing one planet still
offers the others. Download asks for a key of kind `oademo` and writes
`Recordings/Core Prime/<name>.oademo`, and the row is then on this computer.
A 503 is not asked again inside ten seconds. A 429 waits for its
`Retry-After` and then saves the file. A 428 shows its code on the card.
Watch plays the saved file. A release that changed is read again and then
saved. A file whose digest differs is refused and not kept. With the server
stopped the list reads "Core Prime cannot be reached." `replays-core-prime-clean`
clears the working folder before each run. The check never calls a live host.

`native-replays-import` (`--check-replay-import`,
`tools/check_native_replay_import.py`) copies a made-up `.oademo` and a
made-up `.tad` into `Recordings/Imported` and selects each, copies nothing
when either is opened again, leaves sector 6 out of the `.tad`, and refuses
a damaged file and a version this game does not play, each with its
sentence. The check clears `Recordings` as it starts and again as it ends,
and the script gives it a user folder of its own.

`native-library-install` (`--check-library-install`) posts made-up packages
through the installer while the Library shows over a fixture feed. After
that feed's own mutator steps it swaps in the runtime's verdict entries
and dry-runs two made-up packages on the Library's worker against the
installed game. One changes every builder: the Checked fact names a unit
count above 0 for No Mod, and `Cache/mutator-verdicts.tsv` in the scratch
user folder holds one `compatible` line. Rebuilding the Library shows the
same fact and does not dry-run again. The other matches no unit and reads
the no-effect words. A third declares only the maps layer: the Checked
fact reads that it changes the map when a match starts, and the file
holds it as `compatible`. After those, about forty more made-up packages
are written with origin records, and eighty more with none, each with a
file to digest, as a hand-copied collection has: more than 120 in all.
Their folder digests are made as the worker's search makes them. One
rebuild of the Library then lists the Mutators folder once, prepares no
dry run, reads no package folder, and finishes within 250 ms. Thirty more
rebuilds with nothing changed, as each frame of a download makes, list
nothing, read nothing and take under 4 ms each. The picker's own list
over that folder lists it once and reads no package folder either.
START's hand-off of three of them, from one listing, finishes within
250 ms. No match starts.

`native-map-pack-lists` (`--check-map-pack-lists`) installs one map pack
from the Library during the run and requires its fitting map in the
skirmish list and the battle room's map list the next time each is shown,
including when the skirmish list was already open. The pack's other map
stays out of the base game's lists, then appears under a local mod when
its files fit that mod and drops out again when they do not. Uninstalling
the pack removes both maps. The check serves the pack itself.

`native-mod-map-settings` (`--check-mod-map-settings`) plays a mod folder
(`--mod-dir`) whose maps folder holds, as Total Mayhem does for
downloadable maps, an OTA with no terrain for a map the check writes as a
pack, and one for no map at all. It requires both left out of the lists,
the pack map listed under the mod's title alone with the mod's
description, size and player counts, and its OTA mounted as the mod's file
over the pack's terrain. A third map, with junk terrain and no start
positions, is listed and does not load: highlighted in the picker, it
shows its name and the game's "The terrain for the selected map does not
exist." with no picture, not the map highlighted before it. CMake writes
the mod folder's files.

Every native check names its preferences file with `--preferences-file`, so
that it never reads or writes the player's own, nor the renderer records
the game keeps beside it, which with a named file live in memory for the
run. With a named file the player's own folder, which holds the saved
games, screenshots, films, recordings and mods, is the `Open Annihilation`
folder beside that file too, so that no check writes into the Documents
folder; on macOS the Documents folder does not follow `$HOME`, so a
temporary `HOME` does not keep a run without a named file out of it. `native-user-folder`
(`--check-user-folder`) checks that folder: saved games and recordings
where earlier versions kept them, beside the preferences file and loose in
`Saves`, left alone by a start with a named file, then moved into its
`Saves/default` and `Recordings/default` once as a start with the player's
own file moves them, one named as a saved game already there kept beside
it, a mod's into its own folder, the earlier folders still read while they
hold one, a save, a screenshot and a film
placed in the `default` folders of `Saves`, `Screenshots` and `Films`
without a mod and in a made-up mod's own with one, the main menu's notice
of the moves shown once in the settings dialog's look and closed, at
640x480, 800x600 and 2560x1440, with its corners rounded as the OA layer
shows them, its Open
folder button and the
settings' Your files buttons through a recorded opener, and the `Mods`
folder's mods offered to the Library. With `--snapshot` it writes the
notice and the Your files row beside the snapshot. `native-mod-switch`
(`--check-mod-switch`) writes two test profiles into that folder's `Mods`
and switches the mod thirteen times through the Library, with Play now, and
Switch off and its Switch Mod question for no mod, each a soft restart on
the same window: each run plays the mod chosen, shown as played, and what
the heap holds in live allocations as the last
round's runs reach the main menu stays level with the first round's after
the first start.
`native-mod-install` (`--check-mod-install`) installs made-up mod packages
(`.oamod`) it writes beside that folder, in five runs on one window: the
first refuses the command line's missing package, installs a package the
Finder made from a dropped file with no question, cancels and then accepts
an update, replaces a third revision keeping one `.backup`, reinstalls,
declines an older revision, installs another version alongside, leaves a
folder that holds another mod as it is, refuses a profile that does not
resolve, rolls a mod back twice in the Library, offers no roll back to a
kept version this build cannot play and refuses, in a prompt, one that
became so while it asked, and plays the mod with Play
now; the second replaces the mod played, the change staged until the run
ends; the third finds it put in place, told, and rolls it back; the fourth
finds that put in place and leaves the folder as a stop after a replace's
first rename would; the fifth finds the mod played all the same, put back
before its folder was resolved. With `--snapshot` it writes the installed notice and
the Another Version question beside the snapshot. The installer is
`src/app/package-install`. Its four unit tests keep the names
`app-mod-install-package`, `app-mod-install-plan`, `app-mod-install-change`
and `app-mod-install-handoff` (the executables are
`oa-app-package-install-*-test`); `app-package-install-kinds` tests the
kind table. The streamed zip reader is `formats-zip-stream`, and the prompt
is `ui-engine-settings-prompt`.
`native-mod-warning` (`--check-mod-warning`) writes made-up profiles into
that folder's `Mods`: one whose second side's interface art and font are
missing, one whose unit files are missing, one whose commander's file the
catalog refuses and one that plays whole; and two folders without a
profile: one whose SIDEDATA names the same missing art and font, and one
that lacks a side's LOGO, which the Library's Play now refuses in a note.
It switches to each of the others with Play now, in one run: the folder
and the
profile whose side files are missing each warn of both files once over the
main menu and say that their games still start, and a skirmish on that
side starts from Skirmish's Start and plays 300 ticks; the profile whose
unit files are missing warns once over the main menu, shows its folder
through the recorded opener, warns again over Skirmish's Start and stays
on the setup, and a start past the warning comes back to the setup without
ending the run, where a finger's tap just under OK closes the warning; a
commander's file whose Copyright line the catalog refuses still counts as
missing while one it keeps does not; and the profile that plays whole warns
of nothing and starts its skirmish. With `--snapshot` it writes the
warnings over the main menu, the side files' skirmishes and the skirmish
setup at 1280x720 beside the snapshot. With a named file, each
Open Annihilation setting's default is the game's own behaviour on every
platform, and the installation's `totala.ini` is not read for the unit
limit, so a check plays the same on every machine. A check that depends on a
setting writes the setting's key into its file first
(`src/platform/preferences/README.md` lists the keys).
`native-engine-settings-determinism` holds this in place: with every key at
its default, the seeded skirmish writes the same trace stream and draws the
same frame as with no file, and the director render keeps its pinned frames
and sound with the weapon lighting, nanolathe glow, wreck glow and heat
distortion at 0%, and its pinned sound with them at their defaults. With a
named file Hardware acceleration and Vertical sync are
Off, and the windowed checks' `SDL_RENDER_DRIVER=software` locks both, so no
check draws through the graphics card or waits for the display; game text
keeps the game's own fonts there too, as modern fonts are Off. Six
checks pass `--force-capable`, which lifts those locks:
`native-engine-settings`, so that it can turn Vertical sync On and read it
back, and set Hardware acceleration to Basic, Full and Off through the
dialog in one step of its own, which from 2 GiB draws in the accelerated
tier on SDL's software renderer, Full in the full tier with the status
saying so,
and retries it after a drop or a function test forced to fail, every other
step leaving it Off; `native-kill-board-full`, which pins the kill board
in the Full tier and holds the card's darkening of the world under it to
the shade level's share within 2, where `native-kill-board` holds the
processor's shade exactly; and `native-render-tiers`,
`native-render-tiers-density`, `native-render-tiers-visual-rules`,
`native-render-tiers-modern-fonts` and `native-demo-render-tiers`, which with `--hardware-acceleration` run the
start-up function test on SDL's software renderer and draw in the
accelerated tier, switching it off and on as the flags would. That flag
names Full: the Basic cases run at `basic`, and the Full cases, the whole
battlefield drawn by the card (the terrain, the fog, the sprites, the
models and the darkening under the kill board and the +stats panel) with
the painters' overlay canvas laid over it, at `full`, held to the standard
tier's picture of the same moment beside the card's own draws. With the
bare flag `native-render-tiers` and `native-demo-render-tiers` also set
Enhanced anti-aliasing to 2x and 4x, where the card draws the battlefield
into a world target at that factor and the check holds the battlefield to
the target read back and reduced on the processor as the card reduces it
(see [src/app/README.md](../../src/app/README.md)).
`native-render-tiers-visual-rules` runs under a mod profile, written into
its folder of the build tree, that turns the visual rules on over the
installed game, and checks their overlays in every tier as well, the Full
tier's at its zoom floor too and with the shadow, outline and letter edges
of modern text drawn by the card as asked (`check_visual_rule_overlays`).
`native-render-tiers-modern-fonts` runs the same profile with the modern
fonts on, so that every line of game text, the clock and the resource
panel among them, leaves to the card in the Full tier what of it lies over
the battlefield, and the cases above hold it to what it asks: beside the
processor's picture, under the fog, over the never-mapped ground's black
and over the kill board.
`native-underwater-features` draws metal patches on the sea bed of Steel
Jungle, in open water, at a shore and at the edge of a boat's sight, in
the Off, Basic and Full tiers with `--hardware-acceleration`, and holds
each to the processor's picture: exactly with the water's effects off and
the fog Classic, in the processor's colours with the water's effects at
their defaults, and pixel by pixel through the Smooth fog's edge; its
pictures go to `local/reports` in its folder of the build tree.

No window of a check opens at the display's own pixel density but
`native-render-tiers-density`'s, which `--native-density` opens so, and
`native-render-tiers-density-setting`'s, which the Native pixel density
setting in its preferences file opens so: on the dummy video driver its
density is 1, unless `OA_PIXEL_DENSITY` names a whole density from 2 to 8,
which grows that window to that many pixels per point. The check holds the
match laid out in window points, read back at the display's size, at zoom 1
equal to the processor's composition, and picking the unit drawn under the
pointer. On a
display above density 1, such as a laptop's built-in display, run the same
check by hand, without `SDL_VIDEO_DRIVER`, to hold the read-back at zoom 1
to the composition enlarged by nearest replication:

```sh
SDL_RENDER_DRIVER=software build/open-annihilation.app/Contents/MacOS/open-annihilation \
    --game-dir "/path/to/Total Annihilation" --skip-intro --mute \
    --check-render-tiers --hardware-acceleration --force-capable --native-density \
    --preferences-file /tmp/render-density.conf
```

### Threads

`installed-content` spreads its work over one thread per logical core, so it
takes a few seconds on a machine with many cores. The `OA_TEST_THREADS`
environment variable sets the number of threads instead: a whole number from
1 to 1024, where 0 or an empty value keeps one per logical core. Each thread
holds the entry it is decoding, so memory grows with the count, to about
1 GB with 24 threads. In a parallel ctest run the sweep starts among the
first tests and its threads share the cores with the others. A container
limited to fewer CPUs than its host usually still reports every core of the
host, so there, and on a machine with little memory, give the sweep fewer
threads:

```sh
OA_TEST_THREADS=4 ctest --test-dir build -R '^installed-content$'
```

The output is the same for any number of threads, apart from the time the
sweep took.

The game draws the terrain, the fog, the battlefield's units, features and
effects and each frame's conversion for the window in bands on spare cores,
and plans the frame's units there before its draw list takes them
([job pool](../../src/platform/job-pool/README.md)):
up to four threads, one on a machine of one or two logical processors.
`OA_DRAW_THREADS` (1 to 32) sets the count for every run of the game a test
starts, and `--draw-threads N` for one run; every count plans the same draw
lists and draws the same frames, which `native-draw-threads`,
`world-draw-bands`, `app-xrgb-conversion`, `app-renderer-effects`,
`present-surface-band`, `model-render-rgb-bridge` and
`model-render-mesh-raster` check. To run the whole suite on one drawing
thread, or on many:

```sh
OA_DRAW_THREADS=1 ctest --test-dir build
OA_DRAW_THREADS=8 ctest --test-dir build
```

### Time limits

The `native-navigation-*` and `native-side-commanders-*` tests play the
longest headless games, and give each run of the game 900 seconds. A build
whose run-time error checks make the game several times slower can set
`OA_TEST_TIMEOUT_SCALE` to a positive number that multiplies that limit.
ctest's own limit for each of these tests, 1800 seconds, still applies:

```sh
OA_TEST_TIMEOUT_SCALE=2 ctest --test-dir build -R '^native-(navigation|side-commanders)-'
```

Each `native-navigation-GROUP` test runs one group of the navigation check,
`--check-navigation GROUP`, which starts from the main menu and stands
alone: `screens`, `orders`, `outcomes`, `zoom` (the zoom's limits and the
first view of a match on the game's own screen), `zoom-1366x768`,
`zoom-1920x1080` and `zoom-2560x1440` (the same on that window) and
`campaign`. `--check-navigation` without a group runs every group in one
game. `native-navigation-zoom-effects` runs the zoom group with weapon
lighting, nanolathe lighting, wreck glow and heat distortion at 100%. A
named preferences file otherwise reads those four effects as 0%, so the
other navigation checks do not draw them. `native-single-player-offers`
(`--check-single-player-offers`) opens Single Player over the installed
game, where a side has a campaign, and checks New Campaign opens the
campaign screen and Previous Menu returns.
`native-single-player-offers-hidden` runs the same check over a mod folder
that hides every campaign file with an empty file of the same name: New
Campaign stays shown and greyed, a press and its key stay on Single Player
with no notice, and Any Mission is hidden. The side commanders check runs
as one test for each part (`--part`): `art`, `rule`, `loopback`, `give`, and
`records-GROUP`, the navigation group over its mod folder.

`native-side-data-per-match` (`--check-side-data-per-match`) needs `--mod-dir`
on an empty folder. It plays a skirmish on the sides the game started with,
then writes `gamedata/sidedata.tdf` into that folder so one side names the
other side's commander and plays again. A file with no sides, and a
commander the game has no unit for, each refuse the start and leave those
sides in place. With the file removed, one more skirmish plays the sides
the game started with. The skirmish setup keeps that side count throughout.

`native-gadget-art-per-match` (`--check-gadget-art-per-match`) needs
`--mod-dir` on an empty folder. It writes a download file there that links
a unit onto a lab's download page, then plays three skirmishes. The button
has no picture, then the picture after the check copies build-page art into
`anims/<unit>_gadget.gaf` in that folder, then no picture once that file is
removed. The check removes what it wrote.

`native-picture-frames` (`--check-picture-frames`) needs `--mod-dir` on an
empty folder. It writes a unit there, a page for it and the page's GAF,
whose picture has a single frame, and plays a skirmish with that unit
beside the commander. Pressed, the picture asks for a frame the GAF does
not have: the button is drawn without a picture while the press is held,
as 3.1c draws it, the match plays on and the fault is logged once, naming
the page, the unit and the GAF. The check removes what it wrote.

### The demo's installer

`OA_DEMO_INSTALLER` names the installer of the Total Annihilation demo (1997):
the absolute path of the installer file itself, which the engine recognises by
its size and SHA-256. Set it in the environment or pass it to CMake as
`-DOA_DEMO_INSTALLER=PATH`; like `OA_GAME_DIR`, a build tree keeps the value
it was configured with, and tests receive it at run time. It is the only
setting the demo's tests need: each unpacks the demo's data from the
installer itself. These tests read it:

- `demo-installer-data` unpacks the installer's archive into
  `Games/totala-demo` of a temporary player's folder, checks its size and
  SHA-256, and checks that a second start reuses it and that the unpacked
  folder is a usable installation;
- `native-demo-installer` starts the game headless, with `--game-dir` on the
  installer's folder and `--data-dir` on a scratch folder, and checks that the
  archive is unpacked into that scratch folder's player folder at
  `Games/totala-demo` with its `.oa-content` record, checked and mounted, that
  every start opens the main menu with no dialog over it, that a second start
  mounts it without unpacking it again, that a damaged archive is unpacked
  again, that an earlier `demo-1997` archive in the data folder is copied
  from once and left there, and that the installer's folder is left as it
  was;
- `native-demo-navigation`, `native-demo-saved-games` and
  `native-demo-multiplayer-menu` start the game the same way and run its
  `--check-navigation`, `--check-load-save` and `--check-multiplayer-menu`
  over the demo's data, which has no skirmish or multiplayer map and no
  save and load dialog: the notices, MULTI's among them, the grayed-out
  entries and the campaign's way in and out;
- `native-demo-render-tiers` starts the game the same way, windowed on
  SDL's software renderer, and runs `--check-render-tiers
  --hardware-acceleration --force-capable` over the main menu and the
  demo's first Arm mission (`--campaign "Arm Campaign" --mission 0`), as
  `native-render-tiers` runs it over a skirmish of the installed game: the
  accelerated presentation's frames held to the processor's composition
  and to that renderer's own filters. It skips with the game's check on a
  machine under 2 GiB of memory;
- `native-demo-campaign-ending` starts the game the same way and plays the
  demo's last Arm mission, AC03, at easy with `--give-orders`: it must be won
  without a failed tick, and its end screen must leave through the ending
  for the main menu;
- `native-demo-mission-ac01`, `-ac02` and `-ac03` start the game the same
  way on each mission of the demo's Arm Campaign (`--mission 0`, `1` and
  `2`), which must start from its briefing with units on both sides and play
  3000 ticks, or up to its outcome, without a failed tick;
- `native-demo-mission-saveload` saves each of those missions at tick 300;
  each save must load back into the same tick, units, world digest and
  orders with no state dropped, and, loaded again with none dropped, play
  on. `tools/check_native_demo_missions.py` says what each run must print;
- `frontend-dialogs-notice` opens the demo's `DEMOMSG.GUI` notice, unpacked
  into a temporary data folder, and checks its text, its buttons and their
  captions.

Without `OA_DEMO_INSTALLER` each reports skipped, under
`OA_REQUIRE_GAME_DATA` too, since the demo is optional; a path that names no
installer, or the wrong file, fails them. Each carries the ctest label
`demo`, so `ctest --test-dir build -L demo` runs them all. Configure with
`-DOA_REQUIRE_DEMO_INSTALLER=ON` to prove they ran: configuration then fails
when `OA_DEMO_INSTALLER` names no file, and a demo test that skips fails.
`demo-installer` covers the same code over a synthetic installer and runs
everywhere.

### Mod profiles

`OA_MOD_PROFILES_DIR` names a folder of reference mod profiles: one folder
per mod, each holding its `oamod.yaml`. Set it in the environment, or pass it
to CMake as `-DOA_MOD_PROFILES_DIR=PATH`, which the test then receives.
`data-mod-profile-references` resolves every profile it finds there and
checks each against the hashes the engine pins for the reference profiles;
without the variable it reports skipped. The engine names no mod: the
profiles stay outside the repository, and the test knows them only by their
hashes. The other mod-profile tests (`formats-oamod`, `data-match-rules`,
`data-mod-profile`, `data-mod-profile-bindings`, `app-mod-profile`,
`mod-registry-sync` and `mod-docs-sync`) need nothing and always run.
`mod-registry-sync` fails while the tables and records
`tools/gen_mod_registry.py` writes from the hack registry differ from the
files in the tree, and `mod-docs-sync` while a generated block of the
[standard hack and script extension pages](../mods/README.md) or their
tables differs from what `tools/gen_mod_docs.py` writes, or a page names no
hack or extension; running the script brings each back in step.
`input-actions-sync` fails while the action table
`tools/gen_input_actions.py` writes differs from the tree or the registry
breaks a rule, and `input-actions-sync-selftest` while the self-test
reports a failure.

`OA_MOD_GAME_DIR` names a mod's copied install: an `OA_GAME_DIR`
installation with the mod's files copied over it. With it and
`OA_MOD_PROFILES_DIR`, `native-mod-layout-data`
(`tools/check_native_mod_layout.py`) picks the profile whose revision
archive the install holds and starts a headless skirmish twice: on the
copied install with `--mod`, and on `OA_GAME_DIR` with `--mod-dir` naming a
scratch mod folder of hard links to the files the install adds or changes.
Both must load the same unit types, mount every archive, and never look up
a directory the profile renames by its base name (`--trace-lookups`). It
reports skipped without a matching profile, or when the scratch folder
cannot link to the install's files. With the same two settings,
`native-mod-net-loopback` (`tools/check_native_mod_loopback.py`) runs
`--net-loopback-check` on the copied install with that profile: host and
joiner must end with equal worlds under the profile's network and recorder
rules, and every recording the two machines made must lie in the mod's
folder in `Recordings` and play back with `--play-demo`.
`native-mod-stockpile-builds`
(`tools/check_native_mod_stockpile.py`) runs `--check-stockpile-builds` on
the copied install with that profile, as `native-stockpile-builds` does on
`OA_GAME_DIR`: every unit whose first weapon stockpiles must open on its
weapon page when selected, queue and drop rounds, and keep a round it built
and its queue through a save and a load. `native-mod-unit-pages`
(`tools/check_native_mod_unit_pages.py`) runs `--check-unit-pages scaled`
on the copied install with that profile for a commander, a lab, a unit that
builds nothing and one that stockpiles, as `native-unit-pages` runs
`--check-unit-pages whole` on `OA_GAME_DIR` for a builder of one page, one
of several, a factory and a unit that builds nothing: on windows of
640x480, 1280x720, 1920x1080 and 2560x1440 the side column must be drawn
at one scale and be as wide as its 128 columns at it, narrowed so that the
profile's tallest unit page ends on the window's last row where it is
taller than the window at the interface's scale; the bars and the
battlefield must start at its edge, and the bars, at the interface's
scale, must reach the window's right edge with their art; the game's own
pages must leave the column at the interface's width and scale. Each of
their pages is opened and must be drawn whole as its file places it, at the
column's scale and no other, and each control the side column draws must lie inside it and take a
click there; the radar must be drawn in the column at its scale and scroll
the view, and a press on the battlefield's first column must reach the
battlefield and one on the column's last must not. The whole window on
each type's first build page at 1920x1080 and 1280x720 is written to
`local/reports` as a PNG. The layering and layout rules
themselves are covered without game data by `hpi-layering`, `defs-layout`
and `game-directory`.

`OA_MOD_GAME_DIR` may be set in the environment or passed to CMake like
`OA_MOD_PROFILES_DIR`. The other tests that read a mod's own files find the
reference profile the same way, and skip without both variables:

- `defs-rule-keys-mod-install` reads every unit and weapon file in the
  directories the profile names with the data keys it binds; every bound key
  must read without a problem and agree with a plain scan of the text, and
  it prints how many files hold each key;
- `unit-script-extensions-mod-install` runs every unit script of the mod in
  the interpreter for a minute of game time, in a world of allied,
  unallied, local, computer and remote players, and checks each GET at an
  index the profile mounts against the extension table; then it runs the
  scripts that walk every unit id again at 1500 units per player, where each
  walk must still finish within its tick.

`defs-rule-keys` and `unit-script-extensions` cover the same readers with
made-up files and scripts and always run.

`limits-fixture` (`tools/make_limits_fixture.py`) makes two mod folders from
the installed game when `OA_GAME_DIR` is set, and writes them under
`${CMAKE_BINARY_DIR}/limits-fixture`. Nothing of them is committed. They
hold weapon ids past 255 and seven sides, for the weapon and side limits.
`ridge-wide` carries `RIDGE_W300` at id 300, `RIDGE_W1000` at id 1000 and
`RIDGE_W2000` at id 2000, a unit whose first weapon is `RIDGE_W300`, and
sides Ridge2 to Ridge6 beside the game's first two, with limits of 1024
weapon types and 10 sides. `ridge-sides` carries the same seven sides and a
limit of 7 sides, and no weapon clones. A check that reads either folder
declares `FIXTURES_REQUIRED limits-fixture`. Make the folders by hand with
`python3 tools/make_limits_fixture.py --game-dir "$OA_GAME_DIR" --oa-tool build/oa-tool --out build/limits-fixture`.

### Network play

Network play is always built and tested. Its tests are `network`, the
`net-*` tests of the wire and protocol, the session and its loopback over
sockets, the records, the match, the multiplayer frontend states, the
console commands, sync, messages and the launch switches, `tad-format` and
`demo-playback` for recorded games, `ui-multiplayer-*` for the multiplayer
screens, and `netgame-*` for its options, launch, close handlers, traffic
overlay and the `Runtime` names it uses; those ending in `-data` read the
installation and skip without it. Over the installation `OA_GAME_DIR`
names, these run the game headless:

- `native-net-loopback` (`--net-loopback-check N`) hosts and joins a match
  in one process over 127.0.0.1 and compares both worlds after N ticks.
  When the run chooses mutators, the check builds the shared mutator layer
  before the battle room's unit sync, reads the unit headers over that
  layer and takes the layer off before the launch mounts the same bytes
  again, so a mutator that rewrites or adds a unit is in the match. It
  creates no battle-room list. A run that chooses none syncs the installed
  unit table.
  On the way the host's player turns its commander's build page, which
  stays on the host: the joiner's copy of the commander keeps its page and
  the worlds still agree. Then it gives every unit it has through
  SHARE.GUI: the kbot goes, one 0x14 to the joiner, and the commander,
  whose type is in the Commander category, stays on both machines;
  `native-side-commanders-give` gives a commander outside that category, which
  goes too;
  `native-net-loopback-photo` (`--net-loopback-check 300 --net-loopback-photo`)
  is that check with the host's player in photo mode from tick 100: the
  photo camera pans and zooms, stopping, slowing, stepping and the moment
  between ticks are refused, the picture is held for 30 ticks and photo
  mode ends at tick 200. At tick 300 both worlds agree.
  `native-net-loopback-watcher` has the joiner watch, and
  `native-net-loopback-computer` and `native-net-loopback-computer-watcher`
  have the host seat a computer player that builds, is given a squad and
  attacks. After each machine's tick the computer checks read every byte
  it sent the other before the other plays its tick, so each machine
  applies the other's records at the same tick whatever the load, and the
  fight plays the same way every run. Each wait on the other machine
  counts rounds, each serving both machines once, and gives up only after
  200 rounds and 15 s with no progress (the unit sync shows its records as
  progress), so a machine under load takes longer over the checks without
  failing them;
- `native-net-loopback-wide` (`--net-loopback-check 300 --mod-dir` of
  `ridge-wide` and `--net-loopback-shooter RIDGEW300`) runs that check under
  a profile with more than 256 weapon types. The host makes one `RIDGEW300`
  beside its commander. That unit fires at the ground beside the commander's
  aim point, its shots cross as the wide weapon records, both worlds digest
  the same, and the joiner receives no 0x0d, 0x0e or 0x0f. It needs the
  `limits-fixture` fixture;
- `native-net-loopback-sides` runs the plain check on `ridge-sides`, whose
  profile raises only the side limit, so weapon records stay the narrow ones
  (`wide records applied 0 / 0`). It needs the `limits-fixture` fixture;
- `native-multiplayer-menu` (`--check-multiplayer-menu`) clicks MULTI and
  checks the multiplayer screens;
- `native-recording-hook` (`--check-recording-hook`) checks that the game
  declines bytes that are no recording and refuses a truncated one;
- `native-replays-record` (`tools/check_native_replays_record.py`) runs the
  headless loopback five ways, each in a scratch user folder: `--record-game`
  with the setting on writes one `.oademo` that `--play-demo` plays to its end
  with no record or tick error and whose `meta.json` names both players; the
  setting on and off, with the same `--seed`, give the same `--trace-digest`
  and the same setup blocks, none presenting a recorder; a game killed part
  way leaves its recording in `Recordings/.replays/partial`, which the next
  start finishes with `recorded.after_crash` true, and which plays clean; a
  profile with the recorder hack writes a `.tad` with no sector 6 and no
  `.oademo`; a headless game without `--record-game` writes no
  `Recordings` folder; and a replay of a game with a mod names the mod by a
  SHA-256 (the folder's, computed by the check, for a mod folder). `netgame-recording-partials` and `demo-playback`
  check the same rules without the game;
- `native-replays-skirmish` (`tools/check_native_replays_skirmish.py`) runs
  two built-in computer players on Ashap Plateau for 9000 ticks with and
  without `--record-game` and compares the `--trace-digest` streams, plays
  the `.oademo` to that tick and checks each player's units, kills and
  losses, then records a plain headless skirmish (`--match-ticks 3000
  --seed 7 --give-orders`) and plays it back clean. It also records a fight
  (`--give-orders --combat 60 --seed 9 --match-ticks 9000`) and checks each
  side's kills, metal and energy and the feature cells against the replay,
  records a game saved mid-way and the game loaded from that save (the same
  traces either way; the loaded replay starts from the map's features),
  plays a `ridge-tough` skirmish under the strict unit table, and runs
  `--check-away-pause --record-game`;
- `native-replays-campaign` (`tools/check_native_replays_campaign.py`) records
  Arm Campaign mission 0 for 2400 ticks with `--give-orders --seed 7`, and
  mission 8 for 700 ticks. Each writes the same `--trace-digest` with and
  without `--record-game`. Mission 0's own trace is the `--past-outcome` run
  to the victory tick that 2400-tick run prints, the same with recording on
  and off. A further recording to that tick, through the end screen, is
  mission 0, `game.ended` `finished`, with `game.length_ticks` that tick.
  Each recording's players (units, kills, losses, metal and energy) and
  feature cells match the replay, and `--play-demo --match-ticks 1` creates
  as many units as the live `campaign tick 0` line. A `.tad` from
  `--record-game` and from `--net-record`, the `.tad` unpacked from the
  `.oademo`, and that `.tad` packed back, each play strict to the live end.
  `--check-away-pause --campaign "Arm Campaign" --mission 0 --record-game`
  pauses the mission when the window loses focus;
- `native-host-not-found` (`tools/check_native_host_not_found.py`) checks
  a joiner whose host is never listed: "Host not found.  Exiting...", and
  the game leaves 4 s later;
- `native-net-options` (`tools/check_native_net_options.py`) runs games
  started with `--host` and `--join`, each a program of its own on the
  dummy drivers: a host reaches its battle room, a joiner joins it by
  address and both list both players by the names given, a wrong
  `--game-password` is refused with the game list's notice, the right one
  joins the game `--game-name` names, and the joiners' preferences keep
  their address and name.
- `native-room-keys` (`tools/check_native_room_keys.py`) hosts a TCP/IP room
  and joins it from a second game, both on the dummy drivers. By the keyboard
  alone the joiner gets ready, picks a side and a colour, and the host
  starts. It then plays once more with Next button on F7: F7 walks the battle
  room, from Select Map to Mutators and on to Metal, and the game list, and
  Tab does not. Last, the host alone sets a repeat delay of 300 ms and a
  rate of ten a second, and Right held a second steps the Metal slider once
  and then at that rate. Each run writes its own preferences and user
  folder, so the next run starts clean.
- `native-keyboard-walk` (`tools/check_native_keyboard_walk.py`) presses Tab
  through the main menu, Single player, New campaign, the Load game message
  and list, the Options hub and its Sound, Music, Speeds and Visuals pages,
  Skirmish, map selection, the map browser, the mutator picker, Settings
  and its Controls page, the key editor, the Library, the welcome, the keys
  overlay, a hosted TCP/IP room on loopback, the in-game menu, the sound
  page Return on Options opens, and the in-game Options hub the O key
  opens. It fails when a press leaves no focus, a stop has no label, the
  focus leaves the top dialog, the order is not a rotation of the one the
  menus document, or the ring shows before any key. The Library is the
  exception: it opens on its search with the ring showing, and the walk
  checks that before Tab. Each launch writes its own preferences, user
  folder and data folder, with both update checks off, so the next run
  starts clean and no catalogue another run kept reaches a screen.
- `native-boneyards-keyboard-walk`
  (`src/boneyards/extension/tests/check_native_boneyards_keyboard_walk.py`)
  walks the login console, the main frame (its buttons, the zoom slider
  and the map arrows), the war map's planets, chat, the player pane, Recon,
  the side prompt, News, Ladders, Help's list and one help page, Battle,
  the host form, Profile, the Locator, QuickPlay and matchmaking, and
  presses F6 between the frame, a console and the map. It fails when a
  press leaves no focus, a stop has no label, the order is not a rotation
  of the one the consoles document, the ring shows before any key (except
  on a help page already open on a link), or a stop's window is not one of
  the windows its station owns. The label and the window it checks are the
  focused control's, not those of a caption with the same name. The replays
  are `alpha_chat.replay`, `side_prompt.replay`, `alpha_contents.replay`,
  `alpha_features_beta.replay`, `quickplay.replay` and `matchmaking.replay`.
  The last two are reply fragments, joined to the beta dossier in a scratch
  file the check removes. Each launch writes its own preferences, user
  folder and data folder. The update check is never and the catalogue is
  read only when the Library opens, which the walk never does: `-y` stops
  only when both checks are never. Nothing is dialled, and the ladders
  search is not run.

`open-annihilation --play-demo FILE.tad` replays a recorded game through
the path a watching machine receives a match on; with `--headless-check`
and `--match-ticks N` it replays without a window.

### The automation endpoint

The automation endpoint (`--fark`, [src/app/automation](../../src/app/automation/README.md))
is always built and tested. `automation-protocol` reads the protocol's
test vectors, kept in `src/app/automation/tests/vectors`, and malformed
input; `automation-protocol-vectors` checks that the vectors are the ones
their `make_vectors.py` writes; `automation-options` checks the options
and the combinations refused; `automation-input` reads the device events
of input requests and refuses those that cannot be read; and
`automation-reports` checks what the endpoint reports of a match and the
battle room, and its events, over records it builds. Over the
installation `OA_GAME_DIR` names, `native-automation-menus`
(`tools/check_native_automation.py`) starts the game with `--fark` on the
dummy drivers and drives it through the endpoint on the main menu, its
controls and a click on SINGLE among it; `native-automation-input`
(`tools/check_native_automation_input.py`) holds back a client that sends
input faster than the game takes it, types an address into the TCP/IP
dialog's field and scrolls a skirmish's camera with an arrow key the
endpoint holds down; `native-automation-frame`
(`tools/check_native_automation_frame.py`) asks the game on the main menu
for the frames it presents, in a 640x480 window and in a 1280x720 one,
and checks them against the frame it composed;
`native-automation-hit-areas` (`tools/check_native_automation_hit_areas.py`)
presses every control of Settings, and clicks a sample of the Library's
and the key editor's, at the centre of where it is drawn, at each
Interface size and Window size Settings offers in both looks, and
requires each to answer there; and `native-automation-digest`
(`tools/check_native_automation_digest.py`) plays a recorded network game back in the main loop with and without the
endpoint, a client reading the match and every event at every frame, and
requires the same trace stream.
`native-automation-extension-windows`
(`tools/check_native_automation_windows.py`) starts the game with `--fark`
and `--fixture-window`: the endpoint lists the fixture window's controls, a
click reaches its button, and the window's clock reads the fixed clock's
step times the frame. The fixture is a test extension, so only a build
configured with `-DOA_RECORD_EXTENSION_HOOKS=ON` combines it and runs this
check.

A script that starts the game imports `tools/native_offline.py`. The import
writes the preferences file when it is absent, makes a private `--data-dir`,
and adds both flags to that launch. The update check is `never`. The catalogue
check is `never`, or `library` when the launch enters the Boneyards with `-y`
or `--enter-boneyards`: `library` reads the catalogue only when the Library
opens. The mod entry check stores the update check as `when-asked` and content
updates as `library`. `never` on the update check asks to turn the online
features on, and `when-asked` does not look for a release. The welcome setup
(`--check-first-run`, `--check-setup-window`) starts with no preferences file.
A check that serves its own release feed (`--check-engine-feed`,
`--check-update-offer`, `--check-update-download`, and the update pill in
`check_native_boneyards_menu_keys.py`) keeps the update check able to run and
points `--update-feed` at a loopback publisher. The download check leaves the
catalogue at `never`.

`check-native-online` (`tools/check_native_online.py`) fails when one of those
scripts starts the game without importing `native_offline`, when a preferences
file sets either online check to `automatically` without a loopback feed, and
when a registered test starts the game on the shared data folder or without
the offline lines. A direct `oa-game` test receives both flags from
`cmake/OaOfflineGameTests.cmake`, which also writes a seed beside the
preferences file. A fixture that removes the file copies that seed back, and
the check reads the seed, so a game that rewrites its preferences afterwards
does not change the result. The two language-notice start files gain the same
lines, because their fixture copies the start file over the live one. The check
fails a fixture that removes a preferences file and puts no seed back, and a
start file that lacks the lines. A script counts as importing `native_offline`
only when the import is a statement at the top of the file, not a comment.
The check reads the registered tests from a listing that its fixture,
`check-native-online-listing`, has the build's ctest write first
(`tools/write_test_listing.cmake`), so it also runs where its Python cannot
start that ctest, as Windows Python under Wine cannot start a Linux ctest. A
cross build's tests may start the game through a runner such as Wine, which
the listing names first; the check reads the game in either place.

The MCP server in [tools/mcp](../../tools/mcp/README.md) has two checks:
`mcp-server-selftest` drives its tools against a stand-in endpoint and
needs no installation, and `native-mcp-server`
(`tools/check_native_mcp_server.py`) starts the server as an MCP client
does and has it start the game on the dummy drivers, go from the main menu
to Single Player and back, take a picture and quit.

The in-game MCP server (`--mcp`, [mcp.md](../mcp.md),
[src/app/mcp](../../src/app/mcp/README.md))
is always built. `mcp-protocol` checks framing, the lifecycle, the tool
list and the errors a bad message, an unknown tool and a schema mismatch
produce. `mcp-options` checks `--mcp`, `--mcp=observe`, `--mcp-http`,
`--mcp-clock` and the options refused beside them. Over the installation
`OA_GAME_DIR` names, `native-mcp-stdio`
(`src/app/mcp/tests/check_native_stdio.py`) starts the game with `--mcp`
on the dummy drivers, initializes, lists the tools, calls `game.status`
and `game.quit`, and requires the process to exit 0. `mcp-http` checks
the loopback HTTP framing, the bearer token and a second client being
refused. `native-mcp-http` (`src/app/mcp/tests/check_native_http.py`)
starts the game with `--mcp-http` on the dummy drivers, connects to the
loopback port named in `mcp.json`, calls `game.status` and `game.quit`,
and requires the process to exit 0. `native-mcp-clock`
(`src/app/mcp/tests/check_native_clock.py`) runs a skirmish twice with
`--mcp --mcp-clock stepped`, sets the speed, steps the same ticks, and
requires the two saved-state hashes to match. `native-mcp-status`
(`src/app/mcp/tests/check_native_status.py`) starts one such skirmish and
reads `game.info` and `game.status`: the mod and the engine version, the
local metal and energy, the unit limit, a clock that advances, and a
computer player's economy left unread.

### The renderer

The game makes its renderer by walking SDL's render drivers and makes it
again when one fails while the game runs ([src/app](../../src/app/README.md)).
On the dummy video driver every hardware driver refuses, so these run on
SDL's software renderer, with `SDL_RENDER_DRIVER` left unset so that the
walk runs:

- `app-render-host` walks made-up drivers through stand-in hooks, at start
  and in a rebuild, then makes, rebuilds and loses a renderer on the dummy
  video driver, with its renderer records in scratch folders, one that
  cannot be written among them; `app-render-host-env` names a driver that
  does not exist through `SDL_RENDER_DRIVER` itself;
- `app-scaled-world-software` draws textures beyond a texture limit of
  1024 as tiles on SDL's software renderer and reads them back as one
  texture with no limit draws them;
- `app-full-sprites` draws one list of sprites, blended sprites, particle
  squares, lines and selection lines on SDL's software renderer through
  the Full tier's sprite stage (`src/app/runtime_full.hpp`) and on the
  processor through the bands, and holds the card's picture to the
  processor's: exact where sprites are opaque at zooms 1 and 2, within 2
  levels where the alpha table blends them, the squares exact, the lines
  covering the game's lines within a pixel; with the stage's order and
  batches, the fog's states (a sprite under a cell out of sight greyed,
  under a never-mapped cell left out, every sprite in colour under the
  dithered option), a feature's frame cut off where the map the view
  shows ends, the refusals, a frame overflowing the pages and a
  pinned digest of its frame. `app-full-sprites-data` draws a scene of the
  installed game's GAF frames through its palette and alpha table the same
  way, against the processor and a reference that blends to the true mean;
  `app-full-models` and `app-full-models-data` hold the model stage to the
  processor's raster the same way, over synthetic scenes and over every
  unit model of the installed game; `app-full-fog` checks the fog passes'
  quads, corner alphas and placement, the fog mask's texels and its LINEAR
  read to those alphas, and the painters' level quads with no renderer at
  all;
- `native-renderer-ladder` (`--check-renderer-ladder`) forces each failure
  the game handles while it runs and checks that it presents on through
  it, each frame after a failure equal to the frame composed on the
  processor: the walk, a present that fails in a match and on a loading
  frame, a texture SDL cannot make, a device reset in a match, during a
  load, in a drain of input and through a movie's hook, a lost device, a
  device that waits to be reset, present stalls on steady frames and on
  frames that are not, a changed floating-point setting, and window-size
  layers in tiles past a texture limit of 2048. Then it starts the game
  again and again, in the one process, on renderer records in a folder of
  its own beside its reports, removed when it ends, as the player's own
  profile keeps them, with the setting at Basic under `--force-capable`,
  8 GiB of memory taken
  and its own clock for the stages of the sentinel: a left-over trial
  struck at the first restart, also with the sentinel's file garbled or
  lost, and recorded at the second, or at the first where a fault can stop
  the whole system; a clean pass that clears the strike, and a left-over
  `running` marker only logged; a trial that cannot be written; an
  accelerated path's trial and sentinel, the magnified world's written only
  at the first zoomed-in frame of a match and closed by switching the tier
  off; the notice waiting through `-n`'s multiplayer signal and the
  multiplayer screens; an error of the game's own, struck against nothing;
  an accelerated-only failure and the dialog's retry, a failure struck
  during it, Cancel and OK; and, naming the renderer by SDL's first
  hardware driver, left-over `create` and `standard` sentinels that make
  the walk skip it, the walk again with the records ignored, the adapter
  in another driver's words after a skip or a rebuild, lost devices,
  repeated resets, present errors and a machine under 2 GiB, each new
  record told once at the main menu. The Full tier's own fallbacks run
  with them, Full switched on as `--hardware-acceleration=full` and
  `--force-capable` would: a card call that fails on a Full match frame
  (`--render-fault card`) drops Full to Basic for the run, which presents
  the composed frame, with the failing call struck against the driver and
  the status saying Full stopped, Off and back trying Full again; an
  effects pass that fails on a Full match frame (`--render-fault effects`)
  draws that frame without the effects and keeps Full, with nothing
  struck and the failure logged once; the same card failure in two runs
  in a row records `full-unusable`, told once at the
  main menu and cleared by a raise of the row from Basic to Full, a strike
  alone cleared by a clean run; Full's first match frame writes its trial
  and sentinel, `path full`, a left-over Full trial is struck at the first
  restart and recorded at the second, or at the first where a fault can
  stop the whole system, leaving Basic standing; a Full trial that cannot
  be written keeps Basic with nothing struck; and in a shared game or a
  replay Basic applies at once, Full from the next game, a page Full would
  make during the match waits for its end, and the loading screen makes
  every page and target first. A named preferences file keeps
  the cases before them, and the other checks, on records in memory.
  `--render-fault POINT[@FRAME]` forces one failure alone, at a presented
  frame of its case; `native-renderer-ladder-create` makes every driver but
  software refuse at start;
- `native-renderer-ladder-memory` (`--render-fault memory`) switches the
  accelerated tier on, as `--hardware-acceleration` and `--force-capable`
  would, and runs only when named: it forces the memory guard's sample of
  the system's memory, so that the guard refuses the tier's buffers and
  then drops it for the run. `native-renderer-ladder-full-memory`
  (`--render-fault full-memory`) does the same for the Full tier, switched
  on as `--hardware-acceleration=full` would: the guard refuses Full's
  pages, which drops Full alone with nothing struck and nothing Off and
  back lifts, and, tripped while Full draws, drops Full first, freeing its
  pages, and Basic at the next sample while memory stays short. Each skips
  under 2 GiB.

`native-dialog-backdrop` (`--check-dialog-backdrop`) opens the in-game
briefing, the save dialog and the help dialog over a paused CORE mission
at sea in the standard, Basic and Full tiers, at zoom 1, 1.5, 2 and 0.5,
and holds the frame presented with each dialog to the frame presented
without it in every battlefield pixel outside the dialog's panel, the
units' among them, and at a raised display gamma the Full tier's within
half a step of the gamma's levels. Unlike the checks above it leaves the video driver to
the environment: on SDL's dummy driver it runs on the software renderer,
switched to every tier as `--force-capable` allows, and on a machine with
a graphics card, run without `SDL_VIDEO_DRIVER=dummy`, on the card (Metal
on a Mac), in a window. It writes the frames at zoom 1, and any that
differ, to `local/reports` in its folder, and skips under 2 GiB.

`native-factory-departure` (`--check-factory-departure`) places, in a
skirmish of its own each, a finished vehicle plant, Kbot lab, hovercraft
platform, aircraft plant and shipyard of each side, the shipyard on the
water nearest the commander, and has it build its construction unit. On
every tick from the unit's release until it has left the factory's
footprint, it draws the frame at each of the sixteen camera rows of one
plot's scroll, and every frame must draw the unit after the factory, so
that the platform never covers it; an aircraft may stay over the platform
instead. A factory the installed game lacks is passed over with a line
saying so. With `--snapshot`, it writes the frames of each factory's first
tick beside that path.

### Touch controls

The touch controls ([docs/touch-controls.md](../touch-controls.md)) are
tested without a touch screen. Three tests need no game data:

- `ui-touch-gestures` feeds the gesture recogniser
  (`src/ui/touch-gestures`) timelines of finger reports with explicit
  times: taps and double taps, the slop, holds at the shortest, default and
  longest hold delays, both one-finger drag settings, two-finger taps, pans
  with their velocity, pinches, a second finger during a drag, a third
  finger, cancels and resets;
- `ui-touch-hud` holds the controls' model (`src/ui/touch-hud`) to its
  rules: the device class from the window's size in points, the QUEUE, ADD
  and x5 latches in both modes, the tablet layout at 1180x820 and the phone
  layout at 852x393 and 956x440 with their safe areas, the rail's length,
  the drawer's and the MORE sheet's cells, the overlays' clear area, the
  left-handed mirror, hit testing with the fat finger's reach, the radial,
  the help and label texts, and the texts the labels that stand for two
  things are looked up by;
- `ui-paint` checks the touch layer's drawing primitives: rounded
  rectangles blended over a known background, the icon marks inside their
  boxes, text coverage and clipping at the layer's edges.

Over the installation, `native-touch-controls` and
`native-touch-controls-phone` run `--check-touch-controls` in a window of
the dummy video driver, at 1180x820 (an iPad's size in points, the tablet
layout) and at 852x393 (an iPhone's, the phone layout). The check turns the
touch controls on, starts a skirmish and sends `SDL_EVENT_FINGER_*` events
from a touch device of its own through the game's event dispatch, with its
own clock for the fingers' times and a frame run between steps as the game's
loop runs it. Each case starts from a known selection and fails on its own,
naming what is missing and the part of the touch controls that owns it; the
run ends with one line starting `touch controls check:` that counts the
cases passed and names the ones that failed, and exits non-zero if any
failed. The tablet run checks taps (with the fat finger's 12 pt reach), a
double tap, ground orders, boxes, a hold then a drag, the Scroll setting,
QUEUE, ADD and One action, a factory's buttons (+1, a hold's −1, x5, the
pressed look of a resting finger and a finger sliding off), the two-finger
tap, pinch and pan keeping the map under the fingers, the order wheel
(Patrol, its QUEUE hub ending PATROL with its pick, latched QUEUE
keeping the 3.1c PATROL button lit until QUEUE goes off, Info, a greyed
place), the unit info panel taking the finger (the side column's ATTACK
doing nothing under it, a drag from it drawing no box even with ATTACK
armed, a box that selects nothing and OK closing it), building placement (a drag moving the ghost under the finger, a
hold near the ghost placing it there, a double tap or a hold away from it
placing where the finger is, a refused site staying armed, QUEUE keeping the
placement for the next site, CANCEL, CLEAR and a two-finger tap ending
it), the minimap, edge scrolling and auto-scroll, fingers of other
devices, the Cmd keys, the modifiers with no latch, PAUSE and the
lifecycle's hold through
the in-game menu with When the window loses focus at Keep playing, taps
against the same mouse clicks, the overlays kept
clear of the controls, help on a long press, the groups' STORE and chips,
SELECT ▾'s All, and the banner's title in English, German and Simplified
Chinese: a Metal Extractor being placed named as the bottom bar names it,
and PATROL armed. The phone run checks the full-bleed layout and its placed
regions, the safe area of 59, 0, 59 and 21 points, scrolling, the rail, the
build drawer for a factory and for a building (with the same placement
gestures, each started from the drawer), the zoom buttons, controls
that keep their taps from the battlefield, MORE's INFO, the left-handed
layout, the start zoom of 1.25, the chat line, message log and kill board on
a phone, and the fingers of other devices, the modifiers, PAUSE and the
lifecycle, help, groups, SELECT ▾ and the banner's title there too. Both
write snapshots of the composed frame into their working directories,
`native-checks/touch-controls` and `native-checks/touch-controls-phone` in
the build tree: `touch-tablet.ppm`, `touch-phone.ppm`,
`touch-phone-safe.ppm`, `touch-radial.ppm`, `touch-placement.ppm`,
`touch-overlays.ppm` (the message log, the chat line and the kill board at
once), `touch-drawer.ppm` and `touch-more.ppm`, before the cases run. To run
one by hand:

```sh
SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy SDL_RENDER_DRIVER=software \
    build/open-annihilation.app/Contents/MacOS/open-annihilation \
    --game-dir "/path/to/Total Annihilation" --skip-intro --mute \
    --check-touch-controls --resolution 852x393 \
    --preferences-file /tmp/touch-controls-phone.conf
```

`--touch-controls` switches the touch controls on with no touch screen, to
look at their layout in a window: with `--resolution 852x393` it shows the
phone layout. A run without it and without a finger never turns them on,
which the rest of the suite relies on: every other check, digest and
recording is the same as before the touch controls existed.

### Gamepad controls

The gamepad controls (described in docs/controllers.md) are tested without
a gamepad. These tests need no game data:

- `ui-pad-controls` feeds the gamepad controls' model (the module in
  src/ui/pad-controls) timelines with explicit times: the right trackpad as
  a pointer (acceleration, speed, the landing dead band, click lock, glide
  and its ticks, the absolute mode), the rings' wedge aim, triggers read as
  buttons, holds, menu repeat, double clicks, the stick cursor and flicks,
  every row of the maps from buttons to actions with the fallback map and
  the left-handed mirror, the effective scheme, the glyph styles and the
  haptics;
- `ui-touch-hud` and `ui-paint` also check the slim pad HUD, the
  build and group rings, the badges and the glyphs;
- `ui-engine-settings` and `ui-engine-settings-dialog` check the Controller
  section's settings, the Control size and the Steam Deck's defaults;
  `platform-machine` checks how a Steam Deck and Steam's Game Mode are
  recognised;
- `platform-game-installs` builds home folders in a temporary folder with
  Steam, Heroic, Lutris and Bottles installs, and checks the folders found
  and the readers of each library file; `ui-folder-chooser` checks what the
  locator recognises, the order of its rows, its name filter, its places and
  rows, and choosing one with keys and presses; `game-directory` checks
  resolution with found folders, which asks with the system's dialog when it
  can open and with the locator in Game Mode, with no dialog, or when the
  dialog cannot open; `native-folder-chooser` drives that screen with Tab
  and the choose key onto the fixture install.

Over the installation, `native-pad-controls` runs `--check-pad-controls` in
a window of the dummy video driver at 1280x800, the Steam Deck's screen. The
check starts a skirmish and drives the game with SDL virtual pads imitating
a Steam Deck and an Xbox pad, through the game's event dispatch, with its
own clock; it ends with one line starting `pad controls check:`. A run
without a gamepad never turns the gamepad controls on, so every other
check, digest and recording is unchanged. While the check runs, the game
opens only SDL's virtual pads, so a gamepad the machine has, such as the
one an iOS simulator adds, stays out of it.

`native-pad-controls-touch-phone` (874x402) and
`native-pad-controls-touch-tablet` (1180x820) run the same check with
`--touch-controls`, as an iPhone and an iPad always have them, at their
sizes. There the pad's input leaves the touch controls in place with its
badges on them instead of showing the slim pad HUD (K16), and the phone
layout, which has no side panel, presses the build ring's wedges where the
other runs press the side panel's build buttons (K4, K6 and K18).

### Game files screen

The Game files screen ([docs/game-files.md](../game-files.md)) and the
import behind it are tested on the desktop, where no platform installs the
hooks that offer the screen to players. These tests need no game data:

- `app-game-files-import` checks the import core (`oa-app-game-files`)
  over synthetic folders: the files left out and why, the name check and
  the search two levels down, the plan's parts and switches, the space a
  copy needs, the copy into the staging folder, stopping, resuming, the
  commit, recovery at the next start, adopting files the player copied,
  and the additions and removals of the management state;
- `ui-game-files` lays out the screen's model (`src/ui/game-files`) at
  1194x834 points, at 852x393 with a phone's safe area and at 640x480, and
  checks button heights, the safe area, text inside its box, hit tests,
  focus, presses and the texts;
- `game-directory` checks resolution with the screen offered (no notice, the
  report the screen opens with, the stored folder still tried first, a check
  run that does not stop) and the missing-folder notice's Check again, which
  looks again until a folder can be played; `game-options` checks
  `--check-game-files`, its companions and `--no-game-files-screen`;
  `ui-engine-settings-dialog` checks the settings dialog's Game files
  section, listed only where the host says, and the Language dialog
  the screen opens, drawn in the modern fonts before the game's own are
  installed.

`app-game-files-import-data` copies a subset of the installation
`OA_GAME_DIR` names, checks it and commits it, and takes the demo route
over the installer `OA_DEMO_INSTALLER` names; it skips without them.

Over the installation, `tools/check_native_game_files.py` runs the game
with `--check-game-files` in a work folder under `native-checks/` of the
build tree, over a subset of the installation linked into it (copied where
the volume refuses links), on the dummy drivers and the software renderer.
The check drives the screen through taps and keys as a finger and a
keyboard would, fails with the step it waited for when one does not come,
and each test passes when the game prints `game-files check: <variant>:
passed`:

- `native-game-files`: Choose folder through Ready to copy (its list of
  files left out, a switch off and on), copying and Ready to play to the
  main menu, at 1194x834, with the OA · Aa dialog opened and cancelled on
  the way; the game folder then holds the subset and none of the files left
  out, and it was kept out of the device's backups;
- `native-game-files-phone`: the same in the phone form, at 852x393;
- `native-game-files-demo`: Choose installer with the demo's installer, its
  game data unpacked (skipped without `OA_DEMO_INSTALLER`);
- `native-game-files-copy-yourself`: I have copied it with nothing there,
  the files copied in by the check under another name, Check again, Use it;
- `native-game-files-stop`: a copy held to 4 MB a second; after 8 MB the
  game leaves the screen and comes back while it goes on, then leaves again
  until its time away runs out and the copy pauses and starts again on
  return; then Stop, keeping what was copied;
- `native-game-files-resume`: the next start continues that copy, skipping
  the files already copied (it needs `native-game-files-stop`);
- `native-game-files-not-game`: a folder that holds no installation, chosen
  with a mouse click, left with Escape;
- `native-game-files-short-space`: not enough space for the copy, Copy off,
  Check again;
- `native-game-files-manage`: the management state: adding a mod,
  checking, removing the music, Done;
- `native-game-files-manage-next-start`: the next start applies the
  removal (it needs `native-game-files-manage`).

Each writes pictures of every state it waits for into its work folder,
`game-files-<variant>-<nn>-<step>-<window|tablet|phone>.png`: the window's
frame and the same state painted at a tablet's and a phone's size, for
people to review. One variant can be run by hand:

```sh
python3 tools/check_native_game_files.py \
  --game build/open-annihilation.app/Contents/MacOS/open-annihilation \
  --work /tmp/game-files-folder --variant folder --game-dir "$OA_GAME_DIR"
```

A run without `--check-game-files` and without the hooks never
opens the screen, so every other check, digest and recording is unchanged.

### The main loop while inactive

An inactive window normally leaves the main loop waiting for an event, so
nothing runs until the player comes back. An extension can ask it to keep
running there, and let that go (`keep_running_while_inactive`, version 14
of the extension table). `native-running-while-inactive` runs
`--check-running-while-inactive` in a window of the dummy video driver: the
window loses focus, the request is held, and the frame hook is called on
three frames; the request is then released and the loop waits, the hook not
called again, until the check wakes it. The line it prints starts
`inactive loop check:`.

A run with its own work to finish first calls `defer_inactive_loop_release`
before the loop and lets the release go once that work has finished. While
the release is held off, the check counts no frames and does not release.
A build without self-checks has nothing to hold off, and the call does
nothing.

### A game played alone while the window is away

A campaign mission or a skirmish against computer players pauses while the
window is away, as Settings › Controls › When the window loses focus says,
and with Keep playing keeps the main loop running every frame there.
`native-away-pause` runs `--check-away-pause` on a skirmish, and
`native-replays-campaign` runs it on Arm Campaign mission 0 while that
mission is recorded. Each runs in a window of the dummy video driver, sending
the window's focus, minimise and restore
events and the app lifecycle's background and foreground events, on a frame
clock of its own: Pause single player sets the pause bit and shows the
paused title while the window is away, holds the skirmish for the moment
after it is back and lets it go then; the click that brings the focus back
gives no order; the Pause key's pause and the game menu stay as they were;
Pause in the moment keeps the skirmish paused and Resume on the game menu
runs it; Keep playing ticks on while away, and in the background waits
behind the game menu; and with the live-game bit set, as a multiplayer
session sets it, nothing pauses. Its lines start `away pause:`. Each
`native-net-loopback*` run also takes the focus from the host's window in
the running network match: neither machine pauses. `app-match-clock`
holds the rule for every kind of match, a replay among them.

### Opening a web address

An extension can ask whether this machine can open a web address in the
system's browser, and can ask the engine to open an http or https address
(`web_address_available` and `open_web_address`, version 14 of the
extension table). The answer is no on a Steam Deck in Game Mode, which
has no browser to hand an address to; a Steam Deck in its desktop session,
and every other machine, answers yes. Any other scheme is refused and is
not handed to the browser. `app-web-address` calls the open through a stub
opener: an http or https address reaches it unchanged, and every other
scheme is refused. `platform-machine` checks the Steam Deck answer.

### Reading a game file

An extension can read one whole file from the game's files
(`read_game_file`, version 14 of the extension table). The files are the
loose files of the folders the run layers, then the archives it mounted. A
loose file wins over a file of the same path in any archive, a mod's
archive among them. Where several loose folders are layered, the first that
holds the path wins, and a mod's folder is layered ahead of the game
folder. Where only archives hold the path, the earliest mounted archive
wins. A missing file returns false, and the call does not throw.
`app-read-game-file` reads a loose fixture file, a file that only a fixture
archive holds, and a missing path.

### The simulation hash

An extension can read the simulation hash the run plays under
(`simulation_hash`, version 14 of the extension table): 64 lower-case
hexadecimal digits. With no mod it is the plain baseline's. Under a mod it
is that profile's sim hash. While a match has mutators applied, that hash
is folded with the match's mutator digest, and the presence facts report
the folded hash with the rules differing from 3.1c. With no mutators
applied the hash and the presence facts are the profile's alone. A list
that is chosen and not yet built changes nothing. A Developer Mode
override that changes the simulation changes it at once while no match is
running; a running match keeps the hash it started with until it ends,
which is the hash a save of that match and the network game are matched
on. A display-only override leaves it unchanged. The plain baseline is
resolved once a process and kept: it comes from the rules built into the
program alone, so a mod switch, a game mode or Developer Mode never
changes it. `native-simulation-hash` checks the baseline, a skirmish with
no mutators, then one with the command line's mutator, and an override in
one start, then the first hash again once the settings are put back, and
the kept baseline against a fresh resolve; `native-simulation-hash-mod`
checks a profile that changes the simulation, the same two skirmishes,
then the same override.

### Modern text for an extension

An extension can read the modern text faces and their fallback chain at a
pixel size, lay a line out and draw it as coverage, and read the player's
Use modern fonts for game text and Text size settings (`modern_text_chain`,
`modern_text_layout`, `modern_text_pixels`, `message_log_text_size`,
`game_text_preferences_of` and `game_text_preferences`, version 14 of the
extension table). It can also tell the engine where the focused text field
is, so the input method's candidate window and the on-screen keyboard stand
clear of it (`set_focused_text_field`). `app-extension-text` checks the
message log's face at a text size, that a face's rows at that size are the
ones the fonts report, that settings written and read come back, and that
the field's place is handed to a stub in the window's coordinates.

### The kit for an extension

An extension builds Settings pages, kit screens, notices, prompts and
match-overlay components from the OA UI kit through its table
(`register_settings_pages`, kit screens in the `ScreenRegistry`,
`open_kit_screen`, `show_notice` and `ask_prompt` in the screen services,
and the overlay's kit calls, version 15 of the extension table).
`screen-registry` checks the registrations: a page and a kit screen taken,
and the refusals, the first of which is kept. `app-extension-list` checks
that `register_settings_pages` is called for every extension in list order,
and `extension-hooks-game` that the game calls it. In a build configured
with `-DOA_RECORD_EXTENSION_HOOKS=ON`, `extension-kit`
(`tests/extension/check_kit_fixture.py`) runs the Kit fixture, a test
extension (`tests/extension/kit_fixture.cpp`) that uses the kit through the
table alone, over the installation `OA_GAME_DIR` names. In a 640x480, a
1280x720 and a 1920x1080 window, on the dummy drivers with `--fark`, it
opens Settings by the OA button and requires the fixture's page listed below
Developer, its controls inside the window with no two overlapping, Tab
through them in the order the page declares, and OK storing a change and
calling the page's `applied`. It opens the fixture's kit screen with
`--kit-fixture-screen` and requires its dialog, its controls inside the
window, its tab order and CLOSE; and it asks the fixture's prompt with
`--kit-fixture-prompt` and requires Yes to reach the answer. Then a
headless match with `--snapshot` must show the fixture's chip edged in the
outline colour it prints, where it prints it, from the battlefield's
corner; with the chip's setting stored Off the fixture draws none. Its
output names each window size. `native-registered-kit` checks the same
hosts with a page, a screen, a notice and a prompt registered from inside
the engine, with their size class and scale. `native-focus-ring` checks
the family's focus ring on the original menus: hidden when a screen opens,
shown by Tab, hidden by a pointer move, and shown from the open when
Always show focus is on. It also checks that Load game's message opens
without the ring unless that setting is on, that Skirmish's mutators
button takes the ring, that the in-game menu's OK takes it after Tab, and
that `ui.screen` names the control under the strip once the ring is drawn.

### OA's own screens

The native checks of OA's own screens open them at window sizes chosen for
the size class each gives under Auto: the largest class whose settings
dialog fits with 24 points round it, one step of scale down where that
gives Large ([src/ui/kit](../../src/ui/kit/README.md#entry-points)). With
the modern fonts Off, as a check's preferences file leaves them, 640x480 is
Compact at 1x, 800x600 Regular at 1x, 1280x720 and 1920x1080 Large at 1x,
and 2560x1440 Large at 2x. `native-kit-gallery`, `native-key-editor` and
`native-key-views` use 640x480, 1280x720 and 1920x1080;
`native-mutator-picker`, `native-brain-picker` and `native-map-browser`
add 2560x1440, the map browser showing six columns at Large. The checks
that show a notice or a prompt (`native-user-folder`,
`native-registered-kit`, `native-mod-warning` and `native-mod-install`)
show it at 640x480, then at 800x600 and 2560x1440. Each compares the
window with its own drawing of the screen, its corners rounded as the OA
layer rounds every window and dialog; a check that draws a screen straight
rounds it the same way (`round_drawn_screen` in
`src/app/oa_layer_check.hpp`).

`native-engine-settings` (`--check-engine-settings`) shows Settings over
the main menu at 640x480, 800x600, 1280x720, 1920x1080 and 2560x1080, and
chooses Interface sizes on 1920x1080: 200% is Regular at 2x, 400% Compact
at 3x and Auto Large at 1x. In a match it shows the dialog at 640x480,
1280x720, 1920x1080 and 2560x1080 with HUD scaling On and at 1920x1080 with
it Off: Compact at 1x at 640x480 and Large at 1x on the others, clear of the
side column, and the wheel scrolls a section only where it is taller than
its view. It reads the step Page Up and Page Down take from the dialog
(`dialog_page_step`, 184 points at Compact). At Compact, Developer scrolls
as one: its walk pages down past its rows and its list to Show Active Only
and Restore profile values, and scrolls back up before it presses Enable
Developer Mode. The OA layer's own steps in the same check move Settings by
its title (it keeps its place, a double-click centres it, Alt and the
arrows move it, and Reset window positions puts it back), find a window's
and a dialog's corners clear outside the quarter circle and edged along it
at 1x, 1.5x and 2x, show a notice with rounded corners over the main menu
at 640x480 and 2560x1440, and press S on Accessibility's practice pad, which
shows it there and does not pass it to the main menu.

`app-window-placement` checks where windows that move are kept: the Window
size words and classes, the preference keys of a window's place, their
values and Reset window positions forgetting them, rounding, a window
kept 64 points on screen at 1x and 2x with its title in the room, snapping to
the centred place, the room's centre and the safe edges, a drag and the
Alt and arrow keys, a finger's hold before it drags, and the in-match
dialog's class and place on a table of windows from 640x480 to a phone's.
`app-overlay-placement` checks Arrange Overlays' places: the keys, values
and refused texts, a place per size class, Reset forgetting only the
overlays' keys, each place measured from the nearest edge or centre of the
battlefield on the 8-point grid, a drag and the arrows with and without
Shift, and the overlays' own places at 640x480, 1280x720 and 1920x1080,
none past the battlefield nor over the 128-point side column.
`native-photo-mode` (`--check-photo mode`) plays the skirmish's fight twice
in a window on the dummy drivers, drawing a frame each tick. One pass enters
photo mode, moves the photo camera a few pixels within play's limits, turns
the selection boxes and the health bars on and off, and leaves. The other
never enters. At the last tick the two worlds read back the same. After
leaving, the layout, the camera, the zoom, the follow and an armed order
are what they were on entering. Follow is started with T, as a player
does. It does not carry in. A pan while following, and T with follow off,
leave follow as it was, after the camera step has run. In photo mode the
layout is the battlefield alone, the HUD layer is empty, and a frame with
the selection boxes on differs from one with them off. With Attack armed
the cursor over the battlefield is the order's in play, the arrow in photo
mode, and the order's again after leaving. Orders are read
before Stop. Stop, a click on open ground, a click on an enemy with Attack
armed, a click on the commander with nothing selected, a right click and a
dragged box change neither the selection nor any unit's orders, whether or
not an order is armed. Home centres on the selected unit, or on the
commander when none is selected, and selects nothing. Enter takes a photo
into the check's user folder and leaves the chat line closed, the kills
board stays unpinned and the keys overlay stays
closed. Escape leaves and keeps the armed order. A resize lays the
battlefield out alone and leaving puts back the play layout of the new
size. The game menu leaves photo mode. Settings (Primary+,) leaves photo
mode and opens that same menu. In a shared match the line carries
the clock, the latest under-attack line and the chat count, and with German
on it carries a translated word. The same steps run for a shared match and
for a watcher. The preferences file is put back to the bytes the check
started with.
`native-photo-time` (`--check-photo time`) plays the same fight twice.
One pass freezes at tick 30, draws that frame forty times, steps five
ticks, draws each eighth between two ticks, runs slowly for 15 ticks and
leaves at tick 70. The other never enters. Both reach tick 120 and read
back the same, one frame a tick outside photo mode. While frozen, two
draws of one frame match. The pause and the speed stay as they were. In
a shared match and for a watcher those commands are refused, the clock
goes on, and a held picture stays byte for byte the same for 30 ticks
and is live again after leaving. The preferences file is unchanged.
`native-photo-camera` (`--check-photo camera`) plays the same fight twice
in a window on the dummy drivers. One pass enters photo mode, zooms to the
whole map and to eight times, draws each at the battlefield's size, pans to
the view's limits and follows a unit, and leaves. The other never enters.
At the last tick the two worlds read back the same. Units stay models
unless a budget forced through rendered_units_budget_override refuses them,
and then they are dots. For the visit, Zoomed out units is Dots and After
zoom is 1/2, and both are put back. The hover is cleared on entering and
on leaving. A pan moves the photo camera and the Game block's camera stays
where it was on entering. At eight times the followed unit, after the
frame's placement, is inside the battlefield's middle half. Play's zoom
settings are unchanged, and so is the preferences file.
`native-photo-camera-full` is that check with Full taken as capable
on SDL's software renderer: every zoom it offers is drawn by Full, and a
zoom Full cannot draw is not offered, with the reason.
`native-live-renderer`'s photo-mode turn reads the unlocked turns from the
setting, holds a pan toward each edge at the whole map and at eight times,
and draws the frame at Full's floor after Classic view. In play, the Right
key held for 20 seconds in the turned view, at Full's widest zoom, keeps
the camera within the view's limits. A further pan stays there, and so do
a drag and a wheel step onward past that limit. The frame is still drawn,
and Full stays on. A zoom in from a corner of a view wider than the map,
in play and in photo mode, pulls the centre back within the closer view,
and the card still draws the frame. A wheel zoom in from that corner,
pointed near the corner, stays within the closer view too. In the classic
view under Full, with the centre a zoom left past the closer limits, the
pan role's drag moves by the drag. Turning that classic view draws the
turned frame with the place pulled within the closer limits, and turning
back leaves the classic place and its remembered centre. A picture held in
the turned view, with the skirmish read as a shared match, refuses a turn,
a tilt, the wheel and the zoom keys, keeps its zoom when the view goes
classic under Full, and gives its camera back when it is let go.
`native-photo-capture` (`--check-photo capture`) takes a photo of that fight
in photo mode, on the dummy drivers, and reads the PNG back. The check sets
the player's gamma to the saved value 20. The picture is the window's
battlefield with that gamma once, pinned by its SHA-256: the frame the
window presented, outside the photo line, and the processor's battlefield
mapped through the gamma table. A second photo in the same second is named
with ` 2`. The file is under `Screenshots/default`. The check sets the
played profile id to ridge for one photo, which is written under
`Screenshots/ridge`. The preferences file is unchanged, and a pass that
takes no photo reads back as the pass that does. Leaving and entering again
shows `Photo mode  Esc leaves`, then Frozen and the clock, and no photo's
sentence until the next photo. Then, in a shared match, it holds the
picture, lets the match run 30 ticks and takes a photo: the photo is the
held copy with that gamma once and the frame the window presented, outside
the photo line, and a photo taken after letting go differs from it. The
Basic and Full checks below end with the same held photo. The check
removes the photos it wrote.
`native-photo-capture-basic` (`--hardware-acceleration=basic --force-capable`)
saves the battlefield the screen shows, the area pass below zoom 1 included,
at 640x480 and 1280x720 and at zooms 0.5, 1 and 2. The photo matches the
frame the window presented, outside the photo line, and the processor's
battlefield mapped through the gamma table.
`native-photo-capture-full` (`--hardware-acceleration=full --force-capable`)
reads the battlefield the Full tier drew, at the window's pixel size, before
the photo line, and requires that Full drew the frame. The photo matches the
frame the window presented, outside the photo line. Its pixels are not pinned.
`native-photo-capture-live` (`--check-photo live --hardware-acceleration=full`)
photographs the turned view on the system video driver, with the player's
gamma once, and skips with status 77 when no device can draw it. Then it
holds the turned view in a shared match, drawn as played frames are, and
the photo taken 30 ticks later is the held copy, not the live picture. The
check removes the photos it wrote.
Each of the four capture checks then takes the pointer through photo mode,
in Off, Basic, Full and the turned view: with Attack armed, played frames
show the arrow at the pointer, every pixel of it in its own colours, over
the panel and over the battlefield, and photos taken with the pointer at
those two places are the same, at the window's size, at 2× and as a
poster. The photo equals the frame the window presented outside the chrome
and the arrow. A line `photo pointer <mode>:` names the photos compared.
`native-photo-depth-live` (`--check-photo depth-live --hardware-acceleration=full`)
photographs one frozen turned frame four times on the system video driver:
with depth of field, with haze, and twice with both off. Each cue changes
the picture, the two with both off match, and the frame's black, ground the
player has never seen, stays black under both cues. Then it takes a 2× and
a Smooth edges 2× photo with depth of field: the smooth one is the 2× one
box-reduced, and each record names the blur its picture has. The check
skips with status 77 when no device can draw the turned view or the card
keeps no depth. The check removes the photos it wrote.
`native-photo-sizes` (`--check-photo sizes`) freezes that fight and saves
Screen and a photo twice the battlefield. Screen keeps the picture the
window already drew. The 2× photo is drawn again by the processor, at the
player's gamma. Smooth edges is locked, because Off and Basic draw whole
pixels, and the panel says so. With memory forced short, 4× is locked for
not enough memory, and a picture held for the clipboard then locks 2×,
which was open. With the memory back, the 4× photo is four times the
battlefield. With grain on (60, size 3), Screen and 2× are taken again:
each is the look on its picture without grain, the 2× one at grain size 6,
and their records keep `grain_size` 3 and `grain_pixels` 3 and 6.
Box-reduced by 2, the 2× photo equals the Screen photo. After the pins,
with the whole map drawn, the camera goes past the left and top edges, then
the right and bottom ones, as far as the view goes: there too the 2× photo,
and the 2× photo with haze 60, box-reduce to their Screen photos, and the
camera, the sight rules and the look are put back. Box-reduced by 2, the
grain the 2× photo carries (with grain less without)
equals the grain the Screen photo carries in every colour channel no grain
pushed to 0 or 255, at least nine in ten; grain left at size 3 on the 2×
photo must fail that comparison. With only haze on, the 2× photo
box-reduces to the Screen photo in every pixel. The draw's own pixels
cancel, so the criterion holds in any render mode. The world's read-back matches a pass
that took no photo. A fight writes the engine's own preference rows; the
photos add none, and the file is put back. The SHA-256 digests of Screen,
2× and 4× are pinned.
`native-photo-sizes-full`
(`--hardware-acceleration=full --force-capable`, SDL's software renderer)
sets Enhanced anti-aliasing to 4× and draws a 2× photo with Full, at the
anti-aliasing factor the window is using, which must be above 1, and that
picture differs from the same size drawn on the processor. Smooth edges 2×
matches that 2× photo box-reduced and differs from Screen. The grain
comparison of `native-photo-sizes` holds on Full's photos too. The 2× photo
taken while photo mode keeps a replay seek's framed view is the same
picture, and that view is still kept for the window. With the texture
limit forced to the 2× photo's world target, 2× stays offered and is drawn
whole, and 4× is offered in tiles. With the limit forced to a side no
bordered tile fits, 4× is not offered and the reason names the picture that
fits at the window's factor. A held picture locks both rows
with Held picture. The window's frames after the photo match the frames
before it, apart from the file name's clock. `native-photo-tiles-full`
(`--check-photo tiles --hardware-acceleration=full --force-capable`, SDL's
software renderer) freezes one frame of a skirmish on Steel Jungle with
weapon lights, heat, a burning wreck, water and an aircraft. A 2× photo
drawn in tiles, with the check-only tile edge at a quarter of the photo's
long side, may differ from the same photo drawn whole in at most 0.01% of
the photo's pixels, rounded up and at least one. The card rasterises a tile
from another origin than the whole photo, which leaves a few pixels a few
levels off at model edges and on the water, and the maintainer ruled that
need not be bit-exact (a follow-up may tighten the bound). The check fails
when more pixels differ, and it fails on a seam: more than 16 of the
differing pixels on the columns and rows next to a tile boundary, and more
than half of all that differ. A line along a tile edge shows as that, and
a model edge or a ripple that crosses one touches a few pixels. Each
comparison that finds a difference prints the count with its bound, the
pixels on a tile edge and the largest channel gap, and the check names
every comparison outside the bound when it ends. The same holds with a tile
edge two pixels longer, whose tiles start off the game's draw rows, and with
the view half past the map's corner, where the window's limits hold it. The
same frame is compared again at size 3 (an edge of 399 photo pixels, and a
quarter of that photo's long side), at Enhanced anti-aliasing 2×, and at a
window zoom of 0.45, whose size-2 photo is below zoom 1 and is drawn through
the moved target. The window is then 1920×1080, and the 2× photo is compared
in tiles there too, framed and past the corner, before the window returns to
the size it opened at. Enhanced 2× is also compared past the corner.
Below zoom 1 has two bounds. On SDL's software renderer, which this test
runs on, that case is held to 0.5% of the photo's pixels with the same seam
rule, and the check prints that it uses that bound. A player's Full always
draws on a graphics card, and only checks use the software renderer, which
places a tile's pixels differently from the whole photo's there: about 0.26%
of them in this frame, where a card differs by about 0.002%. Every other case
keeps 0.01% on both renderers. `native-photo-tiles-full-gpu` is the same
check without `--force-capable`, on the system's own video driver, and it
holds every case, below zoom 1 too, to 0.01%. It skips with status 77 where
no graphics card draws Full, under the dummy video driver and SDL's software
renderer included. Where the shell sets the dummy driver, as `oa-ctest` does
on the build host, run it with
`env -u SDL_VIDEO_DRIVER -u SDL_RENDER_DRIVER ctest`. What
stays exact is the window: after a photo in tiles it draws as it did before,
and Full's anti-aliasing factor is unchanged, and the preferences file is put
back. `native-photo-sizes-full` runs its grain comparison a second time with
the 2× photo drawn in tiles at a quarter of its long side. The tiled photo
is held to the whole photo's look within the same bound and seam rule, the
channels of pixels that differ are not read, and every other channel is held
to the grain criterion exactly. `native-photo-sizes-live`
(`--check-photo sizes --hardware-acceleration=full`, the system's video
driver) sets Enhanced anti-aliasing to 4×. Every offered size keeps the
window frame's sampling. A size past that limit is offered in tiles, up to
16384 pixels a side, and a size that cannot be tiled is locked with the
reason that names the limit. Smooth edges 2× matches the turned view's own
2× photo box-reduced where that step is offered, at Enhanced anti-aliasing
2× when 4× does not offer it, where every size offered is drawn too, and
the window's frames stay at 4×. The grain comparison runs again with the
2× photo drawn in tiles at a quarter of its long side, held to the whole
photo's look within the same bound and seam rule. It skips with status 77
when no device can draw the view, including under the dummy video driver.
`native-photo-tiles-live` (`--check-photo tiles-live
--hardware-acceleration=full`, the system's video driver) freezes one
turned frame of that fight. A 4096 photo drawn whole matches the same photo
in four tiles of 2048, with no differing pixel on a tile edge, depth of
field off and on. Those two photos are drawn again with a forced shorter
viewport, and the card must draw every tile with its projection offset,
through a viewport within that limit. Without depth of field that pass
still has no differing pixel on a tile edge. With depth of field it holds
the tile tolerance. The frame is compared again past the map's corner, at
Enhanced anti-aliasing 2× and 4×, each also with a forced shorter viewport
held to the seam rule, at a tile edge of 500, and at 1920×1080.
At 1920×1080 and Enhanced 2×, every size above Screen that is offered is
drawn, at least one of them in tiles.
On a 640×480 battlefield at Enhanced 4×, 2×, 3× and 4× are offered. It
skips with status 77 when no device can draw the view. None
of these photos is a preference.
`native-photo-poster` (`--check-photo poster`) freezes that fight and saves
a poster two views wide at the game's own scale, its join through the fight,
half a view past the map's edge. It equals, pixel for pixel, two photos of
that frozen tick placed side by side, and seen ground lies on both sides of
the join. Ground the player has not seen is black. A poster two views tall
equals two photos one above the other, and a poster with haze, taken with
the window elsewhere, equals the plain poster with the haze painted from the
poster's own corner. A poster two views wide records grain and the tilt
band at the panel's sizes at zoom 1 and at twice them at zoom 0.5,
`grain_size` with `grain_pixels` and the band's width and radius the same
way. The turned view's reason names the
chord bound to Classic view. The battle's sight, messages and pause are unchanged,
and the preferences file is unchanged. `native-photo-poster-full`
(`--check-photo poster --hardware-acceleration=full --force-capable`, SDL's
software renderer) draws that poster in tiles and holds it to the same two
photos within the tile bound of `native-photo-tiles-full`. The picture is
not pinned. The check removes the photos it wrote. In both, a drag inside
the view marks the area it covers, a click keeps the mark, a short drag
marks 64 pixels a side, and Left at the map's left edge leaves the mark as
it is.
`native-photo-keys` (`--check-photo keys`) plays the skirmish's fight in a
window on the dummy drivers, with Off acceleration. F9 and Enter in one
input batch write nothing until a photo-mode frame is presented. That photo
is the size of the window and matches a photo of the next frozen frame.
Primary+/ opens the keys panel titled "Keys in photo mode", with a Take
photo line and without the window keys Accept, Press, Page up and Page down.
It lists Pan up, Mark area, Speed up and Back, and not Open chat, Status strip,
Screenshot or Unit info, which photo mode takes and does nothing with.
A photo with that panel open matches one with it closed, and leaving closes
a panel that was closed on entering. F9 enters and the clock is frozen.
Space lets it run and − starts Slow 9, then + runs at the match's speed and
+ again stays there, and Space freezes. `.` steps one tick, Shift+, then
Shift+. move the shutter from 8 to 7 and back to 8, + starts slow motion
and Space freezes it, Enter writes a PNG, Primary+C offers that photo
through the clipboard hook, and Page Up zooms. Alt+Left is refused and the
line says Needs Full acceleration. M chooses Poster and Area and starts a
mark at least 64 pixels on a side, without moving the camera. Right grows
that mark's width. A click that does not drag keeps the mark. S, a left click and C with the modifier
that is not Primary (Ctrl+C on a Mac, the Windows or Super key and C
elsewhere, since there Ctrl+C is Primary+C) change neither a unit nor the
view, and copy nothing. F9 and Escape each leave. Ctrl+F9 writes a PCX
screenshot outside photo mode and, on the default layout, a PNG photo inside
it. A scratch Keys layout moves Take photo to K: K writes a photo and Enter
does not. The preferences file is unchanged.
`native-photo-looks` (`--check-photo looks`) plays the capture check's fight
twice on the dummy drivers. One pass enters photo mode. With no look it
saves the pinned Off photo. Each colour grade then saves a photo of that
frozen frame, pinned by its SHA-256, equal to that grade on the battlefield
the window presented outside the photo line. One photo's record names the
grade. A square crop with a frame, a vignette and grain saves the look on
the crop, and the window shows that photo inside the crop. With the caption
bar on and the panel hidden, the presented bar equals the photo's bar,
letters included, on the whole battlefield and on a 3:2 crop, whose bar
lies above the window's bottom. A tilt-shift photo and a haze photo are
pinned the same way, and each record names that cue. At haze 100 every
pixel the fog's black pass draws keeps its colour, and every other pixel is
the haze with no fog; that photo is pinned too, and on a square crop it is
the photo the window shows. The preferences
file is unchanged, and a pass that never enters reads back as the pass that
does.
`native-photo-looks-full` (`--check-photo looks --hardware-acceleration=full --force-capable`)
filters the Full preview and the photo. The photo is that grade on the
picture Full would have saved of the same frame. Its pixels are not pinned.
`present-photo-looks` runs each grade, the vignette, the grain and each
frame on a fixed 64×36 picture, on one, two and eight threads and on the
caller. The bytes are pinned and do not depend on the number of threads.
None and an amount of 0 leave the picture unchanged, and the same seed
writes the same grain.
`present-photo-depth` pins a tilt-shift of that picture, and the ground
heights a flat plain, a ramp and a plateau project to. One, two and eight
threads, and the caller, write the same bytes. Haze leaves the pixels on the
screen rows a fog marks, and hazes the rest as with no fog.
`native-photo-panel` (`--check-photo panel`, with `--snapshot`) plays the
same fight at 640×480, 1280×720 and 1920×1080. Take photo writes a photo
with no panel in it, equal to the battlefield of that frame. H hides the
panel and shows it again. G cycles the guides, which are in no photo, and
a 1:1 crop writes a square photo. Tab walks every control and the first Tab
shows the focus ring. A drag on the battlefield pans and never selects.
With the panel hidden the arrow shows where the drag ended, and the photo
is the frame outside it.
Leaving before a frame, after Take photo in that same visit, says Photo not
taken and writes nothing. The preferences file is unchanged.
`native-photo-pad-touch` (`--check-photo pad-touch`) on a phone-sized window
opens photo mode from the game layer's L2, takes a photo, freezes, hides the
panel and leaves with the face buttons, pans and zooms with the sticks, and
presses a focused section header with A after the D-pad, then opens it from
the phone MENU. There every pressable control of the panel is at least 44
points tall; a drag of many small moves on the zoom slider moves the rows
the whole way and does not zoom; it taps a section header, the time strip,
Show selection and Take photo at their centre and a few points outside
their edge. It pans with a drag and zooms with a pinch. While Area is
chosen, L1 starts a mark and the next frame draws its rectangle, the left
stick grows it without panning, and a push across that drifts a little
down grows only its width; a tap keeps the mark, a one-finger drag marks
and two fingers pan. It takes no photo for a
cancelled finger on Take photo and one for a tap, leaves the preferences
file unchanged, and leaves with a tap on Leave.
While the controller frames the shot no pointer shows, where the mouse
left it on the battlefield: the frame is the photo off the chrome. Moving
the mouse shows the arrow there, and the next press of a pad button hides
it again. A request to close the window asks over photo mode, and B
answers No. The phone's photo mode shows no pointer.
`native-photo-close` (`--check-photo close`) opens photo mode on a frozen
fight, framed off the player's camera, and sends a request to close the
window. The question shows over photo mode: Yes and No are in the
automation tree, and the frame changes where they are. A photo taken while
it asks is the photo taken before, and a second request asks no second
question. No, clicked, and Escape go back to photo mode with the same
layout, camera, zoom, tick, freeze and panel. Yes, clicked, leaves the match
and ends the run.
`native-photo-replay` (`--check-photo replay`, with `--play-demo` naming
fixture recording 720, `720 - Show Down - NJ_THUG, Screamer.tad`) seeks to
a tick, opens photo mode from the playback bar, steps three ticks forward
and two back, and takes a photo. The photo's record holds that recording's
file name, its SHA-256 and the tick, and no folder. Slow then plays the
replay at half speed, Space pauses it and runs it at the speed photo mode
opened at, and J, L and `,` jump back ten seconds, forward ten seconds and
back one tick, each keeping the framed view. Leaving keeps the pause and
the speed the replay is watched at. After leaving, the replay plays on to
its pinned end on the replay's own clock, with no frame drawn: a drawn
frame ends the match at the game's outcome, before the recording's last
tick. The preferences file is unchanged.
`native-photo-replay-full` is that check with `--hardware-acceleration=full`
and `--force-capable`. The photo is drawn by Full. Without the fixture
recording it reports skipped, as `native-photo-replay` does.
`OA_REPLAY_FIXTURES` names the folder that holds the fixture recordings:
set it in the environment or pass `-DOA_REPLAY_FIXTURES=PATH` to CMake, with
an absolute path. Without that recording there the check reports skipped.
`ui-photo-panel-layout` lays the panel out at those three sizes in each size
class and both looks, including Large at 640×480, for a skirmish, a replay
and a shared match: every control lies inside the panel and the panel inside
the window, Tab follows the sections, and a locked control carries its
reason. Laid out for touch on two phones and a tablet, every pressable
control and every stop of a strip is a finger's row tall and wide (Size's
four stops take two rows at a phone's largest rows), and the rows show a
row at every scroll offset, with Camera open and with Photo open under the
longest size reason. `ui-photo-panel-draw` and `ui-photo-panel-draw-data` pin the panel
in those three states at those three sizes in both looks
(`src/ui/photo-panel/tests/frames.txt`). `--frames` writes
`photo-panel-<scene>.png`.
`native-photo-capture-full` also sends F9 and Enter in one batch and requires
the photo to be the size of the window and to match the next frozen frame.
`native-keys-overlay` (`--check-keys-overlay`) checks the keys overlay in
a skirmish, then the overlays' places. It scrolls Accessibility until Move
overlays and Overlay places are on screen: colour, speech and the focus
ring stand above them, and a narrow window puts the practice pad under the
rows. Move overlays is off by default with
Arrange… waiting for it; on the main menu at 1280x720 and 640x480, Move
overlays on and Arrange… opening the picture of the match, a handle on each
overlay's sample inside the battlefield, the keys overlay, the latch chips
and the chat line dragged to places on the grid, Cancel putting places
back and Reset forgetting them; in a second skirmish the keys overlay, a
latched QUEUE's chip and the chat line at their places at both sizes and
1920x1080 keeping 1280x720's; the in-game menu's OA Settings as wide as
Resume under it, opening Settings; Arrange… in a match; and Move overlays
off putting every overlay back while keeping the places. With
`--snapshot` it writes `arrange-overlays-<w>x<h>.ppm` and
`arrange-overlays-modern-<w>x<h>.ppm` at 640x480, 800x600 and 1280x720,
`overlays-placed-<w>x<h>.ppm`, `ingame-menu-<w>x<h>.ppm` and
`rounded-settings-<w>x<h>.ppm` at 640x480 and 1280x720,
`arrange-overlays-match-1280x720.ppm` and `overlays-default-1280x720.ppm`.
`native-keys-overlay-restart` starts the game again on the preferences the
first run left, Move overlays on, and finds the keys overlay and the chat
line where they were put. `native-selection-keys`
(`--check-selection-keys`) presses each of 3.1c's Ctrl+letter keys in a
skirmish as the system sends them: the modifier's own key down first, the
key down and up, then the modifier up, each through the event queue with
the pointer moving between them. It plants radar, sonar and jammer units,
a fighter, a bomber, a scout plane and armed kbots, and an enemy radar
tower, and requires Ctrl+A to take only the units that can be selected,
each other selection the loaded data's Category words name,
the rings Ctrl+R draws on the minimap, Ctrl+Shift+R and Ctrl+Shift+P
adding, Ctrl+1 and its group key, and Primary+/ opening and closing the
keys overlay. It plays Group keys centre the view each way: On a double
press, one press of the group's key selects it and leaves the view, two
quick presses centre the view on it and two slow ones do not; Never leaves
the view; On every press leaves the view on the first press and centres it
on a later press, past the double-press time. `native-match-layers` holds the frame a match
ends on to the one the game kept with Show keys on buttons Off, since the
key badges are drawn on the presented frame alone.

The key editor's draw tests (`ui-key-editor-draw`, `-draw-data`) pin, in
`src/ui/key-editor/tests/frames.txt`, the list on Common commands, the
details, the capture's outcomes, the clash question and the settling
question, Find by key, the draft line with Undo, the capture's More
options and the keyboard shown as a list, at each class where the screen
has one. `native-key-editor` (`--check-key-editor`) checks the editor over
Settings and in a paused skirmish, and the question that asks before
unsaved edits are closed, with Save, Discard and Keep editing; with
`--snapshot` it also writes `key-editor-draft.ppm` and
`key-editor-close-question.ppm` at 1280x720. `native-replay-viewer`
(`--check-replay-viewer`) records a short skirmish, watches it and presses
the playback bar, and with `--snapshot` writes the bar at 640x480, 1280x720
and 1920x1080. `native-key-capture`
(`--check-key-capture`) captures a chord, swaps a clash only once the
question's confirm button is pressed, times Choose what happens… for P on
Load, which Patrol uses, and each move of the pointer over the question
(each under 100 ms), hears Primary+ through Find by key, and settles a
draft clash from Settle…. `native-library`
(`--check-library`) writes real layouts and the player's own changes into
the check's Keys folder, presses USE and checks where it asks, What moves,
Keep them on top and Start clean, that the own changes are never written
over, and that Undo after USE puts the layout in use back and removes only
an own folder USE made.

`app-update-centre` checks the update centre's state for each set of
facts, its actions and words, the route's install words, scheduling and
taking it back, Later kept apart from Skip, a held release, each failure's
words and the time left; `app-update-receipt` checks the receipt the
update helper writes and the game reads. `native-update-download`
(`tools/check_native_update_download.py`) runs the download check's cases,
among them Pause and Resume from the main menu's update pill and from the
update centre it opens, `cancel`, where Cancel download keeps nothing and
goes back to the offer, and `pictures`, which draws every update screen in
each state at 640x480, 1280x720 and 1920x1080 from made-up states and
fetches nothing.

The kit's own tests cover its additions: `ui-kit-notices` lays a
question's parts out (callouts, boxes side by side, marked steps, a quiet
and a danger button, a part the height cuts left out whole) and the update
pill; `ui-kit-chrome` checks a header drawn at a height of its own
(`header_takes_its_height`); and `ui-kit-keys` draws the keyboard's main
block alone with every key's legend (`main_block_draws_its_keys_alone`,
`main_block_keeps_every_legend`).

### Other builds

- **Core only**, without SDL or game data: configure with
  `-DOA_BUILD_PLATFORM=OFF -DOA_BUILD_INTRO_PLAYER=OFF`.
- **Without the self-checks:** the game's `--check-*` options and
  `--reclaim-check`, with which it drives itself through its screens and
  orders for the native checks, are compiled in by default. Configure with
  `-DOA_SELF_CHECKS=OFF` and `-DBUILD_TESTING=OFF`, as the release packages
  are, to leave them out: the game then refuses those options, and its
  executable is about a fifth smaller. The native checks run them, so such a
  build builds no tests. Network play's own check options
  (`--net-loopback-check`, `--check-host-not-found`) stay in.
- **Release:** a Release build leaves out the functions and data nothing
  calls or reads (dead stripping on Apple's linker, `--gc-sections`
  elsewhere; `cmake/OaOptions.cmake`).
- **Warnings as errors:** configure with `-DOA_WARNINGS_AS_ERRORS=ON` to
  fail the build on any compiler warning in the targets that link
  `oa-options` (`-Werror`, or `/WX` with Visual Studio's compiler, whose
  linker then fails on its warnings too), as CI does with every compiler.
- **Sanitizers:** configure with `-DOA_SANITIZERS=ON` (Clang or GCC) and run
  the suite as the CI sanitizer job does, since `platform-shims` ends a
  child process with SIGFPE on purpose.

  ```sh
  ASAN_OPTIONS=detect_leaks=0:handle_sigfpe=0 ctest --test-dir build --output-on-failure
  ```

- **Windows:** build with the PowerShell commands in
  [CONTRIBUTING.md](../../CONTRIBUTING.md#build-and-test), which install zlib
  through vcpkg and use the vcpkg toolchain. The commands above are for a
  POSIX shell. A generator that holds several configurations, such as Visual
  Studio's, also needs the configuration named when ctest runs:
  `ctest --test-dir build -C Debug --output-on-failure`.
- **Windows from macOS or Linux:** `tools/build_windows.sh` cross-compiles
  the tree with mingw-w64 for x86-64, or for 32-bit x86 with
  `OA_MINGW_TRIPLE=i686-w64-mingw32`, and refuses any other triple. Each
  target has its toolchain file in `cmake/toolchains` and keeps its
  dependencies and build tree apart from the others' (`build-windows`,
  `build-windows-i686`). `tools/test_windows.sh` builds the x86-64 tree in a
  container and runs its tests under Wine. Use it when a change touches
  platform-specific code. The container keeps compiled objects in a Docker
  volume that every run and every checkout shares, so a rebuild compiles only
  what changed. A cross build cannot run the `oa-tool` it compiles.
  `OA_HOST_OA_TOOL` is the absolute path of a host build's `oa-tool`. When
  it names that executable, the cross build packs `classic.oabrain` in `brains/`
  beside the game with it, the same command a host build runs. When it is
  empty, the cross build leaves Classic unpacked and says so at configure
  time. `tools/build_windows.sh` passes `build/oa-tool` when that file
  exists. An empty `OA_HOST_OA_TOOL` in the environment leaves Classic
  unpacked.
- **Windows XP:** `OA_MINGW_TRIPLE=i686-w64-mingw32 tools/build_windows.sh
  --xp` builds the tree for 32-bit Windows XP SP3 into
  `build-windows-i686-xp`, and `tools/build_windows.sh --xp` for the 64-bit
  edition into `build-windows-xp`, with zlib and SDL3 built the same way.
  Such a build links the C library every Windows release includes, not the
  one Windows 10 added: the toolchain files switch a cross-compiler that
  links the newer one by default, and a configuration that still links it
  is refused. With another toolchain, build zlib and SDL3 with it and
  configure with
  `-DOA_WINDOWS_XP=ON`. Engine code then compiles against the declarations of
  Windows XP and every executable runs on it; see
  [src/platform/xp-runtime](../../src/platform/xp-runtime/README.md). Check an
  executable's imports against a Windows XP installation's own system DLLs
  before running it there: Windows XP refuses to start a program that imports
  anything they do not export. On XP the game plays sound through the
  engine's own wave-out output, which takes a single processor less of its
  time than SDL's there; `OA_SOUND_OUTPUT=sdl` in the environment plays
  it through SDL, which on XP uses the older sound interface XP has.
- **Windows 95:** `OA_MINGW_TRIPLE=i686-w64-mingw32 tools/build_windows.sh
  --win95` builds the tree for Windows 95 into `build-windows-i686-95`, with
  zlib, FreeType and SDL3 built the same way. It is 32-bit only: Windows 95
  has no 64-bit edition.

  Such a build compiles against the declarations of Windows 95 and stamps
  every executable subsystem and system version 4.0, which is what the loader
  checks, so a call a later Windows added does not compile and one executable
  runs on 95 and on everything after it. It links `msvcrt20.dll`, the C
  library both Windows 95 RTM and OSR2 ship; the toolchain files put the
  switch on the link line as well as the compile line, because a program
  linked without it imports the `msvcrt.dll` Windows 95 has not got.

  Two things about the toolchain are required and neither is the default. Its
  C++ run-time library must not thread through mcfgthread, which imports
  `ntdll` and Windows 95 has no `ntdll` at all, and its headers must declare
  Win32 at version 0x0400 rather than the Windows 10 that mingw-w64 defaults
  to. The Nix flake's `win95` output builds both, and
  [src/platform/win95-runtime](../../src/platform/win95-runtime/README.md)
  covers the functions Windows 95 exports as stubs. Check an executable's
  imports against a Windows 95 installation's own system DLLs before running
  it there, as for XP: 95 refuses to start a program that imports what it has
  not got, with a loader dialogue rather than a message.

  The release package is the flake's `win95` output. `nix build .#win95`
  configures a Release build with the game's self-checks left out, the same
  way the other Windows release packages are built, and within what this
  toolchain can do: the i686 instruction set, no SSE, floats on the
  processor's floating-point unit.
  It installs the game, `oa-intro.exe` and `oa-tool.exe` stripped of their
  symbol tables, the notices, `licenses/`, `fonts/`,
  `languages/interface.tdf` and `registries/` into `$out/bin`, the package
  root. The programs as the build made them, symbol table included, are
  installed in `$out/symbols` for the symbols zip. `licenses/` is every
  notice the other Windows packages carry; the Linux-only SDL notice,
  `licenses/SDL-linux.txt`, is not among them. `oa-intro.exe` is the intro
  player the game links, built with that toolchain. The four Windows
  installers are built from packages in that shape; see
  [The Windows installers](releasing.md#the-windows-installers).

  Windows 95 cannot execute SSE at all, so a build for it wants
  `-DOA_X86_FLOAT=fpu`, and `SDL_CPU_FEATURE_MASK` in the environment hides
  the features its CPUID advertises from SDL, which would otherwise choose a
  blit the system faults on.

  Windows 95's command prompt gives a Windows program no standard output and
  does not wait for it, so a check started there names a folder for its log
  and waits: `START /W open-annihilation.exe --log-dir D:\RESULTS
  --headless-check ...` leaves `open-annihilation-*.log` in `D:\RESULTS`.
  Without a sound driver SDL can use the game plays silently.
- **32-bit x86:** a 32-bit x86 build needs no SSE2: floats are computed
  with SSE (`OA_X86_FLOAT=sse`, the default; `fpu` computes them on the
  older floating-point unit, which changes the simulation's results) and
  doubles at a double's precision on the older unit
  ([src/base/float-precision](../../src/base/float-precision/README.md)).
  `cmake/toolchains/i686-w64-mingw32.cmake` builds it, and the dependencies
  with it, for the i686 instruction set without SSE2. Its
  results must equal a 64-bit build's: run the pinned-digest tests
  (`match-determinism`, `match-trace`, `persist-bank-golden`, `game-math`,
  `game-math-extended`) on it.
- **The oldest SDL:** the build accepts SDL 3.2 or newer.
  `python3 tools/bootstrap_sdl.py --version 3.2.0` builds 3.2.0, the
  oldest release, into `local/deps/sdl-install-3.2.0`, beside the pinned
  SDL, which it leaves in place; configure with that folder as
  `CMAKE_PREFIX_PATH`, as the `sdl-3-2` CI job does. Engine code that
  needs a newer SDL checks the version with `SDL_VERSION_ATLEAST`, so that
  the tree still builds and runs on 3.2.0.

### What CI runs

Continuous integration (`.github/workflows/build.yml`) runs these jobs:

| Job | What it does |
|---|---|
| `build` | Builds the tree on macOS (arm64), Windows and Linux, in Debug and in Check, starts `open-annihilation` on each, and runs every test that needs no game data, network play's among them. Running both build types runs the pinned-digest tests (`match-determinism`, `match-trace`, `persist-bank-golden`, `game-math`, `game-math-extended`) on an optimised build as well, so a build type that computes different results fails. Linux also runs the format and licence checks, and macOS the documentation check |
| `sdl-3-2` | On macOS, Windows and Linux, builds SDL 3.2.0, the oldest release the build accepts, into its own folder (`tools/bootstrap_sdl.py --version 3.2.0`), checks that the configuration found it, builds the tree in Debug against it and runs every test that needs no game data, so that engine code needing a newer SDL without checking its version fails on the system whose code it is |
| `demo` | On macOS and Linux, downloads the installer of the Total Annihilation demo (1997), checks it against its pinned SHA-256 and caches it, then builds in Check and runs the demo's tests (`ctest -L demo`) with `-DOA_REQUIRE_DEMO_INSTALLER=ON`, so a demo test that skips fails |
| `extension-recorder` | Builds and tests on Linux with the recorder test extensions registered beside network play (`-DOA_RECORD_EXTENSION_HOOKS=ON`) |
| `sanitizers` | Builds Debug with Clang on Linux under AddressSanitizer and UndefinedBehaviorSanitizer and runs the suite |

Every job configures with `-DOA_WARNINGS_AS_ERRORS=ON`, so a warning from
Clang, GCC or Visual Studio's compiler in engine code fails it. Visual
Studio's C library marks standard functions such as `strcpy`, `fopen`,
`getenv` and `sscanf` as unsafe, in favour of variants the other platforms'
C libraries lack: engine code copies text into fixed-size fields with
`oa-base-text` (`oa/base/text.hpp`), opens files with
`oa::platform::open_file` and reads the environment with
`oa::platform::environment_value`.

The installation of Total Annihilation 3.1c is not CI's to download: the
game-data tests and the native checks over it run only on contributors'
machines, so run them before asking for review.

### Before a push: other compilers' warnings

Clang on macOS does not turn on the narrowing warnings Visual Studio's
compiler reports, or GCC's `-Wsign-compare`, so a push can be the first
time those builds see them. `tools/check_conversions.py` compiles the C
and C++ sources that changed since a merge base, with Clang and
`-fsyntax-only`, and reports only the warnings that fall on changed
lines. The three flags are:

- `-Wimplicit-int-float-conversion` and `-Wshorten-64-to-32`, for the
  narrowing Visual Studio's compiler reports as C4244 and C4267;
- `-Wsign-compare`, the same warning GCC reports under that name.

```sh
python3 tools/check_conversions.py --base origin/main
```

The report names changed lines only. `--whole-files` reports every
warning of those three flags in a changed file. The check needs Clang.
Fix a finding with an explicit cast or a wider type. Do not turn the
warning off.

## Start-up and screen timing

A run writes `open-annihilation: timing:` lines. Each line ends with a
millisecond mark from process start (`oa::platform::tick_ms`). A phase that
has a measured length also names it, as `preferences (span 40 ms) 120`.

The phases are process start, preferences, user folder, the content check,
mounting, fonts, language, catalogue and registries, the display and
renderer probe, the window's first frame, the intro, and the main menu's
first frame. The window's first frame is the first present, including the
quiet frame drawn when the renderer is ready. A screen change writes
`screen <name> begin` and, when that screen is first presented,
`screen <name> first frame`. The setup's steps use `screen setup` and the
step's word: language, game, copying, online or buttons. A scan of
installers that runs off the main thread writes `sources` when it finishes.

A mark taken before the log file is open is kept and written when the log
opens. It is also written to the standard output when lines are not held
for the log, so a check that never opens a log still prints the marks.

A phase that runs longer than the progress dialog's threshold is drawn
under that dialog. See The progress dialog below.

## The progress dialog

Work that lasts past 300 ms (`long_work_show_after_ms`) shows a dialog with
the title Open Annihilation, one line and a bar or a spinner, and no
buttons. It stays at least 800 ms (`long_work_stay_ms`) once it has
appeared, then closes on its own. Quicker work shows nothing and logs
`progress dialog: not shown`. The log lines are `progress dialog: shown`,
`progress dialog: spinner` (once each time the spinner's eighth changes),
`progress dialog: closed` and `progress dialog: not shown`. The timing
marks are `progress dialog shown` and `progress dialog closed`.

The dialog covers loading the game's archives, checking the copy of Total
Annihilation, applying a language, a library copy, and a remount after a
change (`Applying your changes`). The setup shows the same dialog while an
installer scan is still reading on the game step. Pictures of the spinner
and the bar at three window sizes are in `docs/images/progress/`.

## Kinds of test

| Kind | What it does | Examples |
|---|---|---|
| Unit | Exercises one module with inputs the test builds itself | `sim-detection`, `tdf-parser` |
| Characterisation | Pins what a decoder, raster routine or table does today, edge cases stated byte by byte and seeded sweeps pinned by digest | `present-rle`, `hpi-read-node` |
| Pinned values | Pins digests or bytes that must not move: a whole match, the random streams, the bytes the writers produce | `match-determinism`, `match-shared-random`, `hpi-writer-golden`, `persist-bank-golden` |
| Game data | Reads the installed game named by `OA_GAME_DIR` | `unit-definitions-data`, `installed-content` |
| Native | Runs the game headless over the installation and checks what it does | `native-saveload`, `native-saveload-limits`, `native-trace` |
| Demo | Runs the game headless over the demo's data, unpacked from the installer `OA_DEMO_INSTALLER` names, and checks what it does | `native-demo-installer`, `native-demo-mission-ac01` |
| Extension boundary | Checks the extension table and the `Runtime` members an extension may add | `extension-layout-mismatch`, `runtime-surface-names` |
| Source checks | Check the tree's files against the conventions | `doc-links`, `style-ratchet`, `licensing-check` |

`installed-content` is the base-content guard: it mounts every archive of
the installation, decodes every entry with the engine's decoders and runs the
definition loaders a skirmish start runs, on every core (see
[Threads](#threads)). A change to a decoder runs it before review.
`installed-tdf-readers` digests what the feature, unit announcement, side
layout, meteor and scenario readers take from the installation's TDF texts,
so a change to how any of them reads shows there.

`native-mutator-layer` starts the game with `--check-mutator-layer` and two
package paths. It plays a match with no mutator, one that changes the
commanders, the same match again with none, and one mutator that does not
apply. `native-mutator-verdicts` starts the game with
`--check-mutator-verdicts`. The check writes four made-up packages beside
the preferences file and dry-runs each against the installed game: one
that changes every commander's health, one whose rules match no unit, one
that calls `ctx.incompatible`, and one whose script does not load. It
asks again and reads `Cache/mutator-verdicts.tsv` in the scratch user
folder, runs once more when an option changes, and rewrites a file of
garbage. No match starts. `native-mutator-dry-run`
(`--check-mutator-dry-run`) writes two made-up mutators that each multiply
every commander's turn rate and worker time. The verdict stays compatible,
each of those fields is held at 65535 on ARMCOM and CORCOM, and the dry
run notes each held value once and prints the note. No match starts. `native-mutator-prompts` (`--check-mutator-prompts`) starts a skirmish with
two made-up mutators, one of whose plans raises an error, and requires the
start question, Start without it, Back, the question after a crash, Get
mutators with RETRY, and the host's "The game did not start" for a guest
without its release and for one with it. `native-mutator-picker`
(`--check-mutator-picker`) opens the picker over the main menu at four
window sizes on a fixture feed, then on the runtime's own feed. With a
made-up package that changes every builder switched on, the verdict chip
reads the checking words and then `Checked with No Mod ✓`, and again
after its option changes. The scratch folder's `Cache/mutator-verdicts.tsv`
holds one line for each option value. Over SKIRMISH, three more made-up
packages switched on with no verdict are each checked in turn, and the chip
reads `Checked with No Mod ✓`. It then opens that feed with a mutator
whose check already stopped and an On mutator whose package cannot be
resolved: the chip keeps the stopped verdict and does not read the
checking words. The dry run is the Library worker's.
`native-brain-picker` (`--check-brain-picker`) opens the brain picker over the
main menu at four window sizes on a fixture feed of made-up brains, then
on the runtime's own feed after two packages are written into the scratch
Brains folder.
`native-brain-seat-buttons` (`--check-brain-seat-buttons`) checks each computer
row's brain on the skirmish setup at 640x480 and 1280x720, in a scratch
folder: the row's own Player button naming the brain, with the notch
(`oa.brain-seat.<slot>`) at its right end, the label, Tab onto the notch,
the brain menu from Return and from the right button, its level arrows,
Options… opening the brain picker on that seat and USE THIS keeping the
brain with its option, the built-in player putting the Difficulty word
back, Use for every computer seat, the hover card, the left button keeping
3.1c's own press, and the brains a start seats. With the Brains folder
removed the setup's frame matches the one kept before any brain was
written. It writes `brain-seats-skirmish-<w>x<h>.png` and, with the menu
open, `brain-seats-menu-<w>x<h>.png`.
`native-brain-campaign` (`--check-brain-campaign`) checks the campaign's
computer player on NEWGAME and Any Mission: one button under Difficulty in
the room's button art, Tab reaching it, the brain menu with no Use for
every computer seat, the brain the first mission seats and the seat a
loaded campaign save leaves. `native-brain-room` (`--check-brain-room`)
hosts a battle room alone: a computer row's Resolution cell names its
brain and its Mem cell shows the level over the room's greyed difficulty,
the notch on its name opens the brain menu, the menu's built-in player
and its level change the record the room sends, and the Mem cell steps the
built-in player's difficulty. `native-mutator-button`
(`--check-mutator-button`) and `native-mutator-room`
(`--check-mutator-room`) check Mutators as one of the room's own on and
off buttons on the skirmish setup, NEWGAME and the battle room: as large
as its neighbour with the same edges (Previous Menu; on NEWGAME and Any
Mission at the head of Difficulty's column; Select Map in the battle
room), reading "Mutators", automation "Mutators · off" and "Mutators · N
on", the lamp read from each frame (red with none on, green with some on
and, in the battle room, blinking red while Visitor lacks one), the card
the pointer shows on it, and, in the battle room, the button disabled
while the room's options are locked.
Among the battle room's steps of
`native-net-room-mutators` (`tools/check_native_room_mutators.py`), which
runs a host and a joiner as two programs, the host's button reads
"Mutators" and its automation text "Mutators · 1 on" for an installed copy
named by its id, and the room's chat names it by its name.
`frontend-room-parts` draws the rooms' parts (a plate wider than its art,
the notch, the OA stamp, a lamp, the bar and a list's rows) against a gray
palette, steps a blinking lamp through its turns and reads lamps back from
a picture, and `frontend-room-parts-data` draws each over the skirmish
setup's background from the installed game's art and, with `--frames DIR`,
writes `room-parts.png`.
`native-replay-loadout` starts the game with
`--check-replay-loadout`
and two installed mutators. It refuses a loadout that cannot be read or
whose mutator is missing, at another package, at another version or a folder
entry, plays one match of the recorded mutator, plays one with none, and
puts the player's list back, including after a start that does not happen.
`native-replay-mutators` (`tools/check_native_replay_mutators.py`) records a
loopback game with one installed mutator and checks the recording's sector
and content note. Playback of that recording applies the mutator and
refuses a missing package, another package and a sector that is not a
loadout; a recording without the sector plays no mutators. It also records
ridge-tough, which rewrites both commanders: that match starts, the
recording keeps the loadout, and strict playback matches every unit
definition. A loopback with ridge-add-unit holds the added unit, 278 unit
runtimes on 3.1c. `native-mutators-command-line` runs headless
skirmishes with
`--mutator`: none, one package, a number option, two packages in each
order, a missing mutator, an unknown option, a list the manifests refuse
and a mutator whose script stops. A companion that fails alone is run
again from the mutators that applied: that skirmish, and the host of a
network game started with the same list, play the commander health of
the applied mutator alone, and strict playback of the game's recording
rebuilds the recorded unit definitions. A list that applied in full is
not run again.
`native-replay-index` (`tools/check_native_replay_index.py`) records a
loopback game and indexes it twice with `--replay-index`: the two files are
the same bytes, and the length is the last tick `--play-demo` reaches. A
copy of the recording cut at half its packets is indexed too, and ends
`cut` on a tick before the end of the whole game. `app-replay-index` reads
the notes of a made-up recording of every kind of record, also inside a
`.oademo`, and builds the index of a made-up timeline: its totals, damage,
markers, result and end. `formats-oademo` writes and reads a
`timeline.json`.
`native-replay-keyframes` (`tools/check_native_replay_keyframes.py`) records a
loopback game and plays it with `--replay-keyframe-check` at its start, its
middle and near its end: each keyframe restored and played for the same 3,000
ticks reaches the same digest and the same state, and the replay still ends
on the digest, tick and frame count of the plain replay. A check asked for
past the end says where the replay ends. `demo-playback` takes keyframes of a
made-up replay: a keyframe restored gives the same digest tick for tick,
whichever keyframe is restored and in whichever order; a section left out of a
keyframe is reported as a keyframe fault at the tick of the next keyframe; and
a session that holds an order or a unit of its own is refused. `match-keyframe`
writes and reads the bytes of a keyframe's sections, and of a match with
nothing in it, and refuses a match that holds orders or a unit it steers.
`core-world-cpp` lists the tables of a world a keyframe copies.
`native-fast-commanders` (`tools/check_native_fast_commanders.py`) gives
ARMCOM, through `ridge-racing-commander`, the movement seven stacked
commander mutators leave it: 42 pixels a tick, a turn of 28800 a tick, and
acceleration and braking that change its speed by all of that in one tick.
On the flat platform of Over Crude Water it sends fourteen commanders east
by different distances, fourteen in their own directions and one round a
wall of dragon's teeth, and queues a solar collector behind one move. Each
must reach full speed, end its move on the point's cell without reversing
more than three times, and rest there; the solar collector must be
finished. A fourth skirmish sends the fourteen east with no mutator, over a
scratch mod folder whose ARMCOM unit file, taken from the installation with
`oa-tool asset-extract`, carries the same values: a match without mutators
steers as 3.1c does, so there a commander must step more than a cell past
its point.
`native-spawn-heights` (`tools/check_native_spawn_heights.py`) points at
markers a stage places on Coast To Coast and enters unit names on the chat
line: a shipyard, a floating torpedo launcher and a floating metal maker
on the deep water must stand on their build grid at the sea level less
their waterlines, a solar collector on a hill at the lowest corner under
its footprint, read from the map with `oa-tool asset-extract`, and a patrol
boat at a placed one's height, each to the end of the run.
`native-lava-water-effects` (`tools/check_native_lava_water_effects.py`)
finds a shore of flat land beside the sea in three maps' terrain, read with
`oa-tool asset-extract`: Lava & Two Hills (`lavaworld=1`), Higher Ground
(`lavaworld=1` and `nosealeveltrigger=1`) and Coast To Coast. On each, a
stage walks a kbot along the shore's edge while two destroyers off it
shell each other, and a director script renders it on the graphics card's
Full tier, four seconds in the classic view and four turned, once with
the four water effects at 100% and once at 0%. On the lava worlds every
frame of the two renders is the same picture; on Coast To Coast every
frame differs. It needs the system's video driver and skips under SDL's
dummy driver; `--pictures DIR` keeps a still of each view of each render.

## Writing a test

### Where it goes

A module's tests live in its `tests/` directory, one program per file named
`<name>_test.cpp`. The executable and the ctest name start with the module's
own names (`oa-sim-detection-test`, registered as `sim-detection`; a module
with several tests adds each one's case, `<group>-<module>-<case>`), so that
`ctest -R` finds a module's tests by its name. A ctest name never holds the
word `test`; it ends in `-data` when the case reads the installed game and
in `-selftest` when a check tests itself, and begins with `native-` exactly
when it is a native check: its command is the game or a
`tools/check_native_*.py` script. `ctest-names` checks these against the
[naming rule](conventions.md#naming). Tests that span modules live
under `tests/` at the root: `tests/content` for the installed-content sweep
and the installed TDF readers, and `tests/extension` for the extension
boundary.

```cmake
if(BUILD_TESTING)
  add_executable(oa-sim-detection-test tests/detection_test.cpp)
  target_link_libraries(oa-sim-detection-test PRIVATE oa-sim-detection oa-options)
  add_test(NAME sim-detection COMMAND oa-sim-detection-test)
endif()
```

Give a test that takes more than a few seconds a `TIMEOUT` with
`set_tests_properties`.

### What a test program looks like

A test is a program that runs its cases, reports each failed check with the
file, line and failed expression, and returns non-zero when any check
failed. Start the file with a comment saying what it covers. Checks count
failures and carry on, so one run shows every broken case:

```cpp
// The detection ranges of radar, sonar and jammers over a synthetic map.
#include "oa/sim/detection.hpp"

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,       \
                         __LINE__, #condition);                                 \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

void test_radar_range_edge() {
    ...
    CHECK(sees(world, radar, target_at_range));
    CHECK(!sees(world, radar, target_past_range));
}

} // namespace

int main() {
    test_radar_range_edge();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
```

- Never check with `assert()`: Release builds remove it, and the test then
  passes whatever the code does. `style-ratchet` fails on `assert()`, and on
  `#undef NDEBUG`, in a file under a `tests` directory or named
  `<name>_test`.
- Where the module's tests already have a check macro, use it rather than
  adding another variant. Otherwise link `oa-test-support` and use
  `OA_CHECK` from `oa/test/check.hpp`, which reports and counts failed
  checks as above; `main()` returns `oa::test::check_exit_status()`. The
  registry harness in `src/ui/frontend/tests/test_support.hpp` (`OA_TEST`,
  `OA_GAME_DATA_TEST`, `OA_CHECK`) is the model a fuller shared one will
  follow.
- Test code may use the whole C++20 standard library, and may throw, for
  example from a fixture's checks, as long as the test reports the failure
  with its file and line and exits non-zero. The code under test keeps its
  layer's rules (default; maintainer to confirm).
- Keep each case short and give it a name that says what it checks.
- A test never opens a window or plays sound: run the game with
  `--headless-check`, or with `SDL_VIDEO_DRIVER=dummy` and
  `SDL_AUDIO_DRIVER=dummy`.
- Write temporary files under a fresh temporary directory, never into the
  source tree or the installation: `oa::test::make_scratch_directory()`
  (`oa/test/scratch_directory.hpp`, in `oa-test-support`) creates one that
  no other run shares, so test runs from several build trees at once never
  touch each other's files. Never use a fixed name under the temporary
  directory.
- A test that builds a match links `oa-test-match` and takes what the match
  asks of its application from `oa/test/match_services.hpp`:
  `oa::test::QuietServices`, which takes every call and does nothing,
  `oa::test::StrictServices`, on which every call fails the test, and
  `oa::test::EmptyScenario`. Derive from them and override only the calls
  the test watches. To follow one projectile across ticks, take an
  `oa::test::ProjectileHandle` (`oa/test/projectile_handle.hpp`) and
  `follow()` it after every tick: the pool moves projectiles as it closes
  its gaps, and `Projectile.created_tick` changes after launch.
- Never wait a fixed time for something to happen: a loaded machine can
  stall a test for longer. Drive the clock the code reads where it takes
  one, or poll with a generous limit, and make a check that something has
  not happened yet hold however late it runs.

### Expected values

State expected values plainly: as constants in the test, as values read from
the game data of the installation, or as behaviour ("the second save loads
into the same state as the first"). A reader must be able to see where every
expected value comes from.

### Pinned values

Some tests pin a digest or a byte sequence: `match-determinism` pins the
state and trace digests of a whole synthetic match on every platform,
`match-shared-random` the random streams' draws, and the writer goldens the
exact bytes the engine writes. A pinned value moves only when the change
means it to:

1. Find out why it moved. A value that moves unexpectedly is a regression
   until shown otherwise; a change that must not alter the simulation or
   the save bytes must leave every pinned value alone.
2. When the move is intended, update the constant in the same commit, and
   say in the commit message which values changed and why.

`src/sim/match-runtime/tests/determinism_test.cpp` is the model: its header
comment says what the pinned values cover and how to change them.

### Malformed input

Every decoder of file data or network messages has a test that feeds it
malformed input: a stream that ends early, a count or offset that points
past the end, a length that would need an allocation beyond the decoder's
limit. The test checks that the decoder stops there, allocates nothing
unbounded and reports the error it should. `src/present/tests/rle_test.cpp`
is an example (`test_malformed_streams`). `net-wire-malformed`
(`src/netgame/tests/wire_malformed_test.cpp`) also changes, cuts and splices
real messages at random and feeds them to every network decoder; given
.tad recordings, or directories of them, as arguments, it takes their
packets as seeds too.

### Game data

A test that reads the installed game:

1. links `oa-test-game-data` (and `oa-formats-hpi` to open the archives), whose
   headers are `oa/test/game_data.hpp` and `oa/test/game_assets.hpp`;
2. takes the installation at run time through `require_game_directory()` or
   `require_game_assets()`, which skip the test with exit code 77, or fail it
   under `OA_REQUIRE_GAME_DATA`, when there is none;
3. is registered with `oa_add_game_data_test(<name> <command...>)`, or with
   `oa_game_data_tests(<tests...>)` after `set_tests_properties`, from
   `cmake/OaGameData.cmake`.

Never bake an installation's path into a compile definition. A test program
that has self-contained cases as well as game-data ones runs the game-data
cases when given `--data` (`game_data_requested()`), and is registered twice:

```cmake
add_executable(oa-present-pcx-test tests/pcx_test.cpp)
target_link_libraries(oa-present-pcx-test PRIVATE oa-present oa-formats-hpi oa-test-game-data)
add_test(NAME present-pcx-test COMMAND oa-present-pcx-test)
oa_add_game_data_test(present-pcx-test-data oa-present-pcx-test --data)
```

```cpp
int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv))
        test_installed_sweep(oa::test::require_game_assets("the installed PCX sweep"));
    else
        test_round_trip();
    ...
}
```

A test that can only skip because optional content is missing (another
edition's archive, a music folder) calls `oa::test::skip_test()` with what it
skipped and why; that stays a skip even under `OA_REQUIRE_GAME_DATA`.

## Not yet in place

The engine does not yet have a shared test harness beyond `oa-test-support`,
ctest labels other than `demo` (such as unit, data, native and lint) or fuzz
targets for its decoders. Until they arrive, use the registrations above and
name tests after their module so that `-R` finds them.
