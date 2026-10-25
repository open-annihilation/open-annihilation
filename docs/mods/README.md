# Mod support

Open Annihilation plays mods of Total Annihilation as well as 3.1c itself.
A mod describes how it differs from 3.1c in one file, `oamod.yaml`: its
archives and directories, its engine limits, the extra values its unit
scripts read, the extra keys in its data files, and the **standard hacks**
it turns on. The engine reads that profile and plays the mod by those
rules. It holds no code for any particular mod, and it never guesses which
mod is installed: a folder without a profile plays exactly as 3.1c.

- [The OAMOD standard](oamod-standard.md) is the full description of the
  profile format and its rules.
- [The standard hacks](#the-standard-hacks) below explains what a hack is,
  and [every standard hack](#every-standard-hack) lists each behaviour a
  profile can turn on, with a page of its own.
- [Every script extension](#every-script-extension) lists the values a
  profile can give a mod's unit scripts, each with a page of its own that
  shows how a script reads it.
- [Developer Mode](#developer-mode) changes a profile's hacks from the
  settings' Developer section, without editing the profile.

## Installing a mod

A mod published to a registry, Core Prime's or one you added, installs from
the Library: the **OA** button on the main menu opens it, and **GET** on a
mod installs it ([from the Library](#from-the-library)). A mod you have as
a folder or a file installs as follows:

1. Put the mod's folder in the **Mods** folder of your own Open Annihilation
   folder, `Documents/Open Annihilation/Mods` (where that is on each system:
   "Where it keeps its files" in the
   [installation guides](../installation/macos.md#where-it-keeps-its-files)),
   or in the game folder's `mods` folder. A mod that comes as a `.oamod` file
   installs itself there when you open the file
   ([Installing a .oamod file](#installing-a-oamod-file)).
2. Open the Library (the **OA** button on the main menu), choose the mod
   on its **Mods** tab and press **PLAY NOW**
   ([choosing a mod](#choosing-a-mod)). The game reloads its data for the
   mod and returns to the main menu playing it.

**MODS** in the **Your files** row under **System** in the settings
opens the Mods folder in the file manager, making it the first time, and a
mod's **OPEN FOLDER** in the Library opens the mod's own folder.

**Mod folder.** A mod folder holds the mod's files, `oamod.yaml` among them:

```text
Documents/Open Annihilation/
  Mods/
    <mod name>/
      oamod.yaml
      oamod.png             the mod's badge, if it has one
      ...                   the mod's own archives and files

<game folder>/
  totala1.hpi  ...          the plain 3.1c installation
  mods/
    <mod name>/             a mod folder can sit here as well
```

The mod folder is layered over the game folder: a file in the mod folder
takes the place of the game folder's file of the same path, as copying it
over would. One plain 3.1c installation then serves every mod, and the game
folder is left as it is. A game folder that holds its own `oamod.yaml`
cannot carry a mod folder.

**Copied install.** Copy the mod's files into a copy of the game folder, so
that `oamod.yaml` sits in that folder's root. The game folder then plays
that mod and nothing else.

A mod folder does not need an `oamod.yaml`. One without it is layered over
the game folder all the same, its archives and files over the game's, but
with no profile the game plays by 3.1c's own rules: no standard hack, no
renamed directories, 3.1c's limits. Each start that plays such a folder
says so on standard output. This suits a folder of extra units or maps
made for 3.1c itself.

A profile can give a one-line `description`, and a mod folder can hold an
`oamod.png` badge beside its `oamod.yaml`; the Library shows the badge, and
the description in the mod's details when no registry lists it.
[The OAMOD standard](oamod-standard.md) describes them.

### From the Library

The Library lists every mod of the registries that are on, with what each
does to the game: how many standard hacks change the game and how many
only the view, the base game and the Open Annihilation it needs, and its
rules hash. A mod that needs hacks this build does not have is listed, and
says which, but cannot be got. **GET** downloads the mod, checks it as a
`.oamod` file is checked, and installs it in your Mods folder as
`Mods/<id>`; **PLAY NOW** then switches to it
([the Library](../library.md)). That install shows no notice: the Library's
row says it is in place. A file you open still does.

**Where it came from.** The installer writes `.oa-origin.yaml` in the
mod's folder: the registry, the mod's key there, its release and the
package's SHA-256. That record is how OA knows a mod's updates: an update
is the same mod at a higher release in the same registry. A mod you
installed from a `.oamod` file is matched to a registry's release by its
SHA-256, and offered that registry's updates from then on; one no registry
lists, and a folder installed by an earlier version or made by hand, is
your own and is never offered an update.

**Updates.** When the registry lists a newer release, the Library says so,
on the mod's row, in its details ("Revision 4 available · 3.2 MB to
download", and its Rules fact when the update changes the rules) with an
**UPDATE** button, and on its Updates tab; the main menu's **OA** button
wears a dot. Nothing updates until you press UPDATE or UPDATE ALL. An update from
Core Prime's registry installs with no question; a mod from a registry you
added asks before an update that changes the rules, or before it replaces
a copy you did not get from that registry
([which downloads ask first](../library.md#which-downloads-ask-first)). An
update to the mod being played waits until you switch mod, switch it off or
quit ("Finishes when you leave …").

**Rolling back an update.** The update keeps the version it replaced, as a
file's replace does ([one version back](#installing-a-oamod-file)), and
**ROLL BACK** brings it back, with its record of where it came from: the
update is then offered again, and installs only when you press UPDATE.

### Installing a .oamod file

A `.oamod` file is a mod packed into one file: a zip archive of the mod's
folder, the one that holds its `oamod.yaml`. Open it in any of these ways
and Open Annihilation installs it into your Mods folder, in a folder named
by the mod's id (`Mods/<id>`), whatever the file is called:

- double-click it, or choose Open With and Open Annihilation;
- drag it onto the game's window;
- on iOS, tap it in the Files app, or share it to Open Annihilation;
- start the game with `--install-mod FILE.oamod`, or with the file's path
  alone.

A file opened during a game waits until you are back at the main menu, and
several files are installed one at a time. The game reads the whole file
and checks it before it writes anything; then, depending on what your Mods
folder holds already:

| Your Mods folder holds | What happens |
|---|---|
| no folder of the mod's id | it installs, and says where (**MOD INSTALLED**, with **OPEN FOLDER** and **OK**); **PLAY NOW** plays it |
| the same mod and version, another revision | **UPDATE MOD**: **REPLACE** or **CANCEL**; it says when the file is the older revision |
| the same mod, version and revision | **ALREADY INSTALLED**: **REINSTALL** puts its files back as they came, or **CANCEL** |
| the same mod at another version | **ANOTHER VERSION**: **REPLACE**, **INSTALL ALONGSIDE** in a folder of its own (`Mods/<id>-<version>`), or **CANCEL** |
| a folder of that name that holds something else | **FOLDER IN USE**: **INSTALL ALONGSIDE**, or **CANCEL**; that folder is never changed |

A version installed alongside is updated in its own folder the next time a
file of that version and another revision is opened.

**One version back.** Replacing a mod keeps the version it replaced in the
mod's folder, in `.backup`, and drops the one kept before: after revisions
1, 2 and 3, the folder holds 3 and keeps 2. **ROLL BACK** in the mod's
details in the Library swaps the two, so a second **ROLL BACK** undoes the
first; it is offered while the kept version is one the game can still play.
A reinstall keeps the version kept as it is. The game never reads
`.backup`: the kept version is not layered over the game folder and the
Library does not list it. Replacing or rolling back the mod the game plays reloads the
game's data, back on the main menu, once the files are swapped.

What a replace or a reinstall does not carry over: anything you changed or
added inside the mod's folder, such as its INI file. After a replace, the
`.backup` folder keeps it until the next replace. Versions installed
alongside each other share `Saves/<id>`, `Screenshots/<id>`, `Films/<id>`,
`Recordings/<id>` and the settings kept under the mod's registry root,
since they share an id; a saved game loads only under the same sim hash.

**Making a .oamod file.** Zip the mod's folder, the one that holds
`oamod.yaml`, or the files in it, and rename the `.zip` file to `.oamod`:

- on macOS, choose Compress in the Finder;
- on Windows, choose Send to › Compressed (zipped) folder;
- on Linux, `zip -r example-mod-1.0.oamod example-mod`.

Raise `packaging.revision` in `oamod.yaml` for each new package of the same
version, so that the game offers it as an update.

A package may also name a homepage, a few tags and the oldest Open
Annihilation it runs on. Quote the engine requirement, because a value that
starts with `>` is refused unless it is quoted:

```yaml
homepage: "https://example.org/example-mod"
tags: [balance, ai]
requires:
  base: ta-3.1c
  catalogue: 1
  engine: ">= 0.8.0"
```

The Library links to the homepage and files the mod under its tags. A
player whose Open Annihilation does not meet the requirement is told the
version the mod needs.

**What is refused.** A file that is not a zip archive or is damaged; one
without `oamod.yaml` at its top or in the one folder at its top; a profile
that the game would refuse to play, with its first errors shown and all of
them in the log; a name that cannot be unpacked on every system the game
runs on (`..` and absolute paths, drive letters, characters or names
Windows refuses, names that differ only in case); links; encrypted entries,
and entries packed otherwise than stored or deflated; more than 4 GiB
unpacked or more than 16,384 folders, or far more than the file's own size;
and too little free space. Nothing is changed when a file is refused or an
install fails. A file sync or a virus scanner holding the new files can
delay an install by a few seconds.

### Choosing a mod

You choose the mod in the Library, on its **Mods** tab
([choosing a mod](../library.md#choosing-a-mod)). It lists the mods you
installed and every other folder the game can play as a mod: each folder in
the game folder's `mods` folder and in your own Mods folder, and the folder
an earlier version's Pick Folder... chose, while it is still your chosen
mod and still exists. Once you switch to another mod or to none, the game
forgets that folder. Each shows its badge (its `oamod.png`, or letters of
its name) and, from its `oamod.yaml`, its title and version, and its
details its description. A folder without an `oamod.yaml` shows the
folder's name and "N/A", and its details say "No oamod.yaml present"; it
plays with the game's own rules. The mod being played is marked PLAYING.

To play a mod folder kept elsewhere, put it in the Mods folder. **MODS**
under Your files in Settings › System opens your Mods folder in the
system's file manager.

- **PLAY NOW**, on a mod you are not playing, remembers the new mod and
  reloads the game for it in place, without closing the window or
  restarting the game. The main menu then shows, playing the new mod. The
  folder chosen is played as `--mod-dir` would play it, and so are later
  starts.
- **SWITCH OFF**, on the mod being played, opens **Switch Mod**: "Switch to
  No Mod now? The game reloads its data for the new mod and returns to the
  main menu. Your other settings are kept." **Cancel** (Escape) leaves
  everything as it was; **Switch** (Y) plays 3.1c as it is, from then on.

A mod folder that holds the mod's `oamod.yaml` but not all of its game files
can still be switched to, and its menus play. When the units its profile
reads are missing, or a side's commander is not among them, the main menu
warns once from each start: **Mod Files Missing** names the mod and what is
missing, for example "The ARM commander (ARMCOM) isn't in this mod's
units", and says that its games can't start until the mod's files are
added to the folder it shows. **Open Mod Folder** opens that folder, and
**OK** (Enter or Escape) closes the warning. Starting a skirmish, a
multiplayer game, a campaign mission or a saved game with that mod shows
the same warning and stays on the screen it was started from.

When a file a side's SIDEDATA section names, its interface art (`intgaf`)
or its font, is missing from a mod folder, with an `oamod.yaml` or
without, the same warning shows once from each start and names each file,
for example "The CORE side's anims/CORINT.GAF isn't in this mod's files;
games show without it", and says that its games still start. They start
and play: a side without its interface art has no panels, as a side that
names none, and a side without its font draws its resource numbers and
unit panel as a side that names none. The game played without a mod still
ends as it starts when such a file is missing, naming the first.

A folder whose `oamod.yaml` cannot be played over the game folder is
refused, a note over the Library saying why, and the mod stays as it was;
so is any folder
over a game folder that is a mod's copied install, and a folder without an
`oamod.yaml` whose SIDEDATA the game's own rules cannot start with, such as
one that lacks a section a side needs; the log says why. A remembered mod folder that has gone
is dropped with a notice, and No Mod is played. While `--mod-dir` or
`--base-game` decides the run's mod, the Library's PLAY NOW and SWITCH OFF
are greyed, its details saying "The command line chose this run's mod.",
and the stored choice is kept, which a start without them plays. A game
keeps the mod it started with: the Library shows only on the main menu.

The command line chooses a mod for one run:

| Option | What it does |
| --- | --- |
| `--mod-dir PATH` | Plays the mod folder at `PATH`, layered over the game folder. |
| `--base-game` | Plays 3.1c without the remembered mod folder. It cannot be combined with `--mod-dir`. |
| `--mod FILE` | Reads the profile from `FILE` instead of the folder's own. This is for developing a profile, or for trying one with a mod that does not ship it. |
| `--print-profile` | Prints the resolved profile of `--mod FILE`, or of the `--mod-dir` or `--game-dir` folder, with its two hashes, and exits. A profile with errors prints each error and exits with status 1. |
| `--accept-unimplemented-hacks` | Accepts, with a warning each, the hacks this build does not implement yet. For development only. |

The profile comes from `--mod`'s file, else the mod folder's `oamod.yaml`,
else the game folder's own. A profile that fails validation stops the game
from starting and names every error; the game never falls back to 3.1c
with a broken profile. Each run that plays a profile names it and its sim
hash. [Section 4](oamod-standard.md#4-where-the-file-lives) of the
standard has the details.

## What a profile holds

A profile is a strict subset of YAML, written for people to read and edit.
Its blocks are:

- `oamod`, `id`, `name`, `version` and the optional `description`: the
  format version and the mod's id, name, version and one line about it;
- `author` and `packaging`: who made the mod (`unknown` when nobody is
  known, with an optional e-mail address), and who packaged it, on which
  day and in which revision; both are required;
- `identity`: the display version shown on the main menu in place of
  "Open Annihilation" and this build's version, the network version its
  games announce, its settings file, its registry root and its side names;
- `layout`: the archives the engine mounts, and the directories and file
  names it reads;
- `limits`: engine limits such as the number of units per player, and how
  many weapon ids and sides the mod numbers;
- `script-extensions`: extra values the mod's unit scripts read and set,
  each described on its own page under
  [script-extensions/](script-extensions/) (see
  [every script extension](#every-script-extension));
- `data-keys`: extra keys in the mod's unit and weapon files;
- `hacks`: the standard hacks, each with its parameters;
- `settings`: the parameters a player adjusts through the mod's INI file
  or registry settings, and the registry values a first run starts with;
- `strings` and `media`: texts and movies the interface uses.

Anything a profile leaves out plays as 3.1c. The
[OAMOD standard](oamod-standard.md) describes each block, with a minimal
profile and a fuller example.

## The standard hacks

A standard hack is one behaviour the engine implements once, in the module
that owns it, and any mod can turn on. Its id is `area.name`, such as
`repair.rate`, and names the behaviour, never a mod. A profile turns a hack
on under `hacks`, by writing `true` for its defaults, a bare value for its
shorthand parameter, or a mapping of an optional preset and parameters. A hack that is
absent, or written `false`, plays as 3.1c.

Each hack has a **scope**:

- **sim** hacks change the game itself. Every machine in a network game
  must play the same sim hacks with the same values, and they are part of
  the profile's sim hash.
- **view** hacks change only what one player sees, hears or types. Each
  player may set them differently, and they stay out of the sim hash.

Each hack also runs in a set place, which its page names under "Runs on":
on the machine that owns the unit or computer player, on every machine
alike, on the host only, or for this machine's own view. A few hacks have
parts in more than one place.

Every hack page has the same five parts:

1. **Facts:** the hack's area, scope, where it runs, its number of
   parameters and whether this engine implements it.
2. **Description:** what the hack does for a player, and what 3.1c does
   without it. A hack that changes what the player sees shows it in
   screenshots, often with the hack on and off, kept in
   [standard-hacks/images](standard-hacks/images/).
3. **Configuration example:** one or more `hacks` entries to copy.
4. **Details:** the exact behaviour, the edge cases it keeps, how it
   affects network play and saves, the tests that check it, and any known
   limit of this engine.
5. **Full configuration schema:** every way to write the hack, each
   parameter with its type, bounds, 3.1c baseline, default, who may adjust
   it and its scope, the presets, the constraints, and a YAML block with
   every parameter at its default.

The facts and the schema are generated from the engine's registry, so they
always match what the engine accepts. A hack whose status is "not
implemented" is refused by this build unless `--accept-unimplemented-hacks`
is given; its page says what works so far.

## Validation and hashing

The engine checks a profile completely before it plays: every key, hack,
parameter, preset, script extension and data key must be known, and every
value must have the right type and lie within its bounds. Each refusal names
the file, the line and column, the offending path and what is wrong.
A setting a player has saved that does not read as its type is ignored with
a warning, and one out of bounds is clamped with a warning. A valid profile
resolves to an **effective profile** with every value filled in, and two
hashes of its canonical form: the **full hash** of everything, and the
**sim hash** of only the parts that decide the game. Two profiles with the
same sim hash play the same game, whatever their names, comments or display
choices. `--print-profile` prints the effective profile and both hashes.
[Section 11](oamod-standard.md#11-the-resolved-profile-and-its-hashes) of
the standard defines them.

## Network play

Network play reads its rules from the profile. Every machine in a game must
play the same sim hash, the same game data and the same unit set; view
hacks may differ from player to player. A profile's network version is the
version a machine announces, and with any version other than 3.1 a machine
joins only a game of the same major version.

The `network.*` and `recorder.*` hacks select the wire behaviour a mod's
games use: the private chat channel, the integrity check, votes to drop a
player, the lag guard, the commander start sync, recording and replays.
They make the engine speak a mod's protocol, so that its games follow the
same exchanges as the mod's own game client. Where this engine's behaviour
still differs from such a client, the hack's page says so as a known limit;
the integrity check, for one, compares digests of the engine's own rules,
so a machine running another client reports a mismatch
([`network.vercheck`](standard-hacks/network.vercheck.md)).
[Section 12](oamod-standard.md#12-network-compatibility) of the standard
covers network compatibility.

## Saves and settings

Game folders are never written to. With a profile, saved games go to
`Saves/<id>` in the player's own Open Annihilation folder, screenshots and
films to `Screenshots/<id>` and `Films/<id>`, and the recordings of network
games to `Recordings/<id>`, so each mod keeps its own list of saved games;
without one, as for a mod folder without an `oamod.yaml`, they go to the
`default` folders, and a mod whose id is `default` uses `default (mod)`.
Versions before 0.7 kept saved games in `mods/<id>/SAVEGAME` beside the
preferences file, and earlier versions kept recordings in
`mods/<id>/demos` there; the first start of a later version moves them
there once, with those of 3.1c. A save
made under a profile records the profile's id, version and hashes, and
loads only under a profile with the same sim hash. A save without a profile
record loads under any profile.

A mod's INI file is read, never written. Settings the mod keeps in the
registry are kept in the engine's own preferences, under the profile's
registry root, and a mod's first run fills in the values its profile seeds.

## Developer Mode

Developer Mode, in the **Developer** section at the foot of Open
Annihilation's settings, changes the standard hacks of
the profile the game plays without editing its `oamod.yaml`, which is never
written. It is for trying a hack, or a hack's values, before writing them
into a profile. With no mod it changes the plain 3.1c baseline, so it can
turn hacks on over 3.1c itself.

**Enable Developer Mode** heads the Developer section, Off by default, over
the section's **Show performance statistics**, which Developer Mode leaves
alone. Under them the section lists every standard hack, grouped by area,
in a list that scrolls on its own, in two levels that open and close, every
one closed at first: an area with how many of its hacks are on, and under
it each hack with its title and an Off/On switch, areas and hacks
alphabetically by title. Opened, a hack shows its id, its summary, "Applies
at next match" for a sim hack, and a control for each parameter that fits
its type, bounds and unit: a switch, a slider of numbers, of values or of a
list's items, or a switch for each value of a set. **Show Active Only
(X/Y)**, under the list, shows only the hacks that are on: X is how many
are on as the section shows them, Y every standard hack. **Restore profile
values** clears every change.

- **Off**, the list only shows the profile's own values, and the game
  plays the profile as it ships. Changes made while it was on are kept
  for when it is turned on again.
- **On**, the list's controls take changes, and the changes apply: a view
  hack's at once, even in a running match; a sim hack's from the start of
  the next match, so that a running game and its saves keep the rules they
  started with. They apply alike in a game played alone and in a network
  game.

The changes are kept with the other Open Annihilation settings, in the
player's preferences, under the id of the profile the game plays (with no
mod, under `ta-3.1c`; for a mod folder without a profile, under `folder:`
and the folder's path), so each mod keeps its own. Each is checked as the
same value written in a profile would be; one that no longer fits, such as
one kept from an earlier version, is left out with a warning on standard
error. They become part of the effective profile, so a sim hack changed
in Developer Mode changes the profile's sim hash, as writing the same value
into `oamod.yaml` would: a save made with such a change loads only under
the same effective profile, and every machine of a network game must play
the same one. With no mod, while only view hacks are changed, the game
still plays 3.1c's rules and its saves load anywhere.

In a network game's battle room the engine says, as the player's own chat
line, which engine the machine runs: `[Engine: OpenAnnihilation v<version>]`,
or `[Engine: OpenAnnihilation v<version> DEV MODE]` while Developer Mode is
on. It is said once as the battle room is entered, and again whenever
Developer Mode is turned on or off there, so every player sees whether
Open Annihilation and its Developer Mode are in use. 3.1c says nothing
there; the engine differs on purpose.

Developer Mode is the baseline under mutators. A mutator's value for the
same parameter plays in the games that mutator runs, and the picker flags
the overlap.

## For contributors

The registry,
[src/data/mod-profile/registry/hack-registry.yaml](../../src/data/mod-profile/registry/hack-registry.yaml),
is the single source of truth for every limit, script extension, data key
and hack a profile can name, with each parameter's type, bounds, 3.1c
baseline, default, adjustment, scope and presets. The engine implements
each hack in the module that owns its behaviour; the
[mod-profile module](../../src/data/mod-profile/README.md) describes the
registry and the resolver.

- [tools/gen_mod_registry.py](../../tools/gen_mod_registry.py) checks the
  registry and writes the tables and rule records the engine compiles from
  it. The `mod-registry-sync` test fails while a generated file differs
  from what the registry gives.
- [tools/gen_mod_docs.py](../../tools/gen_mod_docs.py) writes the tables
  below and the standard's table of script extensions, the heading (the
  hack's title) and the facts and schema blocks of every hack page, and the
  heading and the facts and related blocks of every script extension page.
  It creates a skeleton page for a new hack or extension. Besides the
  heading, it writes only between a block's `BEGIN GENERATED` and
  `END GENERATED` comment lines; the prose outside them is kept. The
  `mod-docs-sync` test fails while a heading or a block differs from what
  the registry gives, or while a page names no hack or extension.

To add or change a hack, change the registry, run both tools, then write or
update the page's Description, Configuration example and Details.
A screenshot goes in `standard-hacks/images/`, named after the hack, as a
small PNG cropped to the feature, with alt text and a one-line caption on
the page.

To add or change a script extension, change the registry and the
extension's entry in the tool's `SCRIPT_EXTENSIONS` (its title, whether it
takes a unit id, what it returns, how fidelity changes it and whether every
machine reads the same answer), run both tools, then write or update the
page's Description, Syntax, Usage, Configuration example and Details.
Every BOS example on a page must do what its page says, and warn where a
use would break network play.

## Every standard hack

The table is generated from the registry. Each hack is named by its
title, as Developer Mode lists it, beside the id a profile writes; areas
and the hacks within each follow their titles alphabetically. Scope is
`sim` or `view`, as [above](#the-standard-hacks).

<!-- BEGIN GENERATED: hacks table -->
### AI

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Attack Wave Size](standard-hacks/ai.attack-wave-size.md) | `ai.attack-wave-size` | Sets how many units the AI's land and naval attack groups gather before they attack. | sim | implemented |
| [Commander Builder Limit](standard-hacks/ai.builder-withhold-threshold.md) | `ai.builder-withhold-threshold` | An AI commander stops taking builder jobs once the computer player owns a set number of builders. | sim | implemented |
| [Commander Keeps Orders When Hit](standard-hacks/ai.commander-keeps-orders-when-damaged.md) | `ai.commander-keeps-orders-when-damaged` | A damaged AI commander keeps its orders instead of clearing its queue. | sim | implemented |
| [Commander Runs Factories](standard-hacks/ai.squad5-factory-tick.md) | `ai.squad5-factory-tick` | The AI commander's third task runs the factory and economy tick, so computer players build and fire stockpile weapons. | sim | implemented |
| [Difficulty Names](standard-hacks/ai.difficulty-names.md) | `ai.difficulty-names` | Reorders the difficulty names, so each difficulty selects a different AI profile. | sim | implemented |
| [Factory Tick Filter](standard-hacks/ai.factory-tick-filter.md) | `ai.factory-tick-filter` | The AI factory tick skips buildings with queued background orders and switches power on any building that uses enough energy. | sim | implemented |
| [Ignore Submerged Targets](standard-hacks/ai.nearest-enemy-filter.md) | `ai.nearest-enemy-filter` | AI land attack groups ignore fully submerged enemies when they choose the nearest target. | sim | implemented |
| [Income Multipliers](standard-hacks/ai.income-multipliers.md) | `ai.income-multipliers` | Sets the computer player's production and reclaim income multipliers for each difficulty. | sim | implemented |
| [Patrol Edge Fallback](standard-hacks/ai.patrol-null-enemy-skip.md) | `ai.patrol-null-enemy-skip` | When the AI's forces have no position, its patrol goes to a random map edge instead of attacking a missing enemy. | sim | implemented |
| [Patrol Group Size](standard-hacks/ai.patrol-group-size.md) | `ai.patrol-group-size` | Sets how many units the AI's patrol group gathers before it scouts a map edge. | sim | implemented |
| [Squad Assignment](standard-hacks/ai.squad-assignment.md) | `ai.squad-assignment` | Idle AI units are grouped into squads and given standing orders by role. | sim | implemented |

### Aircraft

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Guard Respects Hold Position](standard-hacks/air.guard-respects-hold-position.md) | `air.guard-respects-hold-position` | Guarding aircraft engage an attacker only when their move order is not Hold Position. | sim | implemented |
| [Gunships Hover to Strafe](standard-hacks/air.gunships-hover-to-strafe.md) | `air.gunships-hover-to-strafe` | Gunships hover in range and strafe a point on the ground, and guarding gunships attack enemies in reach. | sim | implemented |
| [No Repair Retreat](standard-hacks/air.no-repair-retreat-flag.md) | `air.no-repair-retreat-flag` | Aircraft with a chosen unit flag never break off to find a repair pad when damaged. | sim | implemented |

### Console

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [AI and Control Cheats](standard-hacks/console.ai-control-cheat-group.md) | `console.ai-control-cheat-group` | The +AI and +Control commands join the cheat commands, so they work in multiplayer games with cheats on. | sim | implemented |
| [ATM Amount](standard-hacks/console.atm-amount.md) | `console.atm-amount` | Sets how much metal and energy +ATM adds. | sim | implemented |
| [Game Speed Range](standard-hacks/console.game-speed-range.md) | `console.game-speed-range` | Keeps the game speed within a set range, which recorder commands can lock for everyone. | sim | implemented |
| [Key Remaps](standard-hacks/console.key-remaps.md) | `console.key-remaps` | Moves the repeat-last-command key and adds a second key for the watch toggle. | sim | implemented |
| [LOS Type Cheat](standard-hacks/console.lostype-cheat-group.md) | `console.lostype-cheat-group` | The +LOSType command needs cheats on. | sim | implemented |

### Economy

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Deterministic Wind](standard-hacks/economy.deterministic-wind.md) | `economy.deterministic-wind` | Wind changes come from one generator seeded the same on every machine. | sim | implemented |
| [Statistics Exclude Shared Income](standard-hacks/economy.stats-exclude-shared-income.md) | `economy.stats-exclude-shared-income` | Resources received from allies no longer count as produced in the statistics. | sim | implemented |

### Game Setup

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [AI Player Names](standard-hacks/setup.ai-player-name-format.md) | `setup.ai-player-name-format` | Computer player names include the hosting player's name and slot. | sim | implemented |
| [Commander Warp](standard-hacks/setup.commander-warp.md) | `setup.commander-warp` | Adds an opt-in start in which the game waits while each player places their commander. | sim | implemented |
| [Map Scripted Units](standard-hacks/setup.map-scripted-units.md) | `setup.map-scripted-units` | Maps may place units for each player at the start and at set times, with a neutral computer player for unowned ones. | sim | implemented |
| [Prebuilt Base](standard-hacks/setup.recorder-prebuilt-base.md) | `setup.recorder-prebuilt-base` | Adds opt-in chat commands that build a prebuilt base for each player at the start. | sim | implemented |
| [Several Local AI Players](standard-hacks/setup.multiple-local-ai.md) | `setup.multiple-local-ai` | One machine may add several computer players to a multiplayer game. | sim | implemented |
| [Start with One Human](standard-hacks/setup.allow-start-with-ai.md) | `setup.allow-start-with-ai` | A multiplayer game may start with one human and computer players. | sim | implemented |
| [Team Start Positions](standard-hacks/setup.team-start-positions.md) | `setup.team-start-positions` | Start positions are given out by team, with team-mates next to each other. | sim | implemented |

### Interface

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Allied Unit Display](standard-hacks/ui.allied-unit-display.md) | `ui.allied-unit-display` | The minimap and info panel show allied units and their details. | view | implemented |
| [Audio](standard-hacks/ui.audio.md) | `ui.audio` | Audio changes: 3D sound, music from numbered or found files, and limits on announcements. | view | implemented |
| [Build Preview](standard-hacks/ui.build-preview.md) | `ui.build-preview` | Shows the building under the build cursor before it is placed. | view | implemented |
| [Build Tools](standard-hacks/ui.build-tools.md) | `ui.build-tools` | Adds line and ring building, dragging of queued orders, and a warning colour when units must clear a site. | view | implemented |
| [Camera Sharing](standard-hacks/ui.camera-sharing.md) | `ui.camera-sharing` | Allies' camera positions show on the minimap, and watchers can follow a player's camera. | view | implemented |
| [Click Snap](standard-hacks/ui.click-snap.md) | `ui.click-snap` | Build and reclaim cursors snap to the nearest metal spot or feature. | view | implemented |
| [Display Modes](standard-hacks/ui.display-modes.md) | `ui.display-modes` | Display changes: a 1024x768 minimum, the menu resolution and screenshots. | view | implemented |
| [Effects Tweaks](standard-hacks/ui.effects-tweaks.md) | `ui.effects-tweaks` | Changes explosion effects: end-smoke weapons add the explosion, and the extra smoke puff can go. | view | implemented |
| [End-of-Game Statistics](standard-hacks/ui.endgame-stats.md) | `ui.endgame-stats` | End-of-game statistics list players who dropped or were removed. | view | implemented |
| [Explosion Flash](standard-hacks/ui.explosion-flash.md) | `ui.explosion-flash` | Draws the flash of explosions at a lower level, which the player's own setting can lower further. | view | implemented |
| [External Exports](standard-hacks/ui.external-exports.md) | `ui.external-exports` | Exports live unit state and the battle room's state for other programs. | view | implemented |
| [Interface Fixes](standard-hacks/ui.interface-fixes.md) | `ui.interface-fixes` | A set of small display fixes, each chosen by name. | view | implemented |
| [Map Features Always Drawn](standard-hacks/ui.map-features-ignore-los.md) | `ui.map-features-ignore-los` | Features the map places are drawn whether or not they are in line of sight. | view | implemented |
| [Megamap](standard-hacks/ui.megamap.md) | `ui.megamap` | Adds a full-screen strategic map with category icons and sensor rings. | view | implemented |
| [Options Dialog](standard-hacks/ui.options-dialog.md) | `ui.options-dialog` | Adds a settings dialog and options page for the added settings, and a chat macro key. | view | implemented |
| [Resource Panel](standard-hacks/ui.resource-panel.md) | `ui.resource-panel` | Watchers see every player's resources and can switch their view to a player and follow it. | view | implemented |
| [Selection Shortcuts](standard-hacks/ui.selection-shortcuts.md) | `ui.selection-shortcuts` | Adds selection shortcuts: same type, filters while dragging and larger queue steps. | view | implemented |
| [Share Dialog and Lobby Buttons](standard-hacks/ui.share-dialog-and-lobby-buttons.md) | `ui.share-dialog-and-lobby-buttons` | Expands the share dialog and adds buttons to the battle room. | view | implemented |
| [Text Rendering](standard-hacks/ui.text-rendering.md) | `ui.text-rendering` | Sends and reads chat as UTF-8, for every language, and can force the message log's backdrop on. | view | implemented |
| [Unit Voice Fixes](standard-hacks/ui.unit-voice-fixes.md) | `ui.unit-voice-fixes` | Changes when the reclaim and landing voices play. | view | implemented |
| [Veterancy Label](standard-hacks/ui.veterancy-label.md) | `ui.veterancy-label` | The info panel shows a unit's veterancy level as a number. | view | implemented |
| [Whiteboard](standard-hacks/ui.whiteboard.md) | `ui.whiteboard` | Allies draw lines, dots and text markers on the map for each other. | view | implemented |

### Network

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Chat Extension Channel](standard-hacks/network.chat-extension-channel.md) | `network.chat-extension-channel` | Chat packets carry private add-on messages to the handlers that take them. | sim | implemented |
| [Clear Session Description](standard-hacks/network.session-desc-clear.md) | `network.session-desc-clear` | The host clears one field of the session description before publishing it. | sim | implemented |
| [Commander Start Sync](standard-hacks/network.commander-start-sync.md) | `network.commander-start-sync` | At a set tick each machine sends where its commanders stand, and the others place them there. | sim | implemented |
| [Host Stays as Watcher](standard-hacks/network.host-stays-as-watcher.md) | `network.host-stays-as-watcher` | A defeated host stays in the game as a watcher, so the session goes on. | sim | implemented |
| [Keep Remote Player Colours](standard-hacks/network.preserve-remote-player-colour.md) | `network.preserve-remote-player-colour` | A remote player's colour and position slot survive player updates during a game. | sim | implemented |
| [Lag Guard](standard-hacks/network.lag-guard.md) | `network.lag-guard` | While no remote player is heard from, the simulation slows to one step an interval and pausing is blocked. | sim | implemented |
| [Recorder Session Commands](standard-hacks/network.recorder-session-commands.md) | `network.recorder-session-commands` | Adds opt-in recorder chat commands for pausing, starting, watching and choosing random maps. | sim | implemented |
| [Version Check](standard-hacks/network.vercheck.md) | `network.vercheck` | Clients challenge each other to compare their game data and report any mismatch. | sim | implemented |
| [Vote to Remove Players](standard-hacks/network.vote-reject.md) | `network.vote-reject` | Removing a timed-out or unwanted player becomes a vote of the players. | sim | implemented |

### Orders

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Build Site Kickout](standard-hacks/orders.build-site-kickout.md) | `orders.build-site-kickout` | Players may place buildings over their own units, which then clear the site, and blocked builders retry longer. | sim | implemented |
| [Builder Patrol and Guard Options](standard-hacks/orders.con-patrol-guard-options.md) | `orders.con-patrol-guard-options` | Each player chooses what patrolling and guarding builders do, for each standing move order. | sim | implemented |
| [Fire While Building](standard-hacks/orders.weapons-free-while-busy.md) | `orders.weapons-free-while-busy` | A builder's weapons stay free to fire while it builds, repairs, reclaims or captures. | sim | implemented |
| [Reclaim Cursor on Any Unit](standard-hacks/orders.reclaim-command-any-unit.md) | `orders.reclaim-command-any-unit` | In Reclaim mode the cursor offers reclaim over any unit; the reclaim still refuses what it cannot take. | sim | implemented |
| [Repair Finish Check](standard-hacks/orders.repairing-state-target-activity.md) | `orders.repairing-state-target-activity` | The repair order's finishing check looks at the target's activity, not the repairing unit's. | sim | implemented |
| [Resurrectors Reclaim Wrecks](standard-hacks/orders.resurrector-reclaims-features.md) | `orders.resurrector-reclaims-features` | The Reclaim command reclaims a wreck even when the builder can resurrect it; right-click still resurrects. | sim | implemented |
| [Selective Weapon Use](standard-hacks/orders.selective-weapon-occupy.md) | `orders.selective-weapon-occupy` | An attack order occupies only the ordered weapon, leaving the others free to choose their own targets. | sim | implemented |

### Recorder

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Game Recorder](standard-hacks/recorder.ta-demo-recorder.md) | `recorder.ta-demo-recorder` | The game recorder is present: it records games and carries recorder messages between players. | sim | implemented |
| [Ten-Player Replay](standard-hacks/recorder.ten-player-replay.md) | `recorder.ten-player-replay` | A recording of a full ten-player game can be replayed and watched. | sim | implemented |

### Repair

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Repair Rate](standard-hacks/repair.rate.md) | `repair.rate` | Repair heals in proportion to the builder's work time and the unit's build time instead of one hit point a step. | sim | implemented |
| [Self-Repair by Heal Time](standard-hacks/repair.healtime-self-heal.md) | `repair.healtime-self-heal` | Self-repair skips unfinished units and takes its pace and amount from the unit's heal time. | sim | implemented |

### Sharing

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Structure Gift Rate Limit](standard-hacks/sharing.structure-gift-rate-limit.md) | `sharing.structure-gift-rate-limit` | Structure gifts are sent in batches, and a batch over the limit waits. | sim | implemented |
| [Take and Give Commands](standard-hacks/sharing.recorder-take-give.md) | `sharing.recorder-take-give` | Adds opt-in chat commands with which a player lets named players take their units when they go silent. | sim | implemented |
| [Take Waits for Commander Deaths](standard-hacks/sharing.take-requires-live-commander.md) | `sharing.take-requires-live-commander` | Taking over another player's units is refused while a destroyed commander is still in the world. | sim | implemented |

### Teams

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Alliance Menu Everywhere](standard-hacks/teams.alliance-menu-all-game-types.md) | `teams.alliance-menu-all-game-types` | The alliance menu opens in every game type, so players can ally computer players outside multiplayer. | sim | implemented |
| [Keep Allied Victory](standard-hacks/teams.allied-victory-kept.md) | `teams.allied-victory-kept` | The battle room keeps a player's Allied Victory choice whatever their team. | sim | implemented |
| [Team Number Alliances](standard-hacks/teams.team-number-alliances.md) | `teams.team-number-alliances` | Team numbers set alliances: team-mates ally, other teams do not, and the host can assign teams. | sim | implemented |

### Units

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Build Rotation](standard-hacks/units.build-rotation.md) | `units.build-rotation` | Buildings may be placed facing any of four directions when their definition allows it. | sim | implemented |
| [Cloak After Build](standard-hacks/units.init-cloaked-after-build.md) | `units.init-cloaked-after-build` | An unfinished unit neither cloaks nor pays for its cloak. | sim | implemented |
| [Ignore Unit 0 Deaths](standard-hacks/units.ignore-null-death-record.md) | `units.ignore-null-death-record` | A unit death record for unit 0 is ignored. | sim | implemented |
| [Mobile Unit Yard Maps](standard-hacks/units.mobile-unit-yardmap.md) | `units.mobile-unit-yardmap` | Mobile units read their yard map too, not only buildings. | sim | implemented |
| [Placement by Builder](standard-hacks/units.placement-by-builder.md) | `units.placement-by-builder` | The selected builder decides whether a build-menu click places on the map or queues in a factory. | sim | implemented |
| [Skip Empty Yard Maps](standard-hacks/units.skip-empty-yardmap.md) | `units.skip-empty-yardmap` | A unit with an empty yard map or no footprint builds no yard map. | sim | implemented |
| [Unit Slot Reuse Delay](standard-hacks/units.id-reuse-delay.md) | `units.id-reuse-delay` | A dead unit's slot is not reused until a delay has passed since its death. | sim | implemented |
| [Water State Rules](standard-hacks/units.water-state-rules.md) | `units.water-state-rules` | A unit's water state is worked out in a new order, and units created under water start submerged. | sim | implemented |

### Veterancy

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Veterancy Model](standard-hacks/veterancy.model.md) | `veterancy.model` | Veterancy levels come from per-type kill thresholds, with adjustable bonuses, caps and accuracy. | sim | implemented |

### Vision and Radar

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [Allied Line of Sight](standard-hacks/intel.allied-los-sharing.md) | `intel.allied-los-sharing` | Allies share line of sight, radar and sonar coverage and unit visibility. | sim | implemented |
| [Ignore Allied Jammers](standard-hacks/intel.allied-jammers-ignored.md) | `intel.allied-jammers-ignored` | Jammers of the viewer and the viewer's allies no longer jam the viewer's radar. | sim | implemented |

### Weapons

| Hack | Id | What it does | Scope | Status |
| --- | --- | --- | --- | --- |
| [High-Arc Ballistics](standard-hacks/weapons.high-arc-ballistic.md) | `weapons.high-arc-ballistic` | Ballistic weapons fire on the high arc when the flat arc is below the barrel's minimum angle. | sim | implemented |
| [Retarget Out of Range](standard-hacks/weapons.retarget-out-of-range.md) | `weapons.retarget-out-of-range` | A weapon drops a target that leaves its range and acquires a new one. | sim | implemented |
| [Timed Shell Detonation](standard-hacks/weapons.timed-shell-detonation.md) | `weapons.timed-shell-detonation` | A ballistic shell whose timer runs out explodes, unless its weapon opts out, instead of fizzling. | sim | implemented |
| [Vertical Launch Before Turret](standard-hacks/weapons.vlaunch-before-turret.md) | `weapons.vlaunch-before-turret` | A weapon that is both vertical-launch and turreted fires as a vertical-launch weapon. | sim | implemented |
<!-- END GENERATED: hacks table -->

## Every script extension

The table is generated from the registry, in the order of the extensions'
usual indices. Each page shows the `#define` and `get` a BOS script writes,
short examples, the `oamod.yaml` lines that mount it, and how `exact` and
`safe` fidelity answer for empty slots and unusual ids.
[Section 7](oamod-standard.md#7-script-extensions) of the standard covers
mounting and fidelity.

<!-- BEGIN GENERATED: script extensions table -->
| Extension | Id | Usual index | Argument | Returns |
| --- | --- | --- | --- | --- |
| [Kill Count Times 100](script-extensions/unit.kills-x100.md) | `unit.kills-x100` | 32 | — | The calling unit's kill count times 100 (a count, not a veterancy level). |
| [Lowest Unit Id](script-extensions/unit.min-id.md) | `unit.min-id` | 69 | — | 1, the lowest id a unit can have. |
| [Highest Unit Id](script-extensions/unit.max-id.md) | `unit.max-id` | 70 | — | The unit limit the game recorded, times 10. A skirmish or multiplayer game records its own limit, so this is the last id of the unit table; a campaign mission records the player's Unit limit setting. |
| [Own Unit Id](script-extensions/unit.my-id.md) | `unit.my-id` | 71 | — | The calling unit's own id. |
| [Owner of a Unit](script-extensions/unit.owner-of.md) | `unit.owner-of` | 72 | unit id | The number of the player that owns that unit's slot, 0 for the first player to 9 for the tenth. Under `exact`, id 0 answers 255, and in a multiplayer game an id in the range of a player place nobody took answers 10. |
| [Build Percent Left of a Unit](script-extensions/unit.build-percent-left-of.md) | `unit.build-percent-left-of` | 73 | unit id | That unit's `BUILD_PERCENT_LEFT`: 0 when it is finished, else 1 to 100. |
| [Allied With a Unit's Owner](script-extensions/unit.allied-with.md) | `unit.allied-with` | 74 | unit id | 1 when the calling unit's owner has allied the owner of that unit's slot, else 0. Alliance is one-way: the target's owner need not have allied back. |
| [Unit Played on This Machine](script-extensions/unit.is-local.md) | `unit.is-local` | 75 | unit id | 1 when the owner of that unit's slot is a human or computer player on this machine, else 0. |
<!-- END GENERATED: script extensions table -->
