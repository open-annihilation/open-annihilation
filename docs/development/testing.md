# Testing

How to run the engine's tests and checks, and how to write a new test. The
rules themselves are in [conventions.md](conventions.md#tests); this page
explains them and shows how to follow them.

## Running the tests

Build the pinned SDL, FreeType and fonts once
(`python3 tools/bootstrap_sdl.py` and `python3 tools/bootstrap_text_fonts.py`,
see [CONTRIBUTING.md](../../CONTRIBUTING.md#build-and-test)), then:

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
of the documentation blocks in public headers is a build target instead
(`cmake --build build --target oa-doc-check`, see
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
of the moves shown once in the settings dialog's look and closed, its Open
folder button and the
settings' Your files buttons through a recorded opener, and the `Mods`
folder's mods listed on the Mods page. With `--snapshot` it writes the
notice and the Your files row beside the snapshot. `native-mod-switch`
(`--check-mod-switch`) writes two test profiles into that folder's `Mods`
and switches the mod ten times through the Mods page's Switch Mod question,
each a soft restart on the same window: each run plays the mod chosen,
listed first, and what the heap holds in live allocations as the last
round's runs reach the main menu stays level with the first round's after
the first start.
`native-mod-install` (`--check-mod-install`) installs made-up mod packages
(`.oamod`) it writes beside that folder, in five runs on one window: the
first refuses the command line's missing package, installs a package the
Finder made from a dropped file with no question, cancels and then accepts
an update, replaces a third revision keeping one `.backup`, reinstalls,
declines an older revision, installs another version alongside, leaves a
folder that holds another mod as it is, refuses a profile that does not
resolve, rolls a mod back twice on the Mods page, offers no roll back to a
kept version this build cannot play and refuses, in a prompt over the
dialog, one that became so while it asked, and plays the mod with PLAY
NOW; the second replaces the mod played, the change staged until the run
ends; the third finds it put in place, told, and rolls it back; the fourth
finds that put in place and leaves the folder as a stop after a replace's
first rename would; the fifth finds the mod played all the same, put back
before its folder was resolved. With `--snapshot` it writes the installed notice and
the Another Version question beside the snapshot. The unit tests of the
installer are `app-mod-install-package`, `app-mod-install-plan`,
`app-mod-install-change` and `app-mod-install-handoff`, of the streamed zip
reader `formats-zip-stream`, and of the prompt `ui-engine-settings-prompt`.
`native-mod-warning` (`--check-mod-warning`) writes made-up profiles into
that folder's `Mods`: one whose second side's interface art and font are
missing, one whose unit files are missing, one whose commander's file the
catalog refuses and one that plays whole; and two folders without a
profile: one whose SIDEDATA names the same missing art and font, and one
that lacks a side's LOGO, which the Mods page refuses. It switches to each
of the others through the Mods page, in one run: the folder and the
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
and sound. With a named file Hardware acceleration and Vertical sync are
Off, and the windowed checks' `SDL_RENDER_DRIVER=software` locks both, so no
check draws through the graphics card or waits for the display; game text
keeps the game's own fonts there too, as modern fonts are Off. Five
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

No window of a check opens at the display's own pixel density but
`native-render-tiers-density`'s, which `--native-density` opens so, and
`native-render-tiers-density-setting`'s, which the Native pixel density
setting in its preferences file opens so: on the dummy video driver its
density is 1, and the check holds the match laid out
in window points, read back at the display's size, at zoom 1 equal to the
processor's composition, and picking the unit drawn under the pointer. On a
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
effects and each frame's conversion for the window in bands on spare cores
([job pool](../../src/platform/job-pool/README.md)):
up to four threads, one on a machine of one or two logical processors.
`OA_DRAW_THREADS` (1 to 32) sets the count for every run of the game a test
starts, and `--draw-threads N` for one run; every count draws the same
frames, which `native-draw-threads`, `world-draw-bands`,
`app-xrgb-conversion`, `present-surface-band`,
`model-render-rgb-bridge` and `model-render-mesh-raster`
check. To run the whole suite on one drawing thread, or on many:

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
alone: `screens`, `orders`, `outcomes`, `zoom` (the zoom's limits on the
game's own screen), `zoom-1366x768`, `zoom-1920x1080` and `zoom-2560x1440`
(the zoom's limits on that window) and `campaign`. `--check-navigation`
without a group runs every group in one game. The side commanders check runs
as one test for each part (`--part`): `art`, `rule`, `loopback`, `give`, and
`records-GROUP`, the navigation group over its mod folder.

### The demo's installer

`OA_DEMO_INSTALLER` names the installer of the Total Annihilation demo (1997):
the absolute path of the installer file itself, which the engine recognises by
its size and SHA-256. Set it in the environment or pass it to CMake as
`-DOA_DEMO_INSTALLER=PATH`; like `OA_GAME_DIR`, a build tree keeps the value
it was configured with, and tests receive it at run time. It is the only
setting the demo's tests need: each unpacks the demo's data from the
installer itself. These tests read it:

- `demo-installer-data` unpacks the installer's archive into a temporary data
  folder, checks its size and SHA-256, and checks that a second start reuses
  it and that the unpacked folder is a usable installation;
- `native-demo-installer` starts the game headless, with `--game-dir` on the
  installer's folder and `--data-dir` on a scratch folder, and checks that the
  archive is unpacked, checked and mounted, that every start opens the main
  menu with no dialog over it, that a second start mounts it without
  unpacking it again, that a damaged archive is unpacked again, and that the
  installer's folder is left as it was;
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
  On the way the host's player turns its commander's build page, which
  stays on the host: the joiner's copy of the commander keeps its page and
  the worlds still agree. Then it gives every unit it has through
  SHARE.GUI: the kbot goes, one 0x14 to the joiner, and the commander,
  whose type is in the Commander category, stays on both machines;
  `native-side-commanders-give` gives a commander outside that category, which
  goes too;
  `native-net-loopback-watcher` has the joiner watch, and
  `native-net-loopback-computer` and `native-net-loopback-computer-watcher`
  have the host seat a computer player that builds, is given a squad and
  attacks. Each wait on the other machine counts rounds, each serving both
  machines once, and gives up only after 200 rounds and 15 s with no
  progress (the unit sync shows its records as progress), so a machine
  under load takes longer over the checks without failing them;
- `native-multiplayer-menu` (`--check-multiplayer-menu`) clicks MULTI and
  checks the multiplayer screens;
- `native-recording-hook` (`--check-recording-hook`) checks that the game
  declines bytes that are no recording and refuses a truncated one;
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
and checks them against the frame it composed; and
`native-automation-digest` (`tools/check_native_automation_digest.py`)
plays a recorded network game back in the main loop with and without the
endpoint, a client reading the match and every event at every frame, and
requires the same trace stream.
`native-automation-extension-windows`
(`tools/check_native_automation_windows.py`) starts the game with `--fark`
and `--fixture-window`: the endpoint lists the fixture window's controls, a
click reaches its button, and the window's clock reads the fixed clock's
step times the frame. The fixture is a test extension, so only a build
configured with `-DOA_RECORD_EXTENSION_HOOKS=ON` combines it and runs this
check.

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
  quads, corner alphas and placement and the painters' level quads with no
  renderer at all;
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
  the status saying Full stopped, Off and back trying Full again; the same
  failure in two runs in a row records `full-unusable`, told once at the
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
- `app-touch-paint` checks the touch layer's drawing primitives: rounded
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
(Patrol, its QUEUE hub lighting the 3.1c PATROL button, Info, a greyed
place), building placement (a drag moving the ghost under the finger, a
hold near the ghost placing it there, a double tap or a hold away from it
placing where the finger is, a refused site staying armed, QUEUE keeping the
placement for the next site, CANCEL, CLEAR and a two-finger tap ending
it), the minimap, edge scrolling and auto-scroll, fingers of other
devices, the Cmd keys, the modifiers with no latch, PAUSE and the
lifecycle's pause through
the in-game menu, taps against the same mouse clicks, the overlays kept
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
- `ui-touch-hud` and `app-touch-paint` also check the slim pad HUD, the
  build and group rings, the badges and the glyphs;
- `ui-engine-settings` and `ui-engine-settings-dialog` check the Controller
  section's settings, the Control size and the Steam Deck's defaults;
  `platform-machine` checks how a Steam Deck and Steam's Game Mode are
  recognised;
- `platform-game-installs` builds home folders in a temporary folder with
  Steam, Heroic, Lutris and Bottles installs, and checks the folders found
  and the readers of each library file; `ui-folder-chooser` lays out the
  in-engine folder chooser and checks its focus, keys, presses and
  scrolling; `game-directory` checks resolution with found folders.

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

- `native-game-files`: CHOOSE FOLDER through Ready to copy (its list of
  files left out, a switch off and on), copying and Ready to play to the
  main menu, at 1194x834, with the OA · Aa dialog opened and cancelled on
  the way; the game folder then holds the subset and none of the files left
  out, and it was kept out of the device's backups;
- `native-game-files-phone`: the same in the phone form, at 852x393;
- `native-game-files-demo`: CHOOSE INSTALLER with the demo's installer, its
  game data unpacked (skipped without `OA_DEMO_INSTALLER`);
- `native-game-files-copy-yourself`: I HAVE COPIED IT with nothing there,
  the files copied in by the check under another name, CHECK AGAIN, USE IT;
- `native-game-files-stop`: a copy held to 4 MB a second; after 8 MB the
  game leaves the screen and comes back while it goes on, then leaves again
  until its time away runs out and the copy pauses and starts again on
  return; then STOP, keeping what was copied;
- `native-game-files-resume`: the next start continues that copy, skipping
  the files already copied (it needs `native-game-files-stop`);
- `native-game-files-not-game`: a folder that holds no installation, chosen
  with a mouse click, left with Escape;
- `native-game-files-short-space`: not enough space for the copy, COPY off,
  CHECK AGAIN;
- `native-game-files-manage`: the management state: adding a mod,
  checking, removing the music, DONE;
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
is that profile's sim hash. A Developer Mode override that changes the
simulation changes it at once while no match is running; a running match
keeps the hash it started with until it ends, which is the hash a save of
that match and the network game are matched on. A display-only override
leaves it unchanged. The plain baseline is resolved once a process and
kept: it comes from the rules built into the program alone, so a mod
switch, a game mode or Developer Mode never changes it.
`native-simulation-hash` checks the baseline and an override in one start,
then the first hash again once the settings are put back, and the kept
baseline against a fresh resolve; `native-simulation-hash-mod` checks a
profile that changes the simulation, then the same override.

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

### Other builds

- **Core only**, without SDL or game data: configure with
  `-DOA_BUILD_PLATFORM=OFF -DOA_BUILD_INTRO_PLAYER=OFF`.
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
  what changed.
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

## Kinds of test

| Kind | What it does | Examples |
|---|---|---|
| Unit | Exercises one module with inputs the test builds itself | `sim-detection`, `tdf-parser` |
| Characterisation | Pins what a decoder, raster routine or table does today, edge cases stated byte by byte and seeded sweeps pinned by digest | `present-rle`, `hpi-read-node` |
| Pinned values | Pins digests or bytes that must not move: a whole match, the random streams, the bytes the writers produce | `match-determinism`, `match-shared-random`, `hpi-writer-golden`, `persist-bank-golden` |
| Game data | Reads the installed game named by `OA_GAME_DIR` | `unit-definitions-data`, `installed-content` |
| Native | Runs the game headless over the installation and checks what it does | `native-saveload`, `native-trace` |
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
