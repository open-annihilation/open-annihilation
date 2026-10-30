# Contributing to Open Annihilation

Thank you for helping. This is the developer guide: what the engine does
today, how to build, test and run it from source, how the tree is laid out,
the checks every change passes, how the code is documented, and what
happens between an idea and a merged change. Everyone taking part follows
the [Code of Conduct](CODE_OF_CONDUCT.md).

## Contents

- **Getting started**
  - [Before you start](#before-you-start)
  - [The engine today](#the-engine-today)
  - [Requirements](#requirements)
  - [Build and test](#build-and-test)
  - [Run](#run)
- **Working on the code**
  - [Layout of the tree](#layout-of-the-tree)
  - [Extending the engine](#extending-the-engine)
  - [Documenting code](#documenting-code)
  - [Checks](#checks)
  - [When a source check fails](#when-a-source-check-fails)
- **Getting a change in**
  - [Licence of contributions](#licence-of-contributions)
  - [Pull requests and review](#pull-requests-and-review)
- [Where to read next](#where-to-read-next)
- [Questions](#questions)

## Before you start

- Open Annihilation plays Total Annihilation 3.1c from the player's own
  installed copy and matches the 3.1c game's behaviour and save files. A
  change that alters what a save file holds, which game data is read and how
  it is decoded, or the results of the simulation needs an issue first, so
  that the reason can be agreed before the work is done.
- Game data never enters the repository: no archives, maps, sounds, movies or
  captures of them, apart from the documentation's small screenshots
  ([conventions](docs/development/conventions.md#application-extensions-and-runsh)).
  Tests read the player's installation (see
  [Build and test](#build-and-test)).
- For anything larger than a small fix, open an issue describing the problem
  and the approach, so that nobody's work is wasted.
- Report security problems privately, as [SECURITY.md](SECURITY.md)
  describes, not in a public issue.

## The engine today

The engine is a portable engine that plays **Total Annihilation 3.1c** from
the player's own installed copy of the game, on macOS, Windows and Linux. It
aims to match the 3.1c game's behaviour and its save files: campaigns,
skirmish, computer players, simulation, unit scripts, rendering, sound and
save/load.

It is a work in progress. The frontend, skirmish, campaign and network matches run,
with land units, ships, submarines, hovercraft, aircraft and transports; some
parts of the game are still missing, and unsupported actions report their
status instead of failing silently.

- **Core:** the canonical game records in `src/core` (Game, Unit, Player,
  UnitDef, WeaponDef, FeatureDef, MapPlot) with fixed, asserted layouts;
  `World` owns the live match state.
- **Formats:** HPI archives with the game's mount order, TDF/FBI, GAF,
  TNT/OTA maps, 3DO models, COB unit scripts and PCX.
- **Simulation:** missions and victory conditions, economy, weapons and
  projectiles, damage, ground path finding, ships, aircraft and transports,
  unit scripts, features and meteors, selection and the computer players of
  skirmish and campaign matches.
- **Presentation:** the 8-bit raster, sprite and polygon layer, the GUI gadget
  engine, the world camera, radar and fog, the 3DO model renderer, the audio
  mixer, speech, CD music and the intro movies.
- **Frontend:** main menu, options, skirmish, campaign and briefings, the
  in-game interface and console, and saving and loading a running game.
- **Network play:** the multiplayer lobby and battle room over TCP/IP, the
  network session and the networked match, and the replay of recorded
  games (`.tad`). It is always built and plugs into the game through the
  extension table. It speaks the 3.1c game's own protocol, so players of the
  original game can join in.

## Requirements

CMake 3.24 or newer, a C++20 compiler, zlib and Python 3.12 or newer. The
engine decodes the movies and the music itself. SDL3 is built
locally by `tools/bootstrap_sdl.py`, and FreeType and the fonts that travel
with the game by `tools/bootstrap_text_fonts.py`, into the ignored
`local/deps` directory; nothing is installed globally. The fonts bootstrap
installs the fontTools it cuts the CJK font with into a virtual environment
there, which needs Python's `venv` module. On macOS the Xcode Command Line Tools provide
the compiler and zlib.

## Build and test

Build the pinned SDL, FreeType and fonts once, then configure, build and
run every test:

```sh
python3 tools/bootstrap_sdl.py
python3 tools/bootstrap_text_fonts.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$PWD/local/deps/sdl-install" \
    -DOA_GAME_DIR="/path/to/Total Annihilation"
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
```

Tests state their expected values plainly: constants, the game data of an
installed copy, or behaviour. Tests that need the game data read the
installation named by `OA_GAME_DIR`: the absolute path of the Total
Annihilation 3.1c folder that holds `totala1.hpi`, set in the environment or
passed to CMake as `-DOA_GAME_DIR=PATH`. A build tree keeps the value it was
configured with; an empty entry takes the environment variable on the next
configure, and `-DOA_GAME_DIR=PATH` replaces any other. The tests read the
installation's archives as the game does, so an ordinary installation is
enough. Without one, each of those tests prints what it skipped and exits with
code 77, which ctest reports as skipped, not passed; say in the pull request
if they did not run. Configure with `-DOA_REQUIRE_GAME_DATA=ON` to make a
missing installation fail instead: a test that skips because the installation
lacks optional content, such as a music folder, still reports skipped. The
native checks, which run the game headless over the installation, are added
only when `OA_GAME_DIR` names one.

Two more options serve checking builds: `-DOA_REQUIRE_GAME=ON` stops the
configure when the `oa-game` target cannot be built (it needs the SDL3 shell,
`OA_BUILD_PLATFORM`), and `-DOA_SANITIZERS=ON` builds with AddressSanitizer and
UndefinedBehaviorSanitizer under Clang or GCC, stopping at the first report.
Run a sanitizer build's tests as the CI sanitizer job does, because
`platform-shims` ends a child process with SIGFPE on purpose:

```sh
ASAN_OPTIONS=detect_leaks=0:handle_sigfpe=0 ctest --test-dir build --output-on-failure
```

The core builds without SDL or game data; it still needs FreeType and the
fonts:

```sh
cmake -S . -B build-core -DOA_BUILD_PLATFORM=OFF -DOA_BUILD_INTRO_PLAYER=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel 8
ctest --test-dir build-core --output-on-failure
```

On Windows, install zlib with `vcpkg install zlib:x64-windows`, build SDL with
`python tools/bootstrap_sdl.py`, FreeType and the fonts with
`python tools/bootstrap_text_fonts.py`, and configure with the vcpkg
toolchain:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="$pwd/local/deps/sdl-install" -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

From macOS or Linux, `tools/build_windows.sh` cross-compiles the tree for
x86-64 Windows with mingw-w64, including static zlib and SDL3 (for 32-bit
x86 with `OA_MINGW_TRIPLE=i686-w64-mingw32`, and for Windows XP as well with
`--xp`), and `tools/test_windows.sh` builds it in a container and runs its
tests under Wine. When the host build's `oa-tool` is `build/oa-tool`, the
script passes it as `-DOA_HOST_OA_TOOL`, so the cross build packs Classic
beside the game. An empty `OA_HOST_OA_TOOL` leaves Classic unpacked.
[docs/development/testing.md](docs/development/testing.md) covers the other
builds, what CI runs, and writing a new test.

## Run

```sh
./run.sh
```

`run.sh` builds the `oa-game` target from the current source and launches the
game it builds, `open-annihilation`; a failed build stops it rather than
starting an old executable. On first start the game asks
for your Total Annihilation folder in the system's folder dialog, checks that
it holds the game's archives and remembers it; `--choose-game-dir` asks again,
and `--game-dir PATH` names one without remembering it; `./run.sh` also
takes the `OA_GAME_DIR` environment variable and passes it on as
`--game-dir`. A headless or scripted run never asks and never reads your
remembered folder: it needs `--game-dir PATH`, or a `--preferences-file`
that holds a folder.
`./run.sh --help` lists the options, including `--skip-intro`, `--mute`,
`--headless-check` and snapshots, and network play's
`--play-demo FILE.tad`, which replays a recorded game, and
`--net-loopback-check N`, which hosts and joins a match in one process.

`oa-tool` inspects game archives:

```text
oa-tool list ARCHIVE
oa-tool extract ARCHIVE ENTRY OUTPUT
oa-tool asset-extract ROOT ENTRY OUTPUT [ARCHIVE...]
oa-tool preview ARCHIVE PCX_ENTRY OUTPUT.ppm|OUTPUT.png
oa-tool decode-pcx INPUT.pcx OUTPUT.ppm|OUTPUT.png
```

## Layout of the tree

| Path | Contents |
|---|---|
| `src/core` | Canonical game records shared by every system (C11-compatible headers) |
| `src/base` | Fixed-point arithmetic, geometry and frame timing that every layer above uses |
| `src/platform` | SDL platform layer |
| `src/formats` | Game data formats |
| `src/data` | Game data read through the formats: definitions, campaigns, savegames |
| `src/sim` | Simulation |
| `src/present`, `src/audio`, `src/media` | Presentation and sound |
| `src/ui`, `src/netgame`, `src/session` | Interface, with the multiplayer screens; network play's session and match; the playback of recorded games |
| `src/app` | The game application, `open-annihilation` (the `oa-game` target) |
| `tools` | Bootstrap, launcher and check scripts, `oa-tool` and the `oa-platform` probe |
| `docs` | The conventions, the testing guide and the index of every document |
| `branding` | The Open Annihilation icon and the icons made from it (`tools/make_icons.py`); not under the project's licence (see [COPYRIGHT](COPYRIGHT)) |

A module uses modules of its own row and of the rows above it in this
table, except that the platform layer and the formats do not use each other
and the simulation does not use the platform layer. The `engine-layout`
test (`tools/check_layout.py`) holds the tree to this order.

## Extending the engine

The game takes optional features through a table of function pointers,
`src/app/include/oa/app/extension.hpp`, which each extension library fills
once at startup. Network play is one: the engine always registers its
library, `oa-app-netgame`. A project that adds the engine with
`add_subdirectory` registers its own extension libraries with
`oa_add_extension` (`cmake/OaExtensions.cmake`), and the engine combines
their tables. [src/app/README.md](src/app/README.md) describes the table and
how it changes.

## Documenting code

The rules every change follows are in
[docs/development/conventions.md](docs/development/conventions.md), each with its reason, a good and
a bad example and the directories it covers, and
[docs/development/testing.md](docs/development/testing.md) explains how to run and write tests. In
short: every function has one `///` block directly above its declaration in
the header. It starts with a verb-first summary in the present tense, may
add what a caller needs to know (order of effects, the canonical state it
changes, units), and then has:

- `@param` for every named parameter (leave an unused one unnamed), a
  lower-case phrase with no full stop that states units, written
  `@param[out]` or `@param[in,out]` when the function writes through it;
- `@return` for every function that returns a value;
- `@quirk` for 3.1c behaviour the engine keeps on purpose, described in game
  terms.

Every record field, constant and flag bit is named by what it holds: by the
engine's use of it, by marked evidence (a tentative name has `?` at the
start of its comment), or, for bytes the engine only carries, by their role
and position; none is a numbered placeholder such as `unknown_3`. Comments
add what the name cannot say (units, ranges, truncation); they never restate
the name, and give byte offsets only in file-format code.
Constants have names, and fixed-width types are written unqualified
(`int32_t`, not `std::int32_t`). Comments and documentation describe
behaviour, not how it was worked out:
[the conventions](docs/development/conventions.md#describe-behaviour-never-derivation)
draw the line and give examples, and
[a worked example](docs/development/conventions.md#documenting-code) shows a documented
record and function.

## Checks

The full ctest of [Build and test](#build-and-test) runs every test,
including the source checks (`doc-links`, `style-ratchet`, `format-check`
and `licensing-check`). Run it in a build configured with `OA_GAME_DIR`
before asking for review.

To rerun one check, name it: `ctest --test-dir build -R doc-links`. The
scripts behind the source checks also run on their own, for example
`python3 tools/check_links.py` and `python3 tools/check_style.py`.

## When a source check fails

`style-ratchet` counts, per file, the breaches of the naming and language
rules in [the conventions](docs/development/conventions.md) against
`tools/style-baseline.json`, and a run fails when a count differs from the
baseline. A count that grows is a new finding: it prints as
`path:line: rule: text`; fix it as the rule's section of the conventions
says. A count that fell is slack, which would let a new finding in where an
old one was fixed: lower the baseline in the same change with
`python3 tools/check_style.py --update`. Counts never go up.

`format-check` fails on a source that is not laid out as `.clang-format`
says, and `licensing-check` on a file that states no copyright and licence:
run `python3 tools/format_sources.py` and `python3 tools/spdx_headers.py`,
which fix both (see [the conventions](docs/development/conventions.md#formatting)).

The `doc-links` check fails when a relative Markdown link, an anchor or a
repository path written in backticks does not resolve; fix the link or the
path.

## Licence of contributions

Open Annihilation is released under the GNU General Public License version 3
only (see [LICENSE](LICENSE) and [COPYRIGHT](COPYRIGHT)). By opening a pull
request you agree that your contribution is released under the same licence;
you keep the copyright in it. Commit with an email address that is linked to
your GitHub account, so that your commits are credited to you.

## Pull requests and review

1. Fork the repository and branch from `main`.
2. Keep each pull request to one concern. Keep commits small: each one builds
   and passes the tests on its own.
3. Write commit messages as the history does: a subject line that says in
   plain English, in the imperative, what the change does (under about 72
   characters, no full stop), a blank line, then a short body wrapped at 72
   columns that says why.
4. Describe the pull request the same way: what it changes and why, and how
   you tested it, including whether the tests that read game data ran.
5. Run every check (see [Checks](#checks)) before asking for review.
   Continuous integration builds the tree on macOS, Windows and Linux,
   starts `open-annihilation` on each, and runs the tests that need no game
   data. The tests
   that read game data and the native checks, which run the game headless
   over an installation, run only on your machine.
6. A maintainer reviews the change. Answer comments by pushing further
   commits to the same branch; there is no need to rewrite its history.
7. Pull requests are merged by squashing, so the pull request's title and
   description become the commit message on `main`. Maintainers check the
   title, the description and every commit message against the same wording
   rules as the sources (see
   [the conventions](docs/development/conventions.md#describe-behaviour-never-derivation)).

## Where to read next

1. [docs/development/conventions.md](docs/development/conventions.md): the code conventions in
   full, and [docs/development/testing.md](docs/development/testing.md): running and writing tests.
2. [src/core/README.md](src/core/README.md): the canonical game records
   every system shares.
3. [src/app/README.md](src/app/README.md): the game application, how to add a
   screen or overlay, and the extension table.
4. The README of the module you are changing, such as
   [src/formats/tdf/README.md](src/formats/tdf/README.md) or
   [src/sim/match-runtime/README.md](src/sim/match-runtime/README.md),
   and its `tests/` directory. [docs/index.md](docs/index.md) lists every
   document, the module READMEs among them.
5. [ATTRIBUTIONS.md](ATTRIBUTIONS.md): the third-party components and their
   licences, before you add a dependency.

## Questions

Ask in a GitHub issue on this repository, or on the Open Annihilation
Discord server, <https://discord.gg/KyUExvCnQ>. Credits are in
[CONTRIBUTORS.md](CONTRIBUTORS.md).
