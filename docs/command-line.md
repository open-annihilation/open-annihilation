# Command Line and Game Launching

Open Annihilation takes options on its command line, and the switches of
Total Annihilation 3.1c's command line, so a launcher or a shortcut written
for 3.1c starts it the same way. This page lists every option, by what it is
for, and every way to start a network game from another program, a script
or a shortcut: a launch file, options alone, TA Forever's settings file, a
launch piped in on standard input, and a launch file opened from the
desktop. `open-annihilation --help` lists the same options.

## Contents

- [Starting the program](#starting-the-program): where the program is, and
  how it reads its options and 3.1c's switches
- [Every option](#every-option), by group: information; game data, folders
  and files; mods; mutators; display and sound; network play; launch; status;
  recordings and video; checks; tools
- [Ways to launch a game](#ways-to-launch-a-game), and
  [examples](#examples) to copy
- [Launch files](#launch-files): the `.oalaunch` file, group by group, with
  a full example
- [Launch settings](#launch-settings): every setting of a launch, its field
  and its option
- [Standard input and pipes](#standard-input-and-pipes): Linux, macOS and
  Windows
- [Opening a launch file](#opening-a-launch-file): the file type, and what
  opening one does
- [Listing what is installed](#listing-what-is-installed):
  `--list-content --json`
- [The 3.1c command line](#the-31c-command-line): 3.1c's switches, the `-c`
  file and where this game differs
- [Exit statuses and refusals](#exit-statuses-and-refusals)

## Starting the program

The program is `open-annihilation`:

| System | The program | Where options go |
|---|---|---|
| Windows | `open-annihilation.exe`, in the install folder or the folder you unzipped | after the program's name in a Command Prompt, or in a shortcut's **Target** |
| macOS | `open-annihilation`, inside the app: `Open Annihilation.app/Contents/MacOS/open-annihilation` | after `open -a "Open Annihilation" --args`, or after the program inside the app, in Terminal |
| Linux and Raspberry Pi | `open-annihilation`, in the package's folder | after the program's name in a terminal |
| Steam Deck | as on Linux | in the game's **Launch Options** in Steam ([Steam Deck](installation/steam-deck.md)) |
| iPhone and iPad | the app you build with Xcode | in the scheme's **Arguments** in Xcode ([iPhone and iPad](installation/ios.md)) |

The examples on this page write the program as `open-annihilation`. On
Windows, in a Command Prompt in the game's folder:

```bat
open-annihilation.exe --version
```

On macOS, in Terminal, either of:

```sh
open -a "Open Annihilation" --args --skip-intro
"/Applications/Open Annihilation.app/Contents/MacOS/open-annihilation" --version
```

`open -a` starts the app as Finder does. The options after `--args` reach
the game only when `open` starts it: a copy that already runs is brought to
the front and gets none. `open` does not connect the game to Terminal, so
what the game prints goes to its log, and nothing typed or piped in Terminal
reaches it. The program inside the app is the same game, started by
Terminal itself: it prints to Terminal and reads its standard input.

On Linux, in a terminal in the game's folder:

```sh
./open-annihilation --version
```

### How options and switches are read

- An option begins with `--`, such as `--skip-intro`. The options are this
  game's own. One that takes a value takes the next word: `--game-dir PATH`.
  A few take theirs after `=` instead: `--hardware-acceleration=basic`,
  `--quit-after-game=after-score`, `--tournament=off`, `--closed=off` and
  `--lock-options=off`.
- A word that begins with `-` or `/` and a letter, such as `-c` or `/d`, is
  one of 3.1c's switches ([The 3.1c command line](#the-31c-command-line)).
  Any other word names the language, as in 3.1c: `german`.
- A `.oamod`, `.oalang`, `.oamap`, `.oamut`, `.oabrain`, `.oakeys` or `.oareg` file named on
  its own is opened
  as `--open FILE` opens it. A launch file is named with `-c` or `--launch`:
  on its own, its name is just a word of 3.1c's command line, and no launch
  starts.
- The options are read first, wherever they stand. They win over the
  switches and over a launch file, whatever their order.
- Each argument the system passes is one word, spaces and all, so a path in
  quotes is one path.
- `--version` prints one line and ends before anything else is read.
  `--help` prints every option and ends.
- An option the game does not know, or a value it cannot use, ends the game
  with status 1 and one line that names it:
  `open-annihilation: unknown option: --nope`. A launch the game cannot use
  ends it with status 2. [Exit statuses and refusals](#exit-statuses-and-refusals)
  lists them all.

## Every option

`open-annihilation --help` lists these options, and the switches of
[the 3.1c command line](#the-31c-command-line), as one line of usage. Each
group below is a table of its options. The options of the last two groups,
[checks](#checks) and [tools](#tools), are for the engine's tests and its
developers; a player needs none of them.

### Information

| Option | What it does |
|---|---|
| `--version` | Prints the version line and ends with status 0. [Below](#--version). |
| `--help` | Prints every option and switch, as one line of usage, and ends with status 0. `-h` as the last word does the same. |
| `--list-content --json` | Prints the installed maps, mods and expansions as one line of JSON and ends ([Listing what is installed](#listing-what-is-installed)). |
| `--print-profile` | Prints the mod profile a run would play and the version line, and ends ([Mods](#mods)). |

#### `--version`

```sh
open-annihilation --version
```

Prints one line and exits with status 0. It does not open a window, look for
the game data, or read or write preferences or a log file.

```text
Open Annihilation 0.8.0 (macos-universal arm64, 2ac0217b)
```

The version is this release's. The platform is the name the release packages
use (`macos-universal`, `windows-x64`, `windows-x86`, `windows-arm64`,
`linux-x86_64`, `linux-arm64`, `linux-armhf`), then the architecture that is
running (`arm64`, `x86_64`, `x86` or `arm`). The last word is the commit the
build was made from, eight digits. On Windows the program writes the line to
the console it was started from.

`oa-tool --version` prints the same line.

### Game data, folders and files

| Option | What it does |
|---|---|
| `--game-dir PATH` | Plays the Total Annihilation folder at `PATH` for this run, as it stands, without remembering it. It cannot be given with `--choose-game-dir`. |
| `--choose-game-dir` | Asks for the Total Annihilation folder even when one is remembered, and remembers the answer. A run nobody watches cannot ask, and refuses it. |
| `--content GAME[:EXPANSION,...]` | The base game and expansions this run plays, by their ids, such as `totala:core-contingency,battle-tactics`, in place of the Library's choice, for this run only ([game content](game-content.md)). |
| `--archive PATH` | Reads this archive in place of the archives the game folder holds; give it once for each archive. A relative path is read from the game folder. |
| `--user-folder PATH` | Your Open Annihilation folder, which holds saved games, screenshots, films, recordings and mods, is `PATH` for this run. |
| `--preferences-file PATH` | Reads and writes the preferences in `PATH` instead of the usual file. Your Open Annihilation folder is then the one beside it ([settings](settings.md#where-the-settings-are-kept)). |
| `--data-dir PATH` | Keeps the data the game unpacks for itself in `PATH`, in place of the system's per-user data folder. |
| `--log-dir PATH` | Writes this run's log into `PATH`, whatever kind of run it is. What the game prints goes into the log too; only `--status-stdout`'s lines stay on standard output. |
| `--open FILE` | Opens a `.oamod`, `.oalang`, `.oamap`, `.oamut`, `.oabrain`, `.oakeys` or `.oareg` file once the main menu shows, as opening it from the desktop does; give it once for each file. |
| `--install-mod FILE` | The same as `--open FILE`. |
| `--no-game-files-screen` | With no usable game folder, shows the notice with its look-again button instead of the Game files screen, where a system offers both ([game files](game-files.md)). |

The installation guides say where the usual folders are
([macOS](installation/macos.md#where-it-keeps-its-files),
[Windows](installation/windows.md#where-it-keeps-its-files),
[Linux](installation/linux.md#where-it-keeps-its-files)).

### Mods

| Option | What it does |
|---|---|
| `--mod-dir PATH` | Plays the mod folder at `PATH` for this run. It cannot be given with `--base-game`. |
| `--base-game` | Plays without the mod you chose, for this run. |
| `--mod FILE` | Reads the mod profile from `FILE` instead of the folder's own `oamod.yaml`, for development and for mods that ship none. |
| `--print-profile` | Prints the profile a run would play, and the version line, and ends. [Below](#--print-profile). |
| `--accept-unimplemented-hacks` | Accepts, with a warning each, the hacks this build does not implement yet, for development. It needs a mod: `--mod`, `--mod-dir`, `--game-dir` or `--print-profile`. |

[The OAMOD standard](mods/oamod-standard.md#43-command-line-options) says
more about each, and [mods](mods/README.md) how a mod is chosen.

#### `--print-profile`

`--print-profile` prints the resolved mod profile and then the same version
line, and exits. It needs `--mod FILE`, `--mod-dir PATH` or `--game-dir PATH`.
The profile comes first, so a program that reads the profile's JSON still
finds it at the start of the output. The version line is the last line.

### Mutators

| Option | What it does |
|---|---|
| `--mutator PATH\|ID[:OPTION=VALUE,...]` | Applies one mutator to every match this run starts. Repeat it for several, in the order given, each acting on the game as the ones before it left it. `PATH` is a package folder, one that holds `oamut.yaml`. `ID` is a mutator installed in the Mutators folder. Options follow the first `:` that is followed by a name and `=`, as `name=value` pairs joined by `,`. A path that starts with a drive letter, `:` and `/` or `\` keeps that `:` on every platform, so `C:\Mods\pack:factor=4` is the folder `C:\Mods\pack` with `factor` set to 4. A toggle is `true` or `false`, a choice one of its values, and a number a step inside its range. |
| `--no-mutators` | Plays this run with no mutators. A list `--mutator` or a launch file names is refused and logged, even one that is not installed, and no mutator is applied. The last set of each setup is left as it is. |

`--mutator packages/ridge-swift:factor=4` plays the package at that path
with `factor` set to 4. `--mutator ridge-swift --mutator ridge-tough`
plays two installed mutators, ridge-swift first.

A value that is not that form, including an id followed by `:` and no
option, is refused with status 1. A mutator that is not installed, an
option it does not have, a value outside its choices, or a list the
manifests refuse (one id twice, a prerequisite that is not above it, or
two that conflict) is refused with status 2, before the game starts. An
id is refused the same way when no player folder can be worked out.

### Keys

| Option | What it does |
|---|---|
| `--keys ID\|PATH` | Plays this run with one key layout. `ID` is a layout installed in the Keys folder, or `oa-default` for the game's own layout. `PATH` is a folder holding `oakeys.yaml`, that file itself, or a `.oakeys` file. A `.oakeys` file is read in memory. The player's own changes are not laid on top. Nothing is saved: not the layout preference, not an own layout, and not which mods have had their stored keys carried over. |
| `--keys-reset` | Plays this run with `oa-default` and no own changes. Nothing is saved. |

`--keys` and `--keys-reset` together are refused with status 1. An id `--keys` names that is not installed, a path it cannot read, a manifest with an error, or a `.oakeys` file the installer refuses, ends the run with status 2 before the game starts; each problem is printed, and nothing is installed or written. `oa-default` is the built-in layout, not an installed folder. A layout named in the preferences, rather than by `--keys`, that is not installed is left unused for that run and `oa-default` is played instead: that does not end the run.

### Display and sound

| Option | What it does |
|---|---|
| `--skip-intro` | Skips the logo and the intro movie. |
| `--mute` | Plays no sound. |
| `--first-run` | Shows the welcome at the first main menu again, even when it is done ([first run](first-run.md)). |
| `--resolution WxH` | Opens the window at that size, for this run only, such as `--resolution 1280x720`. |
| `--max-fps N` | The most frames a second, from 30 through 1000, or 0 for no limit, for this run. |
| `--hardware-acceleration[=off\|basic\|full]` | Sets Hardware acceleration for this run; alone it is `full`. |
| `--no-hardware-acceleration` | Turns Hardware acceleration off for this run. |
| `--experimental-3d` | Accepted for older launch scripts; camera access is now controlled by Graphics → Allow Full 3D Mode, On by default. |
| `--native-density` | Opens the window at the display's own pixel density for this run. |
| `--touch-controls` | Turns the touch controls on with no touch screen ([touch controls](touch-controls.md)). |

None of these is remembered: [settings](settings.md#for-one-run-the-command-line)
says what each one shows in Settings. 3.1c's `-d` and `-df` choose a window
or full screen, and `-s` and `-w` turn the sound off
([The 3.1c command line](#the-31c-command-line)).

With **Allow Full 3D Mode** enabled in Graphics, a match starts at the classic view. On a Mac,
hold Option and click-drag to orbit; add Shift to pan. Option+R resets to
classic, and Option+U toggles the saved Unlocked Camera setting. These gestures
work with a trackpad's primary click or an ordinary mouse. On other
platforms use Alt in place of Option. Alt/Option+middle-drag also orbits;
middle-drag pans when middle-click pan is enabled.
While rotated, selection and unit orders use the 3D camera; mouse-wheel
zoom keeps the terrain point under the pointer anchored. Shift shows projected
order queues and queued build sites. Escape still opens the menu. Enhanced AA and shadow
settings apply, subject to the preview's rendering limits. The 3D
renderer needs Full hardware acceleration and a graphics card SDL can draw
on through Metal or Vulkan. See the [preview notes](development/pluggable-renderers.md#interactive-skirmish-preview)
for limitations and a weapon-effect test recipe.

### Network play

| Option | What it does |
|---|---|
| `--host` | Hosts a TCP/IP game at once and opens its battle room. |
| `--join ADDRESS` | Joins the game at `ADDRESS`, an IPv4 address or a host name, at once, and opens its battle room. |
| `--player-name NAME` | Your name in the game. |
| `--game-name NAME` | With `--host`, the game's name; with `--join`, the game to join when several answer at the address. |
| `--game-password PASSWORD` | With `--host`, the game's password; with `--join`, the password sent. |
| `--dplay-guid GUID` | The application GUID this game hosts under and looks for. [Below](#--dplay-guid). |
| `--dplay-port PORT` | The DirectPlay port this game hosts on and looks for games at, from 1 through 65535, in place of the standard 47624. 0 is any port the system has free, for a game that hosts alone, as the battle room checks do. |
| `--play-demo FILE.tad` | Plays back a recorded network game ([recordings](../README.md#demo-recorder)). |
| `--demo-unit-table strict\|ignore` | `ignore` plays a recording back even when its units differ from the game's, for diagnosis only; `strict`, the default, refuses it. |
| `--net-record FILE` | Records the next network game to `FILE`, replacing it, whatever the mod's profile says. |
| `--replay-viewer` | For joining a game in which another player's replayer plays a recording to those who join: the game presents itself as a viewer of that replay ([recordings](../README.md#demo-recorder)). |

`--host`, `--join` and the names, the password and the DirectPlay options
are launch settings too: [Launch settings](#launch-settings) gives each one's
field and limits, and any other launch setting may stand beside them. The
README's [network games from the command line](../README.md#network-games-from-the-command-line)
says what the game prints as it hosts or joins.

#### `--dplay-guid`

`--dplay-guid GUID` sets the application GUID this game hosts under and
looks for. A search lists a game only when that game was hosted under the
same GUID, and a game this one hosts answers a search only when the search
asks for that GUID.

Without the option the GUID is Total Annihilation's own,
`{99797420-F5F5-11CF-9827-00A0241496C8}`. Games then stay visible to 3.1c
and to Open Annihilation started without the option. Nothing else about
hosting or joining changes.

The GUID is written with or without braces, in any case:

```text
--dplay-guid {99797420-F5F5-11CF-9827-00A0241496C8}
--dplay-guid 99797420-f5f5-11cf-9827-00a0241496c8
```

Those two are the same GUID. Anything that is not a GUID of that form is
refused, with the message `--dplay-guid expects a GUID, with or without
braces`, and the game ends as it does for any other refused option.

A launcher such as TA Forever starts each mod under a GUID of its own, a
UUIDv5 of Total Annihilation's GUID and the mod's name. Pass that GUID and
this game hosts under it and lists only games hosted under it. A player of
that mod on 3.1c, started with the same GUID, then sees the game. A player
looking under Total Annihilation's own GUID does not. For a mod such as
Ridge, the GUID is the one the launcher uses for Ridge; this game does not
derive it.

`--dplay-guid` stands with `--host` and `--join`. It needs neither: a game
opened from the multiplayer screens uses it too.
See [which games are listed](multiplayer/application-guid.md).

### Launch

| Option | What it does |
|---|---|
| `-c FILE` | Starts a network game from a launch file: an `.oalaunch` file or TA Forever's settings file. `-c -` reads it from standard input. This is 3.1c's switch ([below](#the-31c-command-line)). |
| `--launch FILE` | The same as `-c FILE`; `--launch -` reads standard input. |
| the launch settings' options | `--map NAME`, `--metal N`, `--computer-player SIDE` and the rest, one for each setting: [Launch settings](#launch-settings). |
| `--list-content --json` | Prints the installed maps, mods and expansions, for a launcher to choose from ([Listing what is installed](#listing-what-is-installed)). |

[Ways to launch a game](#ways-to-launch-a-game) shows how they work
together.

### Status

| Option | What it does |
|---|---|
| `--status-stdout` | Writes what the game is doing, and how it ended, as one line of JSON for each event on standard output. |
| `--status-file PATH` | Writes the same lines to a file. |
| `--quit-after-game[=after-score]` | Leaves once the game has ended, or, with `=after-score`, once the player has left the score screen. |
| `--fark` | Serves the automation endpoint, through which a program on this machine drives the game ([automation](automation.md)). |
| `--fark-address 127.0.0.1:PORT\|[::1]:PORT` | The loopback address and port the endpoint listens at. It needs `--fark`. |
| `--fark-file PATH` | Where the endpoint writes its address and token. It needs `--fark`. |

`--status-stdout` writes what the game is doing, and how it ended for
each player, as one JSON object a line on standard output. The game's
own log goes to the log folder, so the lines are the only text there.
`--status-file PATH` writes the same lines to a file. Both options
together write every line to both. `--quit-after-game` leaves once the
game has ended. `--quit-after-game=after-score` waits until the player
has left the score screen. A launch file's `status` group turns the same
options on.

A missing file, or a file the game cannot open, is refused, and the game
ends as it does for any other refused option, before the first line.
See [what a game reports as it is played](multiplayer/status.md).

### Recordings and video

| Option | What it does |
|---|---|
| `--capture-video PATH.mp4` | Records what the window shows to an MP4 video; it needs the `ffmpeg` program ([capturing video](capture.md)). |
| `--showcase arm-first-mission\|skirmish-battle\|base-construction` | Plays a scripted run for a showcase video, in place of a player ([capturing video](capture.md)). |
| `--generate-script RECORDING` | Plans a director script's camera shots from a recorded game ([director scripts](director.md)). |
| `--render-script PATH.oascript\|PATH.oamovie` | Renders a director script, or a bundle of one and its recording, to video with its sound ([director scripts](director.md)). With `--hardware-acceleration=full` its frames are drawn by the graphics card's Full tier, with no window ([drawing on the graphics card](director.md#drawing-on-the-graphics-card)). |
| `--output PATH` | Where `--generate-script` writes its script or bundle, or the folder `--render-script` renders into. |
| `--chunks A-B` | The chunks `--render-script` draws, counted from 0. |
| `--stills F,F...` | The frames `--render-script` also writes as pictures, counted from 0. |
| `--effect NAME=PERCENT` | The percent `--render-script` draws an effect at, such as `heat-distortion=0`, in place of the run's settings; once for each effect ([the effects](director.md#drawing-on-the-graphics-card)). |

`--play-demo` and `--net-record` are under [Network play](#network-play).

### Checks

The checks drive the game by themselves, with no player, and end with
status 0 when they pass and 1 when they fail; one that cannot run on this
machine, such as one that needs 2 GiB of memory, ends with 77. The engine's
tests run them ([testing](development/testing.md)). Check and Debug builds
hold the self-checks, the `--check-` options, `--reclaim-check` and
`--weapon-fire-check`; the
release packages leave them out and refuse them with a line that says so.
Network play's own checks, the last rows, are in every build.

| Option | What it does |
|---|---|
| `--headless-check` | Runs without a window or sound: reads the game data and checks the main menu, or plays the run the other options set up. |
| `--frames N` | Ends the run after N frames of the main menu's loop. |
| `--snapshot PATH.ppm` | Writes a picture of what a headless run ends on, such as the main menu, to `PATH`; a check writes its own pictures beside it. |
| `--display-modes MODES` | A made-up monitor for a check on SDL's dummy video driver: `WIDTHxHEIGHT[@RATE][/DENSITY]` modes, separated by commas, or `none`. |
| `--force-capable` | Counts the renderer as one the graphics card can scale frames on, for `--check-render-tiers`, `--check-build-preview`, `--check-dialog-backdrop`, `--check-engine-settings` and `--check-kill-board` only. |
| `--check-navigation [GROUP]` | Walks the menus and a skirmish through their keys and buttons: every group, or one of `screens`, `orders`, `outcomes`, `zoom`, `zoom-1366x768`, `zoom-1920x1080`, `zoom-2560x1440` and `campaign`. |
| `--check-match-dialogs` | Opens the help dialog over a live match, then the briefing and the in-game dialogs, in the language the preferences file names, such as an installed pack's. |
| `--check-match-layers` | Checks the presented match frame against the processor's composition. |
| `--check-render-tiers` | Checks the accelerated drawing of the main menu and a skirmish against the processor's. |
| `--check-match-orders` | Clicks the order page's buttons and checks what the units do. |
| `--check-side-orders` | Clicks each side's order page buttons, whatever prefix their names carry, and checks what the units do. |
| `--check-skirmish-sides` | Clicks the skirmish Side button through every offered side, and back when the game has more than five, and checks the face each side shows. |
| `--check-mod-limits` | Opens Settings on Engine Limits at the menu and in a two-slot skirmish and checks the weapon-type and side rows. |
| `--check-side-data-per-match` | Plays skirmishes and checks each match reads side data again when `gamedata/sidedata.tdf` has changed. |
| `--check-gadget-art-per-match` | Plays three skirmishes and checks a download button's picture appears only in the match whose mod folder holds it. |
| `--check-factory-orders` | Gives a Kbot Lab a move and checks the unit it builds carries it out. |
| `--check-unit-speech` | Selects and orders the commander and checks what it says. |
| `--check-mutator-layer` | Plays skirmishes with the mutators `--mutator` named and checks the commanders, and that a mutator which does not apply leaves no layer. |
| `--check-mutator-verdicts` | Writes four made-up mutators and checks their verdicts against the installed game, then again from the verdict file. |
| `--check-mutator-dry-run` | Writes two made-up mutators that stack commander turn rate and worker time, and checks each field is held at 65535 with one note. |
| `--check-replay-loadout` | Checks a recorded loadout against the installed mutators, plays it for one skirmish and puts the player's own list back. |
| `--check-download-builds` | Builds a unit from a download page, and checks the queued counts on its buttons. |
| `--check-stockpile-builds` | Queues and builds rounds for every unit that stockpiles, through a save and a load. |
| `--check-unit-page-memory` | Checks each unit keeps its order page through reselection, a save and a load. |
| `--check-side-column` | Checks the commander's build pages fit the side column at several window sizes. |
| `--check-match-bars` | Checks the top and bottom bars reach the window's edge at several shapes. |
| `--check-unit-pages whole\|scaled:TYPE,...` | Checks the named unit types' pages at several window sizes. |
| `--check-kill-board` | Checks the kills board F4 shows. |
| `--check-paused-save` | Saves a paused skirmish, loads it and checks it runs. |
| `--check-saved-brains` | Saves a skirmish a brain plays, loads it with that brain's state, and asks to load with the built-in player when the brain is missing. |
| `--check-simulation-hash` | Checks the simulation hash with no mod, with a mod and after a Developer Mode override. |
| `--check-zoom-memory` | Zooms a large battlefield out and back with a small model budget. |
| `--check-unit-language TAG` | Checks units' names and descriptions show in the language `TAG` names. |
| `--check-language-switch` | Changes the language over the main menu and over a skirmish, from Simplified Chinese, or the installed pack's language `--check-language TAG` names, to English and German and back. |
| `--check-language-registry` | Lists every language the game knows. |
| `--check-language-notice` | Shows the notice for a language that is only available to download. |
| `--check-first-run` | Walks the welcome at the first main menu. |
| `--check-setup-window` | Walks the window before the game: the language, the game and the copy. |
| `--check-window-place` | Checks the next start opens the window at the same size and place. |
| `--check-progress` | Shows the progress dialog for a slow piece of work. |
| `--update-feed URL` | For one run, looks for a new release at this feed instead of the published one. A self-check. |
| `--update-key ID=ed25519:KEY` | A key that may sign the `--update-feed` feed. Give it once for each key. Needs `--update-feed`. A self-check. |
| `--check-engine-feed` | Checks looking for a new release against a feed the check serves. |
| `--updated-from VERSION` | Every build. The version this start replaced. The first settled main menu shows "Updated to …" once when this copy is newer. |
| `--rolled-back-from VERSION` | Every build. The version this start replaced by rolling back. Used with `--roll-back-reason`. The first settled main menu says "Back on …". |
| `--roll-back-reason failed-starts or player` | Every build. Why the roll back happened. Used with `--rolled-back-from`. |
| `--check-update-roll-back` | Checks confirming a version, the roll-back prompt, Settings' Roll back… and that a check is not counted as a start. The confirm case also leaves the receipt of a failed install and checks the recovery screen shows it once and holds its release. |
| `--roll-back-case NAME` | With `--check-update-roll-back`, which part to run: `guard`, `confirm`, `prompt` or `settings`. Needs `--check-update-roll-back`. |
| `--update-install-folder DIR` | A self-check. With `--update-feed`, the route is that folder, so a download can be handed to the helper in a test. Needs `--update-feed`. |
| `--update-and-quit` | A self-check. Needs `--update-feed`. At the settled main menu, downloads where there is a route, with no prompt, and quits when the file is ready so the hand-off follows. With no offer or no route, quits with status 1. |
| `--check-update-offer` | Shows the update question against a feed the check serves, including its notes, and checks it waits out a match, a message and Settings. |
| `--check-update-download` | Serves a feed and a package and checks the download, Pause and Resume from the main menu's update pill and from the update centre it opens, the pause during a match, the ready prompt, Settings and the recorded hand-off. |
| `--update-download-case NAME` | With `--check-update-download`, which part to run: `corrupt` (a package that fails its check is deleted and nothing is queued), `background` (Download automatically fetches with no offer, the ready prompt shows first, and Keep it ready schedules nothing), `no-offer` (with `--update-and-quit`, a feed with nothing newer), `cancel` (Cancel download in the update centre keeps no part and no file and goes back to the offer) or `pictures` (draws every update screen in each state at 640x480, 1280x720 and 1920x1080 from made-up states, writes `update-<screen>-<state>-<w>x<h>.png` to its folder and fetches nothing). With none, the whole download is checked. Needs `--check-update-download`. |
| `--check-icloud-wait` | On macOS, holds one evicted archive's read and checks the window keeps drawing the iCloud line, then continues. Elsewhere, checks the line never names iCloud. |
| `--check-text-fit` | Opens every Open Annihilation screen in each built-in language and Simplified Chinese at two sizes, and fails when a text does not fit. |
| `--check-language TAG` | With `--check-text-fit`, the language to check in place of those, such as an installed pack's (`uk`); give it once for each. With `--check-language-switch`, the language it starts in. The language must be one the game shows. |
| `--check-folder-chooser` | Chooses the game folder in the game's own locator. |
| `--check-patrol-reclaim` | Checks a construction kbot on patrol reclaims on its way. |
| `--check-reclaim-cursor` | Checks the cursor over trees, features and wrecks. |
| `--check-wreck-pick` | Checks a sunken wreck and a wreck on high ground are picked where they are drawn. |
| `--check-underwater-features` | Draws metal patches under the water, in open water, at a shore and at the edge of a unit's sight, in every tier, and checks each against the processor's picture. |
| `--check-click-snap` | Checks the cursor RECLAIM shows beside a wreck, and where a RECLAIM click there and a metal extractor's build click beside a deposit land, with `ui.click-snap` on or off. |
| `--check-build-preview` | Checks the building drawn under the build cursor. |
| `--check-dialog-backdrop` | Checks that the briefing, the save dialog and the help dialog over a paused match leave the battlefield as the match showed it, in every tier. |
| `--check-megamap-clicks` | Checks clicks on the megamap. |
| `--check-single-player-offers` | Opens Single Player and checks New Campaign and Any Mission when a side has a campaign, and when neither side has one. |
| `--check-radar-orders` | Checks presses on the radar for every order. |
| `--check-pointer-interfaces` | Drives the pointer's buttons in both interface types. |
| `--check-key-latches` | Checks a latched Shift and Space, then sticky Ctrl and Alt, on a skirmish. |
| `--check-hold-confirm` | Checks Hold to confirm: self-destruct, the order button and the surrender question, on a skirmish. |
| `--check-order-wheel` | Checks the order wheel: a held right or middle button opens it, a short click and a drag do not, on a skirmish. |
| `--check-key-table FILE` | Drives every key, click and wheel turn the match answers and compares what each did with FILE. |
| `--check-key-timing` | Checks slow keys, bounce keys, the double-press time, the player's key repeat and the drag threshold. |
| `--check-pointer-keys` | Drives the pointer from the numpad on the main menu and in a skirmish. |
| `--check-pad-controls` | Drives the gamepad controls with SDL's virtual pads ([controllers](controllers.md)). |
| `--check-touch-controls` | Drives the touch controls with finger events ([touch controls](touch-controls.md)). |
| `--check-running-while-inactive` | Checks the game keeps running while its window is inactive, when it must. |
| `--check-game-files` | Drives the Game files screen through a route ([game files](game-files.md)). |
| `--game-files-route folder\|demo\|copy-yourself\|manage` | The route `--check-game-files` takes. |
| `--game-files-expect main-menu\|stopped-kept\|resumed\|not-a-game\|short-space\|next-start` | What `--check-game-files` expects the route to reach. |
| `--game-files-source PATH` | The folder or installer the check's picker answers with. |
| `--game-files-free-bytes N` | The free space the check reports. |
| `--game-files-copy-rate BYTES` | The bytes a second the check's copy is held to. |
| `--game-files-stop-after BYTES` | Presses STOP once the check's copy has copied this much. |
| `--check-content-copy PATH` | Copies a small fixture into `PATH` and checks the copy, and does nothing else. |
| `--check-installer FILE` | Lists the bundles a recognised installer holds and every file's SHA-256, and copies nothing. |
| `--check-multiplayer-menu` | Clicks MULTI and checks the multiplayer screens. |
| `--check-load-save` | Checks the save and load dialogs. |
| `--check-frontend-controls` | Clicks every option of the skirmish, campaign and options screens. |
| `--check-scroll-bars` | Drives the scroll bars of the lists and the options. |
| `--check-engine-settings` | Drives Open Annihilation's own settings. |
| `--check-registered-kit` | Drives a settings page, a screen, a notice and a prompt registered from inside the engine. |
| `--check-kit-gallery` | Opens the UI kit's gallery at three window sizes. |
| `--check-user-folder` | Drives your Open Annihilation folder and its buttons in Settings. |
| `--check-mod-switch` | Switches the mod thirteen times and checks the memory in use stays level. |
| `--check-map-packs` | Checks which maps of two map packs the skirmish list shows. |
| `--check-map-pack-lists` | Installs a map pack while both map lists exist. |
| `--check-mod-map-settings` | Checks a mod's settings for a map pack's map, with `--mod-dir`. |
| `--check-map-browser` | Checks the map browser at four window sizes. |
| `--check-key-editor` | Checks the key editor at 640x480, 1280x720 and 1920x1080, on the menus and in a paused skirmish. |
| `--check-key-capture` | Captures a chord and settles a clash in the key editor, over a paused skirmish: the clash question's outcome written only by its confirm button, Escape held for a second, Find by key, and a draft clash settled from Settle…. |
| `--check-key-views` | Checks the key editor's keyboard and mouse at 640x480, 1280x720 and 1920x1080. |
| `--check-keys-overlay` | Checks the keys overlay in a skirmish, with `--mod-dir` and `--user-folder`: Primary+/ and Escape, EDIT KEYS…, the order and build cell badges, the help lines, the rotate hint and a latched QUEUE in the overlay. Then the overlays' places: Settings › Accessibility's Move overlays, off by default, turned on; Arrange… opening a picture of the match on the main menu and in a match, its handles dragged on the 8-point grid, Cancel and Reset; the keys overlay, a latched QUEUE's chip and the chat line at their places in a second skirmish, and back where they lay themselves out with Move overlays off; the in-game menu's OA Settings button as wide as Resume. Run again on the same preferences, it finds the keys overlay and the chat line where they were left. With `--snapshot`, writes the overlay and the badges at 640x480, 1280x720 and 1920x1080, Arrange Overlays in both looks at 640x480, 800x600 and 1280x720, and the placed overlays, the in-game menu and the rounded Settings at 640x480 and 1280x720. |
| `--check-selection-keys` | Presses each of 3.1c's Ctrl+letter keys in a skirmish as the system sends them, with the pointer moving between the key's events, and checks the selection each makes from the units' Category words (Ctrl+R the radar, sonar and jammer units, with their rings on the minimap; Ctrl+P the combat aircraft), Ctrl+Shift+R and Ctrl+Shift+P adding, Ctrl+1 and its group key, and Primary+/ opening and closing the keys overlay. Runs under a mod with `--mod-dir`. |
| `--check-mutator-button` | Checks Mutators on the skirmish setup and the campaign launch at 640x480 and 1280x720: the room's own on and off button, its size and place, "Mutators: off" and "Mutators: N on", the card the pointer shows on it, Tab, Down, a finger's reach, Y, the remembered set and the set a start hands on. |
| `--check-mutator-room` | Hosts a battle room alone and checks Mutators above Select Map, the room's own on and off button as large as it, at 640x480 and 1280x720, "Mutators: off" and "Mutators: N on" with its lamp, the remembered multiplayer set, the chat lines, and the button while the room's options are locked. |
| `--check-brain-room` | Hosts a battle room alone and checks each computer row at 640x480 and 1280x720: its Resolution cell naming the brain and its Mem cell the level over the room's greyed difficulty, the notch on its name opening the brain menu, the built-in player and its level, the Mem cell stepping the built-in player's difficulty, Use for every computer seat, the hover card, and the cell meeting no control of the room but its row's empty screen-size column. |
| `--check-mutator-picker` | Checks the mutator picker at four window sizes, including a cancelled download, a check after an install, USE THESE fetching one, a download already under way, and the runtime's own feed. |
| `--check-brain-picker` | Checks the brain picker at four window sizes: the row order, a locked brain, the level, USE THIS, every computer seat, CANCEL, the keys, the controller, and the runtime's own feed. |
| `--check-brain-seat-buttons` | Checks each computer row's brain on the skirmish setup at 640x480 and 1280x720: the row's own Player button naming it, with the notch at its right end, the label, Tab, the brain menu from the notch, Return and the right button, its levels, Options… opening the brain picker, Use for every computer seat, the hover card, the left button keeping 3.1c's own press, and the brains a start seats. |
| `--check-brain-campaign` | Checks the campaign computer player's button on NEWGAME and Any Mission at 640x480 and 1280x720: its place under Difficulty, the label, Tab, the brain menu without Use for every computer seat, the brain the first mission seats and the seat a loaded campaign save leaves. |
| `--check-mutator-prompts` | Checks the questions a stopped skirmish start, a crash and Get mutators ask: START WITHOUT IT, BACK, FORGET IT and RETRY. Then the host's "The game did not start" for a guest without its release and for one with it. |
| `--check-mod-warning` | Checks the warning for a mod whose files are missing. |
| `--check-mod-install` | Installs, rolls back and replaces made-up mod packages. |
| `--check-language-install` | Installs a language pack from a dropped file. |
| `--check-challenge` | Shows a made-up check that you are a person. |
| `--check-registries` | Adds, switches and removes a registry in Settings › Downloads. |
| `--check-library` | Opens the Library over made-up packages and carries out its actions. |
| `--check-library-install` | Installs made-up mod packages from three registries while the Library shows. |
| `--check-library-map-previews` | Checks the Library's map pictures. |
| `--check-library-content` | Turns Core Contingency off in the Library, and chooses the demo when its installer is there. |
| `--check-library-update-question` | At 640x480, confirms a rules-changing update fifty times and requires the Library to stay open with its tabs. |
| `--check-renderer-proof` | Draws a fixed terrain scene offscreen from the installed game and compares its classic projection with the experimental renderer. `--snapshot` names the output location. |
| `--check-live-renderer` | Starts a Skirmish headlessly and checks the experimental live renderer with an offscreen GPU; opens no window. |
| `--check-renderer-ladder` | Forces each renderer failure the game handles and checks it goes on. |
| `--render-fault POINT[@FRAME]` | Forces only that failure, for `--check-renderer-ladder`. |
| `--check-briefing-narration` | Checks a briefing's narration plays and stops when it should. |
| `--check-director-view` | Checks the director's view of a headless skirmish. |
| `--check-director-render` | Renders a small director script and checks the files it writes; with `--hardware-acceleration=full`, on the graphics card's Full tier. |
| `--check-interpolation` | Checks the frames drawn between ticks, and the frames at whole ticks with Smooth motion Off. |
| `--check-unit-playout` | Checks another machine's units move evenly. |
| `--reclaim-check` | Has the commander reclaim the nearest feature in a headless skirmish. |
| `--weapon-fire-check TYPE` | In a headless skirmish of `--match-ticks` ticks, places `TYPE` and checks its first weapon fires at a unit and a feature. |
| `--net-loopback-check TICKS` | Hosts and joins a match in one process over 127.0.0.1 and compares both worlds after `TICKS` ticks. |
| `--net-loopback-watcher` | The loopback check's joiner watches. |
| `--net-loopback-computer` | The loopback check's host seats a computer player. |
| `--net-loopback-shooter TYPE` | The loopback check's host makes one finished `TYPE` beside its commander, and that unit fires at the ground beside the commander's aim point. |
| `--net-loopback-each-ai` | The loopback check's host and joiner each seat a computer player. |
| `--net-loopback-fixed-starts` | The loopback check's host fixes the start positions. |
| `--check-host-not-found` | Checks a joiner whose host is never found. |
| `--check-opened-launch start\|menu FILE` | Opens `FILE` as the system opens a launch file: as the game starts, before its window opens, or once the main menu has settled. |
| `--check-recording-hook` | Checks the game declines bytes that are no recording and refuses a cut one. |

### Tools

These options set up and measure a run without a player, mostly a headless
one, for the engine's developers.

| Option | What it does |
|---|---|
| `--benchmark FRAMES` | Draws a skirmish and a campaign mission, each still and scrolling, for `FRAMES` frames each, at the zoom `--zoom` gives, and reports the zoom, the frame times and the memory in use. |
| `--match-ticks N` | With `--headless-check`, plays a skirmish, a campaign mission or a recording for `N` ticks. |
| `--campaign NAME` | The campaign of the mission a headless run plays. It needs `--mission`. |
| `--mission N` | The mission of `--campaign` a headless run plays. |
| `--past-outcome` | Plays the mission on past its victory or defeat. |
| `--restart-at TICK` | Restarts the mission at that tick and checks it starts again alike. |
| `--combat UNITS` | Gives each side of the headless skirmish an army of that many units. |
| `--busy-combat` | Adds every kind of drawing to the `--combat` armies. |
| `--stage FILE` | Places units and gives orders, as the file lists them, in the headless skirmish. |
| `--camera X,Z` | Places the camera's top left at that map pixel. |
| `--zoom FACTOR` | The battlefield's zoom in a headless run or a benchmark, held within the view's limits. |
| `--frame-rate FPS` | Draws every frame of a headless skirmish at that rate, between its ticks. |
| `--frame-log FILE` | Writes one line a frame of that run. It needs `--frame-rate`, as do the next four. |
| `--scroll-camera` | Sweeps the camera right and back over the army. |
| `--march` | Orders the army to march south. |
| `--follow` | Has the camera follow a unit. |
| `--frame-clock MS` | Starts that run's clock this many milliseconds in. |
| `--save-after TICK` | Saves the headless run once it reaches that tick. |
| `--save-file PATH.sav` | Where `--save-after` saves. |
| `--load PATH.sav` | Starts the headless run from a saved game. |
| `--give-orders` | Gives the units orders in a headless run. |
| `--seed N` | Starts the random numbers from `N`, so two runs play alike. |
| `--brain-match A,B` | Plays a two-seat headless skirmish. Each seat is an installed brain id, a brain folder, or `builtin`. It needs `--headless-check`, `--match-ticks` and `--brain-map`. [Below](#--brain-match). |
| `--brain-levels L1,L2` | The level of each `--brain-match` seat. A built-in level is `easy`, `medium` or `hard`, and `hard` when the word is left out. A brain's level is a level id from its manifest, and its first level when the word is left out. |
| `--brain-map NAME` | The map a `--brain-match` plays. |
| `--brain-shadow BRAIN` | Runs BRAIN beside the built-in player on every computer seat of a skirmish, a campaign mission or a loaded game on this machine. BRAIN is an installed brain's id, or a brain folder. The built-in player still plays the seat. [Below](#--brain-shadow). |
| `--brain-shadow-sections LIST` | Sections to compare, separated by commas: `start`, `schedule`, `sort`, `task1` to `task9`. Left out, every section is compared. It needs `--brain-shadow`. |
| `--brain-shadow-log DIR` | Writes the shadow's call logs into DIR. It needs `--brain-shadow`. |
| `--brain-trace FILE` | Writes a brain log of every computer seat's calls, orders and draws. It needs `--headless-check` and `--match-ticks`. [Below](#--brain-trace-and---brain-record). |
| `--brain-record FILE` | Writes the features and decisions a brain records, and after a brain match each seat's result. It needs `--headless-check` and `--match-ticks`. [Below](#--brain-trace-and---brain-record). |
| `--trace-digest FILE` | Writes a digest of the match at each tick. |
| `--trace-units FILE` | Adds a line for each unit to it. It needs `--trace-digest`. |
| `--trace-lookups FILE` | Writes every game file and listing the run looks up, and, for a path a mutator serves or hides, which mutator. |
| `--trace-input` | Prints each press of the pointer's buttons, and where it lands, each move of the pointer over a match, the game's window, its display and the pointer as a match begins and at each change of the window or a display, and each action event, on standard error. [Below](#--trace-input). |
| `--debug-order-lines` | Draws each construction beam as a plain line, to debug it. |
| `--draw-threads N` | The threads that draw, from 1 through 32. |

#### `--trace-input`

```sh
open-annihilation --trace-input
```

On Windows, add `--trace-input` at the end of the game shortcut's Target,
or run `open-annihilation.exe --trace-input` from a command prompt in the
game's folder. The lines go to standard error, which the game writes into
its log (or into the folder `--log-dir` names).

Each action event writes one line:

```text
input action pressed order.attack chord=A context=play
```

The word after `input action` is `pressed`, `released`, `repeated`, `double`, `held`, `active` or `inactive`. Then the action's id, the chord that matched, and the context that took the event, such as `play`.

Each move of the pointer over a match writes one line:

```text
input match pointer x=960 y=0 hud=none
```

`x` and `y` are the pointer's place on the match's screen, in its pixels
from the top-left corner, and `hud` the HUD button under it. The view
scrolls toward an edge while the pointer is on that edge's outermost column
or row: `x=0` at the left, `y=0` at the top, the screen's width or height
less one at the right or the bottom.

The game's window writes one line as a match begins and after each event
of the window or a display, once the game has answered it:

```text
input window leave window=0,0,1920x1080 pixels=1920x1080 display=1 bounds=0,0,1920x1080 usable=0,0,1920x1040 scale=1.25 edges=covers flags=full-screen,input-focus,grabbed keep=yes mouse-rect=none pointer=960,0 pointer-on=window
```

| Word | What it says |
|---|---|
| `input window` and a word | What the line is for: `match` as a match begins, or the event: `shown`, `hidden`, `moved`, `resized`, `pixel-size-changed`, `minimised`, `maximised`, `restored`, `enter` and `leave` (the pointer entering and leaving the window), `focus-gained` and `focus-lost` (the input focus), `full-screen-entered`, `full-screen-left`, `display-changed` (moved to another display), `scale-changed`, `safe-area-changed`, and a display's `display-added`, `display-removed`, `display-moved`, `display-turned`, `display-desktop-mode-changed`, `display-mode-changed`, `display-scale-changed` and `display-usable-area-changed`. |
| `window=x,y,WxH` | The window's contents: their top-left corner on the desktop and their size, in the window system's units. |
| `pixels=WxH` | The contents' size in pixels. |
| `display=N` | The display the window is on. |
| `bounds=x,y,WxH` | Where that display lies on the desktop, and its size. |
| `usable=x,y,WxH` | The part of the display without the taskbar, the menu bar or the dock. |
| `scale=S` | The display's scale: `1.25` at 125%. |
| `edges=covers`, or `edges=left:L,top:T,right:R,bottom:B` | How far each edge of the window lies inside the display. `covers` is a window lying exactly over it, as full screen does. A number below 0 lies past the display's edge, where the pointer cannot reach that edge of the window. |
| `flags=` | The window's state: `full-screen`; `exclusive`, full screen on a display mode of its own; `maximised`, `minimised`, `hidden`, `borderless`; `input-focus`, the keyboard's focus; `mouse-focus`, the pointer in the window; `grabbed`, the window holding the pointer in itself; `mouse-capture`, a button held. `none` is none of them. |
| `keep=yes` or `keep=no` | Whether full screen keeps the pointer on the window now: in full screen with the input focus. |
| `mouse-rect=x,y,WxH` or `mouse-rect=none` | The part of the window the pointer is held in; `none` is the whole window. |
| `pointer=x,y` | The pointer's place on the desktop. |
| `pointer-on=` | `window` over the window, `display` outside it on its display, `elsewhere` off that display. |

When the view stops scrolling at one edge in full screen, the window's
lines should show `edges=covers`, `full-screen`, `input-focus`,
`mouse-focus` and `grabbed` among the flags, and `keep=yes`, and the match
pointer lines should reach that edge's column or row. Then:

- A `leave` line as the pointer reaches the edge, with `pointer-on=window`
  and `edges=covers`: the pointer is still over the window's place, but
  another window lies over the game there, such as a taskbar or a tool bar
  on that edge, or an overlay. The pointer's moves go to that window, and
  the view scrolls again only once the pointer is back over the game.
- A `leave` line with `pointer-on=elsewhere`: the pointer has left the
  window's display across that edge, onto another display. Without
  `grabbed` or `keep=yes` the window was not holding the pointer.
- No `leave`, but `edges` gives that side a number below 0: the window lies
  past its display there, so the pointer cannot reach the window's
  outermost row or column on that side; with `top:-8` the match pointer
  lines stop at `y=8`. A line after `scale-changed` or
  `display-mode-changed` shows when the window moved there.

#### `--brain-match`

```sh
open-annihilation --headless-check --brain-match steady-commander,builtin --brain-levels steady,hard --brain-map "Ashap Plateau" --seed 4242 --match-ticks 1800
```

Both seats are computer players. `A` and `B` are separated by one comma and
are not trimmed. `builtin` is the built-in computer player. A seat that
holds `/` or `\`, or starts with `.`, is a brain's folder. Any other seat
is the id of a brain installed under the user folder's `Brains`.
`--brain-levels` is two levels in the same order, or left out. A brain
match needs `--headless-check`, `--match-ticks` and `--brain-map`.
`--brain-levels` and `--brain-map` need `--brain-match`. A brain match
cannot be combined with `--stage`, `--combat`, `--load`, `--save-after`,
`--campaign`, `--frame-rate` or `--benchmark`.

The match stops at the first tick where one seat has units and the other
has none (that seat wins), or where neither seat has units (a draw). When
both still have units after `--match-ticks` ticks, there is no winner.
The same command prints the same lines on every run. Each line starts
with `brain match: `, and every number is a whole decimal. A stored
amount with a fraction is cut toward zero. Seats are numbered 1 and 2.

```text
brain match: map Ashap Plateau seed 4242 ticks 1800
brain match: seat 1 steady-commander 1.2 level steady side 0
brain match: seat 2 built-in level hard side 1
brain match: end tick 1800 winner none reason time
brain match: seat 1 units 4 built 3 lost 0 kills 0 metal 512 energy 980 metal-produced 1203 energy-produced 4410 instructions 1834000 status playing
brain match: seat 2 units 9 built 8 lost 0 kills 0 metal 220 energy 1500 metal-produced 1650 energy-produced 5120 instructions - status built-in
brain match: digest 0123456789abcdef
```

| Word | What it says |
|---|---|
| `map`, `seed`, `ticks` | The map, the seed the match played, and the tick count the command asked for. |
| `seat N` and a brain | The brain's id, its version, the level id it played, and the seat's side. A folder is named by its last piece, never by the path. |
| `built-in` | This seat is the built-in computer player, at `easy`, `medium` or `hard`. |
| `end tick` | The tick the match stopped on. |
| `winner` | `seat 1`, `seat 2`, or `none`. |
| `reason` | `last-standing` when one seat still has units, `draw` when neither does, or `time` when the tick count is spent with both still in play. |
| `units` | Live units of that seat. |
| `built`, `lost`, `kills` | Units the seat has built, lost and killed. |
| `metal`, `energy` | Metal and energy in store. |
| `metal-produced`, `energy-produced` | Metal and energy the seat has produced. |
| `instructions` | Instructions the brain's calls have used, or `-` for a built-in seat. |
| `status` | `playing`, `stopped-at` and the tick the brain stopped, or `built-in`. |
| `stopped:` | The next line of a seat whose brain stopped, naming the script and the line. The match plays on. |
| `digest` | The world digest, 16 lower-case hex digits. |

A seat whose brain could not be seated is not played. The run prints
`brain match: seat 2 ridge-raider was not seated:` and the reason, and
exits with status 1. A map that cannot be played prints
`brain match: map NAME could not be played` and exits with status 1. An
option the command refuses also exits with status 1. Status 0 means the
match played to the end above, whoever won.

#### `--brain-shadow`

```sh
open-annihilation --skip-intro --mute --headless-check --seed 1234567 --match-ticks 600 --brain-shadow tests/data/brains/steady-shadow --brain-shadow-sections task9 --brain-shadow-log /tmp/shadow-logs
```

BRAIN is an installed brain's id, or a folder (a path that holds `/` or
`\`, or starts with `.`). The brain is seated beside every computer seat
this machine plays, in a skirmish, a campaign mission or a loaded game. A
seat a brain already plays keeps that brain. A network game seats no
shadow. The built-in player plays each seat as before. The brain's orders
are recorded and not applied.

At the end of the run, one line is printed for each shadowed seat:

```text
brain shadow: seat 1: start not compared, 601 ticks, no difference
brain shadow: seat 1: start same, 601 ticks, first difference at tick 29 in sort, 20 ticks differ
brain shadow: seat 1: stopped at tick 12: brain.lua:4: something failed
```

The load's player pass is compared too, once, before the ticks
`--match-ticks` runs. A run of 600 ticks compares 601, and the first squad
sort is at tick 29.

`start` is `same`, `differs` or `not compared`. The seat number is the
engine's player index. A stopped brain prints only the stopped line.

`--brain-shadow-sections` is a comma-separated list. A word that is not
`start`, `schedule`, `sort` or `task1` to `task9` is ignored. Left out,
every section is compared. `--brain-shadow-sections` and
`--brain-shadow-log` need `--brain-shadow`.

`--brain-shadow-log DIR` writes `seat-<p>-start-builtin.log`,
`seat-<p>-start-brain.log`, and, for the first differing tick and the two
before it, `seat-<p>-tick-<t>-builtin.log` and `seat-<p>-tick-<t>-brain.log`.

#### `--brain-trace` and `--brain-record`

```sh
open-annihilation --skip-intro --mute --headless-check --seed 4242 --match-ticks 1800 --brain-match tests/data/brains/steady-commander,builtin --brain-map "Ashap Plateau" --brain-trace log.txt --brain-record records.txt
```

Both need `--headless-check` and `--match-ticks`. They work in a brain
match, a shadow run, and any headless skirmish where brains are seated.
The files are created when the match starts. A file that cannot be
created stops the run with status 1. Writing either file leaves the
report, the digest and a `--trace-digest` stream unchanged, and a brain
is charged the same instructions whether or not the record file is open.

`--brain-trace FILE` writes a brain log for every computer seat this
machine plays. The first line is `brain log 1`. Each tick then follows
the call log: a brain's hook calls, orders and draws, and a built-in
seat's orders and draws. A shadow run writes both sides. In the log,
`<p>` is the engine's player index. A brain match's report numbers those
same seats 1 and 2, so report seat 1 is log seat 0.

```text
brain log 1
tick <t> seat <p> builtin|brain
call <hook> <n>…
own-draw <low> <high> <value>
own-draw float <16 hex digits>
order <name> <slot> <n>…
section <name>
draw <bound> <value>
assign <slot> <squad>
move <slot> <x> <y> <z> <queue>
patrol <slot> <x> <y> <z> <queue>
attack <slot> <target>
build <slot> <type> <x> <y> <z>
queue <slot> <type> <count>
on <slot> <0|1>
attack_point <slot> <x> <y> <z>
stance <slot> <move> <fire>
refused <name> <slot>
resync <state>
```

Numbers are decimal. Coordinates are signed 16.16 integers. `call` is
written before the hook runs. A unit is its slot, a player is its index,
and an amount is the amount the hook receives. A timer is named `timer`.
`fits` and `start` are logged under tick 0. `brain.random(n)` is
`own-draw 1 n`, and `brain.random(0)` is `own-draw 0 0` with the raw word.
`math.random(m, n)` is `own-draw m n`. `math.random()` is
`own-draw float` and 16 lower-case hex digits of the value's bits.
`brain.shared_random` is a `draw` line. An order the lines above already
name is written as that line. Any other order is `order` and its numbers
follow in the order the brain passed them.

`--brain-record FILE` writes JSON lines, UTF-8, one record a line. Keys
stay in the order shown. Numbers are whole. The same match writes the
same bytes on every machine: the file holds no time, path, user or
engine version. A record's seat number is the engine index plus one.

```text
{"kind":"header","format":1,"map":"Ashap Plateau","seed":4242,"seats":[{"seat":1,"brain":"learned-scout","version":"1.0","level":"rule"},{"seat":2,"brain":"builtin","level":"easy"}]}
{"kind":"sample","tick":300,"seat":1,"features":{"sectors":{"shape":[2,8,8],"values":[0,65536]}},"decision":17}
{"kind":"result","seat":1,"winner":false,"tick":1800,"units":4}
```

The header lists each computer seat this machine plays, in index order.
A brain's seat names its id, version and level. A built-in seat names
`builtin` and `easy`, `medium` or `hard`, and has no version. A sample is
one `brain.record` call: features in byte order of their names, each
value a tensor of counts of 65536ths (`65536` is 1), and a decision that
is either an integer or a tensor of the same shape. An integer decision
is that integer, not a count of 65536ths. After a brain match that
played to its report, one result line follows for each of the two seats.
`units` is how many units that seat still has. `winner` is true only for
the seat that won. A match that does not play writes no result lines.

The record file stops growing at 1 GiB. Its last line is
`{"kind":"full"}`, standard error says `brain record is full`, and the
match goes on.

## Ways to launch a game

A launcher, a server or a shortcut can start a network game with the map,
the rules, the players, the content and the status stream already chosen.
The battle room then opens ready, and the intro is skipped. There are five
ways to say what the game is:

| Way | For example | What it reads |
|---|---|---|
| A launch file | `open-annihilation -c host-now.oalaunch` | an `.oalaunch` file: every setting of the battle room ([Launch files](#launch-files)) |
| Options alone | `open-annihilation --host --map "Acid Foursome"` | the same settings, an option for each ([Launch settings](#launch-settings)) |
| TA Forever's settings file | `open-annihilation -c taforever.ini` | the settings 3.1c's `-c` file holds ([The `-c` file](#the--c-file)) |
| A pipe | `cat host-now.oalaunch \| open-annihilation` | a launch file, from standard input ([Standard input and pipes](#standard-input-and-pipes)) |
| Opening the file | a double-click on `host-now.oalaunch` | a launch file, as `-c FILE` reads it ([Opening a launch file](#opening-a-launch-file)) |

3.1c's `-h`, `-n` and `-b` switches work beside `-c` as they do in 3.1c
([The 3.1c command line](#the-31c-command-line)). On their own they start no
game: `-n1:192.0.2.10` alone opens the multiplayer screens over TCP/IP at
that address, as in 3.1c, and the game list shows the games there for the
player to choose.

`-c FILE` and `--launch FILE` are one option. Each names a launch file, which
is either Open Annihilation's own `.oalaunch` file or the settings file TA
Forever already writes. The game tells them apart by what the file holds,
whatever its name: a file whose first line is `oalaunch: 1` is an
`.oalaunch` file, and a file with a `[totala]` section is TA Forever's. A
file that is neither is refused. `-c -` and `--launch -` read the file from
standard input. `-c` and `--launch` cannot both name a file. `--launch`
needs one; a `-c` with no word after it starts no launch, as in 3.1c.

Each setting of a launch file has an option of its own. An option beside a
file replaces that field of the file, whichever is written first. The
options work without a file too: `--host` or `--join ADDRESS` with any of
them starts a launch of its own. Without a launch file, `--host` or
`--join`, a launch option is refused: `--metal needs --host or --join`.

A value is checked as the file's value is. A launch the game cannot use
ends the game before a window opens, with status 2 and one line that names
the file and the field (`host.oalaunch: rules.metal: 250000 is above
10000`), or the option (`--metal: 250000 is above 10000`). The password is
never part of that line and is never written to the log.

The launch stays active for the whole run, and every battle room applies it
as it opens. A player who joins takes the launch's player settings and
computer players; the host's rules are the ones that play.

### Examples

Each example names its files from the folder the game is started in. A
launch file named without a folder is looked for there first, then in the
game folder.

#### Hosting from a launch file

`host-now.oalaunch`:

```yaml
oalaunch: 1
game:
  role: host
  name: Friday game
player:
  name: Ann
  side: arm
map: Acid Foursome
rules:
  commander-death: ends
  metal: 2000
computer-players:
  - side: core
    team: 2
    difficulty: hard
```

```sh
open-annihilation -c host-now.oalaunch
```

The battle room of Friday game opens on Acid Foursome, with Ann on Arm and
a Core computer player on team 2 that plays hard, a commander's death ending
that player's game, and 2000 metal to start. `--launch host-now.oalaunch`
does the same. The other players join at this computer's address.

#### Joining from a launch file

`join-friday.oalaunch`:

```yaml
oalaunch: 1
game:
  role: join
  address: 192.0.2.10
player:
  name: Ben
  side: core
  team: 1
```

```sh
open-annihilation -c join-friday.oalaunch
```

The game joins the game at 192.0.2.10 at once, as `--join 192.0.2.10` does,
and takes its seat as Ben, on Core and team 1. A file that joins leaves
`rules` out: the host's rules play. `game.password` is the password sent,
and `game.name` chooses among several games at the address. When no game
answers there, the game list says so, and you can try again by hand.

#### TA Forever's settings file

`taforever.ini`:

```ini
[totala]
watching=2
mapmode=2
location=2
cheats=1
los=1
deathopt=2
metal=1000
energy=1000
lockOptions=0
playerlimit=4
pfs=0
session=Friday game
provider=TAForever
mission=Acid Foursome
timelimit=0
registeredonly=0
tournment=1
tournament=1
```

```sh
open-annihilation -c taforever.ini
```

The file names no `connect`, `creator` or `address`, so the game hosts over
TCP/IP at once, on Acid Foursome for four players, with watchers allowed,
a mapped start, random positions, cheats off, true line of sight, a
commander's death ending that player's game and 1000 metal and energy. The
battle room's Previous Menu reads TAForever. The log names `tournment` and
`tournament`, which 3.1c does not read. `--join 192.0.2.10` beside the file
joins that address instead.

#### A launch on one line

The launch of `host-now.oalaunch`, as options:

```sh
open-annihilation --host --game-name "Friday game" --player-name Ann --side arm --map "Acid Foursome" --commander-death ends --metal 2000 --computer-player core:2::hard
```

An option beside a file replaces that field. This hosts the file's game
with 5000 metal instead:

```sh
open-annihilation -c host-now.oalaunch --metal 5000
```

#### A launch piped in

On Linux:

```sh
cat host-now.oalaunch | ./open-annihilation
```

On macOS, to the program inside the app, or to the app through `open`:

```sh
cat host-now.oalaunch | "/Applications/Open Annihilation.app/Contents/MacOS/open-annihilation"
cat host-now.oalaunch | open -f -a "Open Annihilation"
```

On Windows, in a Command Prompt:

```bat
type host-now.oalaunch | open-annihilation.exe
```

A script can write the launch itself, with no file:

```sh
open-annihilation --launch - <<'EOF'
oalaunch: 1
game:
  role: host
  name: Quick game
map: Acid Foursome
EOF
```

TA Forever's file is read from standard input only after `-c -` or
`--launch -`:

```sh
cat taforever.ini | open-annihilation -c -
```

#### A shortcut

A Windows shortcut's **Target** starts a launch file with:

```bat
"C:\Program Files\Open Annihilation\open-annihilation.exe" -c "C:\Users\Ann\Documents\host-now.oalaunch"
```

On a Steam Deck the same goes in the game's **Launch Options**:
`-c /home/deck/host-now.oalaunch`.

#### A launch the game refuses

```sh
open-annihilation --host --metal 20000
```

prints `open-annihilation: --metal: 20000 is above 10000` and ends with
status 2, before a window opens.

## Launch files

The `.oalaunch` file is YAML. Its first line is `oalaunch: 1`; blank lines
and comments may come before it. A missing header, or another number, is
refused. Every other key is a setting's field, grouped by the field's first
word, and a key that is not one is refused with its full name
(`rules.nope`). Each key is named in full words for what it holds.

| Group | What it holds |
|---|---|
| `game` | The network game: whether this player hosts or joins, the address to join, the game's name and password, the player limit, the launcher's name on the battle room's caption, a tournament game, a game closed to more players, and the DirectPlay application GUID and port (`game.directplay`). |
| `player` | This player's seat: name, side or watching, team and colour. |
| `map` | The map the host plays. |
| `rules` | The host's rules: commander death, starting metal and energy, the unit limit, line of sight, mapping, start positions, cheats, watchers, the time limit, the lock on the options and the unit restrictions. A player who joins does not apply them: the host's rules are the ones that play. |
| `computer-players` | The computer players this machine seats, a list. |
| `content` | The base game, expansions, mod and mutators this run plays. |
| `status` | The status stream ([multiplayer/status.md](multiplayer/status.md)). |

A setting written `on` or `off` may also be written `true` or `false`. A
setting the file leaves out keeps the battle room's own choice.

`game.role` is `host` or `join`. A file that leaves it out needs `--host` or
`--join ADDRESS` on the command line. A host plays over TCP/IP. A player who
joins needs `game.address`, or `--join ADDRESS`.

Each item of `computer-players` is a group of `side` (`arm`, `core`, a
number from 0 through 31 or a name of 1 to 29 characters, matched without
regard to case; a side this game does not have leaves the room's own
choice), `team` (`1` to `5`, or `none`), `colour` (`1` to `10`), `difficulty`
(`easy`, `medium` or `hard`) and `brain`, an installed brain's id. Any but
the side may be left out, and the battle room then chooses it as it does
for a computer player a click seats: no team, the next free colour, hard,
and the room's own brain. A brain that is not installed refuses the launch.
One that does not fit the game plays the built-in player. A refusal names
an item by its place, counting from 1 (`computer-players[2].team`,
`computer-players[2].brain`). A deathmatch or a tournament game seats no
computer players, and the computer players and the player must fit
`game.player-limit`.

`rules.unit-restrictions` is a group of units and their limits, each unit by
its FBI UnitName in any case: `0` cannot be built, `1` to `100` is the most
each player may build, and `no-limit` lifts the limit, as the battle room's
Restrictions does. A unit the game's content does not have is left out.

`content` chooses what this run plays, as the Library's choice would, and
only for this run. `base-game` is the base game's id, `expansions` a list of
expansion ids, which needs a base game, and `mod` a mod's id, or the path of
a mod folder or a mod profile. If this player does not have the base game,
an expansion or the mod, the game ends before a window opens, with status
2, and the line says which (`host.oalaunch: content.mod: "ridge" is not an
installed mod`). A file that leaves `content` out plays what the Library
has chosen.
`--content`, `--base-game`, `--mod` and `--mod-dir` beside the file replace
that choice.

`content.mutators` is the mutators this run plays, in the order they apply,
top to bottom, at most 32. Each item is an installed mutator's id, or a
group of its `id`, the `version` it must be and the `options` it plays with.
An option is `on`, `off`, `true`, `false`, a number or text. A mutator that
is not installed is refused before a window opens, and the line names the
file and the item (`host.oalaunch: content.mutators[2]: "wide-sight" is not
an installed mutator`). A version the file names that is not the installed
one is refused the same way (`host.oalaunch: content.mutators[1]: "wide-sight"
1.2 is not installed; 1.3 is`), and so is an option the manifest does not
have or a value outside it. `--mutator` beside the file replaces the list.
The list applies in a `--headless-check` run too. A file without the field
plays no mutators unless `--mutator` names some.

`map` is the map's shown name, or its key (`name@package` for a pack map). A
map that is not installed is refused before a window opens
(`host.oalaunch: map: "Acid Foursum" is not an installed map`). The game
does not open on a different map.

`status` turns on the status stream. `standard-output: on` is
`--status-stdout`, `file` is `--status-file`, `quit-after-game: on` is
`--quit-after-game` and `after-score` is `--quit-after-game=after-score`.
The options beside the file replace the field they name. A launch from
standard input sets the status stream too.

### A full example

A host on Acid Foursome for four players, Core on team 1, with two computer
players and the made-up mod Ridge, which must be installed for the file to
start. The GUID in the example is Total Annihilation's own; a launcher such
as TA Forever passes the GUID it uses for Ridge.

```yaml
oalaunch: 1
game:
  role: host
  name: Example game
  password: secret
  player-limit: 4
  launcher-name: Ladder
  tournament: off
  closed: off
  directplay:
    application-guid: "{99797420-F5F5-11CF-9827-00A0241496C8}"
    port: 47624
player:
  name: Commander
  side: core
  team: 1
  colour: 2
map: Acid Foursome
rules:
  commander-death: ends
  metal: 1000
  energy: 1000
  unit-limit: 250
  line-of-sight: true
  mapping: mapped
  start-positions: random
  cheats: off
  watchers: on
  time-limit: 0
  lock-options: false
  unit-restrictions:
    CORKROG: 0
    ARMFLASH: no-limit
computer-players:
  - side: arm
    team: 2
    colour: 5
    difficulty: hard
  - side: core
    team: 1
content:
  base-game: totala
  expansions: [core-contingency]
  mod: ridge
  mutators:
    - wide-sight
    - id: fast-builders
      version: "1.0"
      options:
        factor: 2
status:
  standard-output: on
  file: status.jsonl
  quit-after-game: on
```

The battle room then uses commander ends, metal 1000, energy 1000, a unit
limit of 250, true line of sight, a mapped start, random positions, cheats
off, watchers on and a player limit of 4, with the Krogoth out and the Flash
without a limit. The host is Core on team 1 in colour 2, beside an Arm
computer player on team 2 in colour 5 and a Core one on team 1. The map is
Acid Foursome. The run plays wide-sight, then fast-builders at version 1.0
with factor 2. The same launch on the command line, the content and status
groups left out:

```sh
open-annihilation --host --game-name "Example game" --game-password secret \
    --player-limit 4 --launcher-name Ladder --player-name Commander --side core --team 1 \
    --colour 2 --map "Acid Foursome" --commander-death ends --metal 1000 --energy 1000 \
    --unit-limit 250 --line-of-sight true --mapping mapped --start-positions random \
    --cheats off --watchers on --unit-restriction CORKROG=0 \
    --unit-restriction ARMFLASH=no-limit --computer-player arm:2:5:hard \
    --computer-player core:1
```

## Launch settings

Every setting a host or a joining player can choose in the battle room is a
field of the `.oalaunch` file and an option. The fields are grouped in the
file by their first word: `game` for the network game, `player` for this
player's seat, `rules` for the host's rules, then `map`, `computer-players`,
`content` and `status`. On the command line a setting written `on|off` takes
`on` or `off`, and in the file `true` or `false` as well.
`--tournament`, `--closed` and `--lock-options` alone turn their setting on,
and `--tournament=off`, `--closed=off` and `--lock-options=off` turn it off.

What a launch does not set is what a player does in the room rather than
chooses for it: READY, an alliance with a player who has joined, the
screen size the RES column shows, which is the player's own setting, and
the buttons a mod adds, which type a command into the chat.

<!-- netgame-launch-help checks that this table is the table of launch
settings in src/app/netgame/launch_file.cpp, word for word. -->

| Field | Option | What it sets |
|---|---|---|
| `game.role` | `--host \| --join ADDRESS` | Hosts the game, or joins the game at `game.address`: `host` or `join`. A host plays over TCP/IP. |
| `game.address` | `--join ADDRESS` | The address of the game to join, at most 63 characters. A host needs none. |
| `game.name` | `--game-name NAME` | The game's name, at most 16 characters. |
| `game.password` | `--game-password PASSWORD` | The game's password, at most 10 characters. It is never written to the log. |
| `game.player-limit` | `--player-limit N` | How many players the game takes, from 2 through 10. |
| `game.launcher-name` | `--launcher-name NAME` | The launcher's name, at most 15 characters. The battle room shows it in place of Previous Menu when it has at most 11. |
| `game.tournament` | `--tournament[=on\|off]` | A tournament game: `on` or `off`. It seats no computer players, and with `rules.lock-options` the players' sides and teams are locked too. |
| `game.closed` | `--closed[=on\|off]` | Closes the game to more players once the launch's own computer players are seated: `on` or `off`. |
| `game.directplay.application-guid` | `--dplay-guid GUID` | The DirectPlay application GUID the game hosts under and looks for, with or without braces. |
| `game.directplay.port` | `--dplay-port PORT` | The DirectPlay port the game hosts on and looks for games at, from 1 through 65535. |
| `player.name` | `--player-name NAME` | The player's name, at most 16 characters. |
| `player.side` | `--side arm\|core\|watcher\|0-31\|NAME` | The player's side: `arm`, `core` or `watcher` to watch instead of play, a side's number from 0 through 31 (SIDEDATA's sections, counting from 0) or its name, 1 to 29 characters, matched without regard to case. A side this game does not have leaves the battle room's own choice. |
| `player.team` | `--team 1-5\|none` | The player's team, from 1 through 5, or `none`. |
| `player.colour` | `--colour 1-10` | The player's colour, from 1 through 10 in the order the battle room steps through them. A colour another player has gives the next free one. |
| `map` | `--map NAME` | The map the host plays: the name the list shows, or its key (`name@package` for a pack map). At most 63 characters. |
| `rules.commander-death` | `--commander-death continues\|ends\|deathmatch` | What a commander's death does: the game `continues`, the player's game `ends`, or `deathmatch`. |
| `rules.metal` | `--metal N` | Starting metal, from 0 through 10000. 0 leaves the battle room's own amount. |
| `rules.energy` | `--energy N` | Starting energy, from 0 through 10000. 0 leaves the battle room's own amount. |
| `rules.unit-limit` | `--unit-limit N` | The most units each player may have, from 20 through 6553. |
| `rules.line-of-sight` | `--line-of-sight true\|circular\|permanent` | What each player sees: `true` line of sight, `circular` or `permanent`. |
| `rules.mapping` | `--mapping mapped\|unmapped` | Whether the map starts `mapped` or `unmapped`. |
| `rules.start-positions` | `--start-positions fixed\|random` | Whether players start at `fixed` or `random` positions. |
| `rules.cheats` | `--cheats on\|off` | Whether the cheat codes work: `on` or `off`. |
| `rules.watchers` | `--watchers on\|off` | Whether players may watch instead of play: `on` or `off`. |
| `rules.time-limit` | `--time-limit N` | A whole number of 0 or more, kept and passed on with the score report. |
| `rules.lock-options` | `--lock-options[=on\|off]` | Locks the battle room's option controls for a host: `on` or `off`. |
| `rules.unit-restrictions` | `--unit-restriction UNIT=0-100\|no-limit` | Build limits, as the battle room's Restrictions sets them: each unit by its FBI UnitName (such as `CORKROG`), from 0, which cannot be built, through 100, or `no-limit`. Give the option once for each unit. A unit the game's content does not have is left out. |
| `computer-players` | `--computer-player arm\|core\|0-31\|NAME[:TEAM[:COLOUR[:DIFFICULTY[:BRAIN]]]]` | The computer players this machine seats, at most 9, each with a side (`arm`, `core`, a number from 0 through 31 or a name of 1 to 29 characters, matched without regard to case; a side this game does not have leaves the room's own choice), and a team, colour and difficulty (`easy`, `medium` or `hard`) the battle room otherwise chooses, then a brain's id. A brain that is not installed refuses the launch; one that does not fit plays the built-in player. Give the option once for each. |
| `content.base-game` | `--content GAME[:EXPANSION,...]` | The base game this run plays, by its id. |
| `content.expansions` | `--content GAME[:EXPANSION,...]` | The expansions this run plays, a list of ids. It needs a base game. |
| `content.mod` | `--mod FILE \| --mod-dir PATH` | The mod this run plays: in the file a mod's id or a path, and on the command line its profile or its folder. |
| `content.mutators` | `--mutator PATH\|ID[:OPTION=VALUE,...]` | The mutators this run plays, applied top to bottom, at most 32: each an installed mutator's id, or a group of its `id`, the `version` it must be and the `options` it plays with. Give the option once for each. |
| `status.standard-output` | `--status-stdout` | Writes the status stream on standard output: `on` or `off`. |
| `status.file` | `--status-file PATH` | Writes the status stream to this file. |
| `status.quit-after-game` | `--quit-after-game[=after-score]` | Leaves when the game ends (`on`), stays (`off`), or leaves after the score (`after-score`). |

`--computer-player` takes the computer player's side (`arm`, `core`, a
number from 0 through 31 or a name of 1 to 29 characters), then, after a colon
each, its team (`1` to `5`, or `none`), its colour (`1` to `10`), its
difficulty (`easy`, `medium` or `hard`) and a brain's id. A part left out
or left empty is chosen by the battle room: `core:2::hard` is Core on team
2, in the next free colour, playing hard, with the room's own brain.
`core:2::hard:steady-commander` seats that installed brain at the level
hard gives. A brain that is not installed refuses the launch. One that
does not fit the game plays the built-in player. The first
`--computer-player` replaces the file's list, and each adds one computer
player.

`--unit-restriction` takes a unit's FBI UnitName, an equals sign and its
limit, such as `CORKROG=0`. In the file the restrictions are a group of
`UNIT: LIMIT` lines under `rules.unit-restrictions`. The first
`--unit-restriction` replaces the file's restrictions, and a later one for
the same unit replaces the earlier.

A deathmatch or a tournament game seats no computer players, the computer
players and the player must fit the player limit, and a host who watches
needs `rules.watchers` on. The game refuses a launch that breaks one of
these, naming the field or the option.

## Standard input and pipes

When the game is given no launch file, and its standard input is a pipe or
a file rather than a terminal, it looks at the first bytes. When they are
`oalaunch:` the game reads the launch from standard input, as `--launch -`
would, so

```sh
cat host-now.oalaunch | open-annihilation
```

starts that launch. The game waits at most a quarter of a second for the
first byte. When none comes, or the bytes do not begin `oalaunch:`, it
starts as usual, so a program that starts the game with a pipe it never
writes to does not hold the start up. Once a launch has begun, the rest of
it must arrive within two seconds, and the game refuses a launch that does
not end by then. TA Forever's file is read from standard input only with
`-c -` or `--launch -`.

- **Linux:** the program reads its standard input when it is started from
  a terminal or by another program, as above.
- **Windows:** the same works from a Command Prompt:
  `type host-now.oalaunch | open-annihilation.exe`.
- **macOS:** the program inside the app reads its standard input when
  Terminal or another program starts it:

  ```sh
  cat host-now.oalaunch | "/Applications/Open Annihilation.app/Contents/MacOS/open-annihilation"
  ```

  An app started through `open`, from Finder or from the Dock has no
  standard input to read. `open -f -a` hands it the launch as a file
  instead:

  ```sh
  cat host-now.oalaunch | open -f -a "Open Annihilation"
  ```

  `open -f` writes what it is given to a temporary `.txt` file, and `-a`
  opens that file in the app, which reads it by what it holds, as
  [Opening a launch file](#opening-a-launch-file) says: at once when the
  game was not running, or from the main menu when it was.

`open "Open Annihilation"` alone cannot take a pipe: `open` does not pass
its standard input to the program it starts, and without `-a` it looks for
a file of that name. Use `open -f -a "Open Annihilation"`, or the program
inside the app.

## Opening a launch file

`.oalaunch` is one of the file types the game registers, with the game's
icon, so a launch file a lobby or a friend gives you opens with a
double-click, from Open With, or dropped on the game's window:

- **Windows:** the game registers it for you each time it starts, under
  `HKEY_CURRENT_USER\Software\Classes`, and the installer registers it for
  everyone who uses the computer. Both open it with
  `"<install folder>\open-annihilation.exe" -c "%1"`; uninstalling removes
  the installer's association.
- **Linux:** the game adds the MIME type `application/x-oalaunch` to your
  data folder, for files named `*.oalaunch` and for any text that begins
  `oalaunch:`, and a desktop entry of its own that opens it with
  `open-annihilation -c %f`.
- **macOS:** the app declares the type and is its owner, so Finder opens it
  with Open Annihilation. The app also takes a plain text file it is handed
  without being its opener, which is what `open -f -a` sends.

Settings › System › **Launch files (.oalaunch)** chooses whether this copy
opens them, On at first: Off withdraws the registration the game made for
you on Windows and Linux, and on macOS hands them to the program that opens
plain text ([Settings](settings.md#system)). On Windows, Off also hides the
installer's association for everyone from your account alone, so a copy the
installer set up no longer opens them for you either; the other accounts on
the computer keep it.

What an opened launch file does depends on when it arrives:

- **When it starts the game,** it is the start's launch, exactly as
  `-c FILE`: the intro is skipped, the launch's content chooses what the
  game plays, and the battle room opens on the file's map, rules and
  players. A file the game cannot use ends the game before a window opens,
  with status 2 and a message that names the file and the field.
- **While the game shows the main menu,** the game goes to that launch's
  battle room. From the single-player menu it goes back to the main menu
  first. A launch's mutators never keep it from opening, because they apply
  per match rather than choosing the content the game has mounted. Its list
  replaces the running game's list for its battle room's games. A mutator
  that is not installed, or whose version or options do not match, is still
  refused, and the file is not opened.
- **During a match,** while a game is being set up (a skirmish, a campaign
  mission, the multiplayer screens), or on any other screen, the game opens
  nothing and says so: in the match's message log, or with a notice. A
  launch is not opened while another launch is under way either.
- **While the game runs,** a launch whose `content` names a mod, base game
  or expansions other than those the game plays is not opened; the notice
  says to quit the game and open the file again, which then starts it with
  that content. A file the battle room cannot use is not opened, and the
  notice names the file and the field.

The `status` group of a launch file opened in the game has no effect: the
status stream is set up from the command line, before the file arrives.

On Windows and Linux, opening a launch file while the game runs starts a
second copy of the game with `-c FILE`, which starts as any second copy
does. On macOS the system hands the file to the copy that runs.

## Listing what is installed

`--list-content --json` prints the installed maps, mods and expansions as
one JSON object and exits with status 0, without opening a window. The maps
are sorted by key, and the mods and expansions by id. The object is one line
of the standard output the game was started with, whatever the log
settings. With `--log-dir`, or when the game keeps its log in its logs
folder, it is the only line there; otherwise lines of the game's log may
come before it, and the object is the line that begins `{`. A listing asks
nothing: when no game folder is known it ends with status 1 and says to
name one with `--game-dir`.

```json
{"maps":[{"shown-name":"Acid Foursome","key":"Acid Foursome","players":4,"size":"13 x 13"}],"mods":[{"id":"ridge","name":"Ridge"}],"expansions":[{"id":"core-contingency","name":"Core Contingency"}]}
```

A map's `shown-name` is the name players see, `key` the name a launch file
uses, `players` how many it is for and `size` its width and height. A mod
or expansion has its `id` and the `name` players see.

`--list-content` without `--json`, or `--json` without `--list-content`,
is refused.

## The 3.1c command line

Open Annihilation takes the command line of Total Annihilation 3.1c, so a
launcher or a shortcut written for 3.1c starts this game the same way. This
section says what each of 3.1c's switches does here, how the file `-c` names
is read, and where this game differs from 3.1c on purpose.

### How the switches are read

The words that do not begin `--` are read as 3.1c reads its command line:

- Each argument the system passes is one word, spaces and all. The shell,
  or Windows' own rules for a command line, has already split the line and
  honoured its quotes, so `-c "/Users/me/Launch Files/host.oalaunch"` names
  one file, and so does `-c"C:\Launch Files\host.ini"`. This differs from
  3.1c, which split its whole line again at spaces and tabs, quotes and
  all, so a quoted name with a space in it was two words there and no
  launcher written for 3.1c relied on it.
- A switch is `-` or `/` and its letter, in either case: `-c`, `-C` and `/c`
  are one switch.
- A switch that takes a value reads the text right after its letter, or,
  when there is none, the next word. That word is then used up: it is never
  read as a switch or a language, even when the switch cannot use it.
- The words are read from left to right, so a later switch replaces what an
  earlier one set.
- A word that does not begin with `-` or `/` names the language. The last
  one counts, and its first 63 characters are kept.
- A letter that is not a switch, a lone `-` or `/`, and a digit are ignored.
- No switch is refused, and no value of the `-c` file. The game ends with
  status 2 and a line that names the file and the key only when it cannot
  use the launch: a launch file it cannot read, or that is neither kind, a
  map that is not installed, or a base game, expansion or mod this player
  does not have. An `.oalaunch` file's values are checked as
  [Launch files](#launch-files) says. A line of game switches of 4096
  characters or more, counting one space between each word and the next,
  is refused too.

The options that begin `--` are this game's own. They are taken out first,
wherever they stand, and they win over a launch file and over the switches,
whatever their order. `-h` with nothing after it shows the help, as `--help`
does; with a word after it, it is the switch below.

A switch that does nothing here, or less than in 3.1c, is named in the log
as the game starts, one line each: `-f`, `-l`, `-w`, `-y`, the developer
switches, a `-b` that does nothing, a `-c` with no file, and `-h`, `-n` and
`-b` that a later `-c` cleared.

| Switch | What it does here |
|---|---|
| `-b WORD` | Sets one of the battle room's options for the launch: `lock` locks the options; `deathplays`, `deathends` and `deathmatch` choose what a commander's death does; `truelos`, `circlos` and `permlos` the line of sight; `mapping` starts the map mapped; `fixedloc` uses fixed start positions; `cheating` allows the cheat codes; and `watching` allows watchers. The word is in any case, attached (`-bdeathends`) or the next word. It applies to the launch `-c`, `--launch`, `--host` or `--join` starts, as the file's key would, and an option such as `--cheats` replaces it. Without a launch, or with a word that names no option, it does nothing. |
| `-c FILE` | Skips the intro and clears the launch, then reads FILE: TA Forever's settings file, read as 3.1c reads it ([below](#the--c-file)), or an `.oalaunch` file. `-c -` reads it from standard input. The next word is the file whatever it begins with. With no word after it, `-c` only skips the intro, and no launch starts. |
| `-d`, `-df` | `-d` runs the game in a window, where the intro does not play, and `-df` full screen, for this run only: neither is remembered. Another letter after `d` counts as `-d`. |
| `-e PERCENT` | Drops that percentage of this machine's outgoing network frames at random, to test a poor connection: 0 to 100. Anything else, or a value that begins with `-`, is 0. |
| `-f` | Nothing. |
| `-h NAME` | Marks this machine as the host and names the game; its first 16 characters are kept. A next word that begins with `-` is used up and sets nothing. |
| `-l` | Nothing, as in 3.1c. |
| `-n TYPE[:ADDRESS]` | Skips the intro, and the main menu goes straight to the multiplayer screens on the transport it names: `1` TCP/IP, with the address to connect to after a colon (`-n1:192.0.2.10`), whose games the game list then shows for the player to choose, or, with no address, the screens ask for one; `2` IPX, `3` a modem and `4` a serial cable, which this game does not have, so the screens open on the provider list. Another number only skips the intro. |
| `-p RATE` | The network send rate in packets per second: 1 counts as 2 and above 30 as 30, 0 is 5 a second, and below 0, or a value that begins with `-`, turns paced sending off. A launch file's `pfs` replaces it. |
| `-r TEXT` | Says, on standard output, that this game needs no DirectPlay registration, and ends with status 0 without opening a window. TEXT is the rest of the line. |
| `-s` | No sound. |
| `-t SECONDS` | The network timeout: a player silent for longer brings up the timeout window. 30 to 300; anything else is 30. |
| `-w` | Turns the sound off, as `-s` does. |
| `-y` | Skips the intro. |
| `-memfussy` and the other developer switches | Nothing. The others are `-memnofussy`, `-memfrontalign`, `-gonzo`, `-memset`, `-memnoset`, `-fpufussy`, `-fpunofussy`, `-dprinton`, `-dprintoff`, `-dprintfile`, `-memorystatus`, `-performancestatus`, `-disableimagehlp`, `-enableimagehlp`, `-disableimagehlplines`, `-enableimagehlplines`, `-debughelper`, `-debughelperold`, `-saveresources` and `-assertmethod`. A word that begins with one of them is skipped whole. |
| `LANGUAGE` | A word that is not a switch names the language, as above. |

### The `-c` file

TA Forever, and other launchers written for 3.1c, write a settings file and
pass it with `-c`: a `[TotalA]` section of `key=value` lines. This game reads
it as 3.1c reads it:

- A name with no `\`, `/` or `:` in it is looked for in the current folder,
  then in the game folder: the one `--game-dir` names, or else the one this
  game remembers from its last start. Any other name is used as written. The
  log says when the file came from the game folder.
- Only the first `[TotalA]` section is read, its name in any case. The keys
  are read in any case, the first copy of a key counts, and a key the file
  leaves out takes its default.
- Spaces around a value are dropped, and so are quotes around it. A text is
  cut to fit its field. A number is the whole number the value begins with,
  so `12abc` is 12 and `abc` is 0; an empty value is the key's default.
- No value is refused: each row of the table says what an odd one does.
  Lines that are not `key=value`, other sections, and keys 3.1c does not
  read are skipped, and the log names each key that is not read, once.

`connect`, `creator` and `address` decide how the game starts, as in 3.1c.
With TCP/IP and an address, the game is hosted at once when `creator` is not
0, and the game at the address is joined at once when it is. With TCP/IP and
no address, the multiplayer screens ask for the address; with another
transport they open on the provider list; with none, the main menu waits for
the player. When the file names none of the three, as TA Forever's does, the
game hosts over TCP/IP at once: that rule is this game's own, so that those
launchers start a game. `--join ADDRESS` beside such a file joins that
address instead.

`-c` clears the launch before it reads the file, as 3.1c does, so `-h`, `-n`
and `-b` written before `-c` are lost, and the log says so. Written after
`-c`, they replace the file's game name and host flag (`-h`), transport and
address (`-n`), and battle room options (`-b`); `connect`, `creator` and
`address` then decide with those. `pfs` replaces `-p` wherever it stands.
The options that begin `--` replace the file's values whatever their order.

The launch stays active for the whole run, and every battle room applies the
file's options as it opens.

<!-- netgame-launch-help checks that this table is the table of the keys of
the -c file in src/app/netgame/launch_file.cpp, word for word. -->

| Key | Field | What the game does with it |
|---|---|---|
| `connect` | none | The transport: `tcpip`, `internet` or `1` is TCP/IP; `ipx`, `novell` or `2`, `modem` or `3`, and `serial` or `4` name ones this game does not have, and the provider list opens. A word of three letters or more counts when it and one of these begin alike, so `tcp` is TCP/IP. `0`, the default, leaves the transport to the menus. |
| `creator` | `game.role` | Any whole number but 0 hosts, and 0 joins. With TCP/IP and an `address` the game is hosted or joined at once; otherwise the multiplayer screens ask. |
| `registeredonly` | none | Read and not used. |
| `playerlimit` | `game.player-limit` | How many players the game takes, from 2 through 10; above 10 counts as 10, and 1 or less leaves the game's own. 6 when the key is absent. |
| `timelimit` | `rules.time-limit` | Passed on with the score report; it does not end the game. Below 0 counts as 0. |
| `pfs` | none | The network send rate in packets per second, in place of `-p`: below 0 turns paced sending off, 1 counts as 2, above 30 counts as 30, and 0 leaves the rate alone. |
| `provider` | `game.launcher-name` | The launcher's name, its first 15 characters kept. The battle room shows it in place of Previous Menu when it has at most 11. |
| `session` | `game.name` | The game's name, its first 16 characters kept. |
| `mission` | `map` | The map the host plays, by the name the list shows or its key, its first 63 characters kept. A map that is not installed ends the start. |
| `password` | `game.password` | The game's password, its first 10 characters kept. |
| `player` | `player.name` | The player's name, its first 16 characters kept. Empty leaves the player's own name. |
| `address` | `game.address` | The TCP/IP address, its first 63 characters kept. With TCP/IP, a host creates its game at once and a player who joins joins the game there. |
| `lockOptions` | `rules.lock-options` | Any whole number but 0 locks the battle room's options, and 0 unlocks them. |
| `maxunits` | `rules.unit-limit` | The most units each player may have; 0 leaves the battle room's own. Below 20 counts as 20, and above 6553 as 6553. |
| `energy` | `rules.energy` | Starting energy; 0 leaves the battle room's own 1000. Above 10000 counts as 10000, and below 0 as 0. |
| `metal` | `rules.metal` | Starting metal; 0 leaves the battle room's own 1000. Above 10000 counts as 10000, and below 0 as 0. |
| `deathopt` | `rules.commander-death` | `1` the game continues, `2` the player's game ends, `3` deathmatch; any other number leaves the battle room's choice. |
| `los` | `rules.line-of-sight` | `1` true line of sight, `2` circular, `3` permanent; any other number leaves the battle room's choice. |
| `cheats` | `rules.cheats` | `2` allows the cheat codes, any other number but 0 disallows them, and 0 leaves the battle room's choice. |
| `location` | `rules.start-positions` | `1` fixed start positions, any other number but 0 random, and 0 leaves the battle room's choice. |
| `mapmode` | `rules.mapping` | `1` starts the map unmapped, any other number but 0 mapped, and 0 leaves the battle room's choice. |
| `watching` | `rules.watchers` | `2` allows watchers, any other number but 0 disallows them, and 0 leaves the battle room's choice. |
| `tournament` | none | Not read: 3.1c never reads it. The game logs it and goes on; a tournament game is `game.tournament` in an `.oalaunch` file. |
| `tournment` | none | Not read: 3.1c never reads it. The game logs it and goes on. |

### Where this game differs from 3.1c

- **The file is always read.** Not every 3.1c reads it: on some installs
  3.1c ignores the file, and `-c` only skips the intro. Launchers expect the
  file read, as the 3.1c that reads it does, so this game reads it whenever
  it is given.
- **A bare name is looked for in the current folder first.** 3.1c looks in
  the game's folder alone.
- **A file that names no connection hosts**, as above. 3.1c leaves the
  choice to its menus.
- **A player who joins joins at once**, as `--join` does. 3.1c shows the
  games at the address and waits for the player to choose one.
- **Some launches are refused.** 3.1c refuses nothing: a file it cannot read
  starts with every key at its default, and a map it does not have leaves
  the battle room on the map it had. This game will not start a game nobody
  chose, so it ends with status 2 when the file cannot be read, when the
  file is neither a settings file nor an `.oalaunch` file (it reads both,
  and cannot tell what such a file meant), when the map is not installed,
  and when the content is not.
- **Values 3.1c mishandles are kept in range.** A `maxunits` below 20 breaks
  3.1c's unit slider and is 20 here; an `energy` or `metal` below 0 or above
  32767 goes wrong in 3.1c, and is 0 or 10000 here.
- **`-b` works.** 3.1c compares the whole word, `-b` included, with its list
  of words, so no word ever matches and `-b` does nothing. This game gives
  each word the meaning its name says.
- **`-y`, `-r`, `-f` and `-w` do less.** This game has no built-in internet
  lobby for `-y` to open, needs no DirectPlay registration for `-r` to make,
  reads no disc for `-f` to find, and has no second sound player for `-w` to
  use. `-r` ends with status 0, where 3.1c ends with 1.
- **The launcher's name** (`provider`) replaces Previous Menu for every
  launch. 3.1c shows it only when a lobby program started the game.
- **The window.** Without `-d` or `-df`, a player's run opens full screen on
  Windows and in a window on macOS and Linux. 3.1c always starts full screen.
- **A name with a space in it is one word.** Each argument is one word
  here, so a launch file in a folder whose name has a space works with
  `-c`; 3.1c splits its line at every space, quotes and all.
- **Long lines.** A line of game switches of 4096 characters or more is
  refused; 3.1c reads any length.

## Exit statuses and refusals

| Status | When |
|---|---|
| 0 | The player left the game. `--version`, `--help`, `-r`, `--list-content --json` and a `--print-profile` of a usable profile end with 0 too, and so does a check that passed. With `--quit-after-game`, the game ended. |
| 1 | An option or a value was refused, an error stopped the game, such as game data it cannot read, or a check failed. A `--print-profile` of a profile with errors ends with 1. |
| 2 | A launch the game cannot use: a launch file it cannot read, or that is neither kind; a field or an option out of range, or one that needs another; a map, a base game, an expansion, a mod or a mutator that is not installed, or a mutator option it lacks or a value outside that option's choices, or a mutator list the manifests refuse. |
| 3 | The game ran out of memory, and said so in a box and in `ErrorLog.txt` beside the program. With `--quit-after-game`, the player left the game or lost the connection. |
| 77 | A check that cannot run on this machine. |

A refusal is one line that begins `open-annihilation: `, on standard error,
and in the log when there is one. A game started with no terminal, from
the desktop, shows it in a box instead. A launch's refusal names where the
value came from, the file and its field or key, or the option, then why:

```text
open-annihilation: unknown option: --nope
open-annihilation: --metal: 20000 is above 10000
open-annihilation: --metal needs --host or --join
open-annihilation: -c and --launch cannot both be given
open-annihilation: host.oalaunch: rules.metal: 250000 is above 10000
open-annihilation: host.oalaunch: map: "Acid Foursum" is not an installed map
open-annihilation: taforever.ini: mission: "Acid Foursum" is not an installed map
open-annihilation: nothere.oalaunch: the file: cannot be read from the current folder or the game folder
```

A launch the game cannot use ends it before a window opens: a value
refused on the command line or in the launch file, and a map, base game,
expansion or mod that is not installed. A launch file opened with the game
is checked the same way. Only where the game must first ask where Total
Annihilation is, or show its first setup, does that window come before
the check. The password is never part of a refusal, and never written to
the log.

With `--status-stdout` the status lines own standard output; the refusal
goes to the log, and `--log-dir` names where that is. A launch refused
once the status lines have begun ends them with `exit`, its reason
`refused` and its code 2 ([what a game reports](multiplayer/status.md)).
