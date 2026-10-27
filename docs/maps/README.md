# Map packs

A map pack adds maps to a game. It is one `.oamap` file: a manifest,
`oamap.yaml`, and the maps' own files, their features, the animations and
models those features use, and previews for the map browser. A pack only
adds. It does not carry units, weapons, sounds or textures, and playing one
of its maps loads that map's files alone.

This page is for players: getting a pack, what its maps are called, how
OA decides whether a map fits the game you play, the map browser, and GET
MAP in the battle room.

- [The OAMAP standard](oamap-standard.md) is the format of `oamap.yaml` and
  the rules a map pack follows: the manifest, the index, names, what a pack
  holds, and how a map fits the game it is played with. It is for people
  who make packs.

## Contents

- [Installing a map pack](#installing-a-map-pack)
- [Map names](#map-names)
- [One map's files at a time](#one-maps-files-at-a-time)
- [A mod's settings for a pack's map](#a-mods-settings-for-a-packs-map)
- [Whether a map fits](#whether-a-map-fits)
- [The map lists and MORE MAPS…](#the-map-lists-and-more-maps)
- [The map browser](#the-map-browser)
- [GET MAP in the battle room](#get-map-in-the-battle-room)
- [What 3.1c players see](#what-31c-players-see)

## Installing a map pack

A pack comes from a registry or from a file, and installs into the `Maps`
folder of your own Open Annihilation folder, as `Maps/<id>`, named by the
pack's id whatever the file is called
([where that folder is](../installation/macos.md#where-it-keeps-its-files)).

**From a registry**, Core Prime's or one you added: GET on the Library's
Maps tab ([the Library](../library.md)), **Download and select** in the map
browser ([below](#the-map-browser)), or GET MAP in the battle room
([below](#get-map-in-the-battle-room)). A pack new to you, or one that
replaces the copy the same registry installed, installs with no question,
on any screen but a game. When that pack is in place, no notice is shown:
the Library's row says so. When your Maps folder already holds that pack
from a file, made by hand or from another registry, OA asks before it
replaces it, as for a file, whichever registry the download comes from, and
the question waits for the main menu.

**From a file**: double-click an `.oamap` file, choose Open With and Open
Annihilation, drag it onto the game's window, or start the game with its
path. It waits for the main menu, and asks before it writes anything:

| Your Maps folder holds | The question |
|---|---|
| no folder of the pack's id | **INSTALL MAP PACK**: "Install the map pack *name* *version* (*n* maps)?", **CANCEL** or **INSTALL** |
| the same pack and version, another revision | **UPDATE MAP PACK**: replace it with that revision, **CANCEL** or **REPLACE**; it says when the file's revision is the older |
| the same pack, version and revision | **REINSTALL MAP PACK**: install its files again, **CANCEL** or **REINSTALL** |
| the same pack at another version | **REPLACE MAP PACK**, **CANCEL** or **REPLACE** |
| a folder of that name that holds something else | refused: the folder is left as it is, and the log says so |

A folder of the pack's id that holds nothing but empty folders, as a file
sync can bring back after you uninstall the pack, counts as no folder: the
install removes those empty folders and puts the pack there. One file in
it, of your own or anyone's, and it holds something else.

A replace keeps the copy it replaced in the pack's folder, as `.backup`.
When the pack is in place, **MAP PACK INSTALLED** says where, with **OPEN
FOLDER** and **OK**.

Nothing installs during a game. Every map in a pack is checked as it is
installed against the game you have, where the game finds a map's
features: Total Annihilation 3.1c, the expansions that are on, and each
expansion the pack's `requiredExpansions` names (with those it needs) that
you have copied, but never a mod. A pack with a map that clashes with them
is refused ([whether a map fits](#whether-a-map-fits)), and so is one with
a map that places a feature none of them defines, naming it:
**MAP PACK NOT INSTALLED** names the file and why. Core Contingency's
water metal (`ArchiAquaOre1` to `ArchiAquaOre3`), which Cavedog's Starfish
Isle, Tropical Paradise and Long Lakes place, is defined in Core
Contingency's own archive, so those packs install while Core Contingency
is on. A pack is refused too
when its manifest has errors, when it holds files outside the folders a map
pack may hold (`maps`, `features`, `anims`, `objects3d` and `previews`) or
files no map uses, when a preview is not a PNG of at most 2 MiB and 1024
pixels a side, or when it needs a later Open Annihilation. A pack from a
registry that is refused says only "the package was refused" in the
Library's queue, the map browser or GET MAP's line. Nothing is changed
when a pack is refused.

To remove a pack, use **UNINSTALL** in the Library
([uninstalling](../library.md#uninstalling)), or delete its folder while
the game is not playing one of its maps (**OPEN FOLDER** in the Library
shows it). In 0.8 a map pack does not roll back: ROLL BACK is for mods.

## Map names

A pack's map is named by its own file name, the stem, then `@` and the
pack's id: the map `isle_of_ashes` in the pack `archipelago` is
`isle_of_ashes@archipelago`. Pack ids are unique across a registry, so two
packs' maps never share a name, whatever their authors called them. The
whole name fits in 63 bytes, so that it travels in the battle room and in
a saved game; the pack refuses a map whose name would not.

The map browser, the skirmish map list and the battle room's map list show
a map's own title, "Isle of Ashes". When that title would read the same as
another row, the pack map's row adds its pack's name in brackets, "Comet
Catcher (Cavedog: Comet Catcher)". Choosing the map, the battle room's
name sent to other players, a saved game and GET MAP use its name,
`isle_of_ashes@archipelago`. A 3.1c player's battle room receives that
name too. The game's own maps stay listed by their file names.

## One map's files at a time

When you choose a pack's map, in the skirmish map list or the battle room,
OA adds that map's files, and only those, to the game: under 3.1c's own
data and the mod you play, so that nothing of the game or the mod changes.
No other pack's maps, nor the pack's other maps, are there at the same
time, so two maps can never clash, whatever they name their features or
files. The map's files stay through the match, and go when you choose
another map, when the match ends, and on the main menu.

## A mod's settings for a pack's map

Some mods ship their own OTA for a map the game does not include, without
the map's terrain: Total Mayhem carries its settings, such as the computer
player's profile, for Cavedog's downloadable maps. Played over the game
alone, such an OTA has nothing to play on, so the lists leave it out, as
they leave out any map whose terrain no file holds. The log names those
maps once each time the lists are built:

```
open-annihilation: map list: left out, their terrain is missing: <maps>
```

When an installed pack has a map of the same file name, the mod's OTA is
that map's settings while you play the mod: the pack's map is mounted with
the mod's OTA in place of its own, over the pack's terrain, as the mod's
OTA took the place of the map's own when players had the map's files. The
lists, the battle room and the map browser show the mod's title,
description, size and player counts for it, and the map is checked as it
is mounted, the mod's OTA with the pack's files. The log line that mounts
it ends `with the mod's settings from maps/<stem>.ota`. Only a mod's own
files count, and only an OTA with no terrain: a mod that ships a whole map
of that name keeps its own map, and the pack's map keeps its own OTA.
Every player in a match plays the same mod and the same pack, so all of
them play the map with the mod's settings.

## Whether a map fits

A pack's map may use 3.1c's rocks and trees, but may not replace or redefine
anything in the game or the mod it is played with:

- **Adds only**: none of its files is a file the game or the mod already
  has, ignoring case;
- **New names**: every feature its files define is new to the game and the
  mod;
- **Complete**: everything it uses is in its own files, the game's or the
  mod's;
- **Isolated**: only its own files are added, and no other map's are
  visible beside it.

A map that clashes with 3.1c itself keeps its whole pack out at install.
One that clashes only with a mod, because the mod already defines a
feature the map adds, say, is installed. The skirmish list and the battle
room's map list still show it, greyed, with the reason, and choosing it
explains why. The same is true when the pack does not support the base
game you are playing, or when an expansion it requires is off. The log
names a map that cannot be played for good, once for that pack and that
game:

```
open-annihilation: map pack <id>: 1 map cannot be played with <key>
```

`<key>` names the base game, then each expansion that is on, in the
built-in order, other than music, then `<catalogue-id>@<release>` when a
mod installed from a catalogue is played. It is `the mod being played`
for a mod that has no catalogue key. Several maps of one pack share one
line, and then it reads `maps cannot be played with`. A pack from a
registry follows the catalogue when that catalogue has answered for this
game: a new name such
as `totala` or `totala+core-contingency` wins, and the older names
`ta-3.1c` and `<catalogue-id>@<release>` are read when the table has no
new name. True can be played, and false is greyed, without reading the
map's files. A mod the catalogue has no row for, including one that was
not installed from a catalogue, can be played when the map's files fit,
and is greyed when they do not. A pack installed from a file is judged
the same way, by its files. A release the catalogue has not listed yet is
judged by its files until that table arrives, and a miss then is not
logged. Nothing is mounted to build either list.

The map browser still shows the map: its card is greyed with "Doesn't
fit", and Select is greyed ([below](#the-map-browser)). Choosing a greyed
map, from a save or by its name, is refused and keeps the map you had.

OA checks a map again each time it adds its files, and when a match
starts, against the game and the mod being played then.

## The map lists and MORE MAPS…

The skirmish list and the battle room host's map list hold every installed
pack's maps beside the base maps, in one order by the name the row shows.
A pack map's row shows the title from the pack's index, or the title in
its map file when the index has none. A row that would read the same as
another adds the pack's name in brackets. A map that cannot be played is
greyed in that place, and the row adds the reason: "for the TA demo",
"for Total Annihilation", "needs Battle Tactics", "for ridge", or
"doesn't fit". Choosing that row says why, and a missing expansion says
to turn it on in the Library. The game's own rows stay their file names.
A listed map whose files cannot be loaded shows its name and the game's
own "The terrain for the selected map does not exist.", with no picture,
and choosing it says the same and keeps the map you had.
The name under a list, and the battle room's map name, are that same
title. Choosing the row still uses `<stem>@<id>`.

A pack installed from the Library joins both lists at once. A pack
removed leaves both lists at once.

Under the skirmish map list, and under the battle room's map list for the
host, **MORE MAPS…** sits clear of the list's frame, at the same size.
It opens the map browser, and so do Cmd+F on macOS, Ctrl+F elsewhere and
a controller's Y.

## The map browser

The map browser shows every map you can play, and every map a registry
offers, in one grid with previews, laid out for the window at your
Interface size and Window size
([settings](../settings.md#user-interface)). It moves by its title, as
Settings does, and opens where you left it:

- **The header** says "Maps", where you came from and what you play:
  "Skirmish · No Mod", "Battle room · playing Ridge". The **X** at the top
  right closes the browser, as in the Library and Settings.
- **Filters**, as counted pills: All, Game (the base game and every
  expansion that is on), Mod (maps from the mod's own layers) and Downloads
  (packs you have installed and packs a catalogue offers, together), each
  with its count, as in "All (324)". **PLAYERS** and **SIZE** are one
  choice each, Any first, then 2, 3, 4 or 5–10, and S, M or L (S up to 10
  on its longer side, M up to 20, L above). "Search maps" finds a map by
  its title, its name, its pack's name or where it comes from.
- **The cards** are in order of their titles, a map online among the rest,
  and a map you have installed shown once. Each shows its preview, "Loading
  preview" until it is drawn, or "No preview"; a chip naming where it comes
  from ("TA 3.1c", "Core Contingency", "Battle Tactics", the mod's name,
  the pack's name, "Downloaded" when you have a pack named for the map,
  "GET" when that pack is still to get, or "Downloading…" while it is);
  and "4 · 16×16", its players and size. Previews are drawn only for the
  cards in view. Pictures of the three sizes are in
  [images/maps](../images/maps/).
- **The footer** shows the chosen map: its title; the same source name as
  its chip for a game or mod map, or its pack, version and registry, then
  its players, size and, for a map online, the download's size; whether it
  fits, "Fits No Mod ✓", "Doesn't fit Ridge" with the reason, or, for a map
  online, "Fit with No Mod is checked when it's installed" or what the
  registry's catalogue says ("Its catalogue says it does not fit Ridge.").
  At Compact the line under the title is only the pack, or that source
  name, and the player count, and the fit chips sit on that line.
- **Select**, for a map you have, chooses it in the list you came from,
  where you then press LOAD or start as ever. **Download and select**, for a
  map online, downloads its pack as the Library's GET does, shows how far it
  has come, and chooses the map once the pack is installed. **Cancel**, and
  the **X**, close the browser; a download already started goes on.

A map online that its catalogue says does not fit what you play is greyed,
and OA checks it again against what you have once it is installed.

## GET MAP in the battle room

When the host plays a pack's map you do not have, the battle room's map
name flashes and you cannot ready up, as in 3.1c. If the host is on Open
Annihilation 0.8 and installed that pack from a registry, its presence
record names the pack, its release and its registry, and your battle room
shows **GET MAP** above the map's name, with a line under it:

| You | The line | GET MAP |
|---|---|---|
| can get it | "Archipelago Pack 1.2 · 2.3 MB from Core Prime" | downloads exactly the host's release and installs it |
| have not added the host's registry | "Add *registry* to get Archipelago Pack" | opens Add registry with the host's registry's address, checked, for you to ADD or not ([adding a registry](../downloads.md#adding-a-registry)); once added, the download goes on by itself |
| have another release | "You have release 2 of Archipelago Pack; the host plays release 3." | downloads the host's release, while the registry still offers it |
| cannot get the host's release | "The host's release of Archipelago Pack is no longer offered by Core Prime." | not shown |
| have downloads from that registry off | "Downloads from Core Prime are off (Settings › Downloads)." | not shown |
| are downloading it | "Downloading Archipelago Pack · 1.1 of 2.3 MB", with a bar | not shown |
| saw the download fail | "Couldn't get Archipelago Pack: *why*" | tries again |

The pack installs as a download from its registry does
([which downloads ask first](../library.md#which-downloads-ask-first)), and
the battle room then finds the map and stops flashing its name. GET MAP
never readies you: you press READY yourself. Two releases of one pack can
hold different features and pictures under the same map, which the map's
check would not notice, so OA fetches the host's exact release, never
another.

A host who installed the pack from a file, an OA 0.7 host and a 3.1c host
send no pack in their presence record, or no record at all, so a joiner
sees no GET MAP; get the pack from the Library, the map browser or a
file instead. The host's player card says whether you have its pack:
"✓ installed", "another release here" or "not installed"
([network games](../../README.md#network-games-from-the-command-line)).

## What 3.1c players see

Nothing new. A pack's map is a map a 3.1c player does not have: their
battle room says so with the missing-map line, naming the map as
`isle_of_ashes@archipelago`, and they cannot ready up, as for any map they
lack. Packs change nothing that 3.1c sends or expects.
