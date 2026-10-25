# Game content

Open Annihilation knows two base games and six expansions. A game uses one
base game. Each expansion adds to Total Annihilation 3.1c and is left out
of the demo. A managed copy keeps every bundle in its own folder, under
the game's documents folder: `Games/totala` and `Games/totala-demo` for the
base games, and `Expansions/totala/<id>` for each expansion. The names
below are the names the copy uses. An install may spell the same file in
any case, or with `\` between the parts; the copy writes these names.

## Total Annihilation 3.1c

Id `totala`. This is the base game.

- `rev31.gp3`: the 3.1 update (unit definitions, scripts and the screens
  they belong to).
- `totala1.hpi`: units, models, scripts, sounds, screens and fonts.
- `totala2.hpi`: the multiplayer maps and the front-end art.
- `Data/1.zrb`, `Data/2.zrb` and `Data/5.zrb`: the logo, the intro and the
  credits.

## The demo

Id `totala-demo`. Its only file is `TADemo.hpi`, unpacked from the free
1997 demo installer the game already recognises. The demo takes no
expansion. Nothing that needs 3.1c is offered while the demo is the base
game. On Single Player, Skirmish is greyed out: it takes no press and
its key does nothing. Load Game stays greyed and Any Mission stays
hidden. New Campaign, Options and Previous Menu stay available. The
full game leaves Skirmish available. When no side has a campaign (the
Campaign expansion is off, or a mod hides every campaign file with an
empty file of the same name), New Campaign is greyed and Any Mission
is hidden. The demo keeps New Campaign.

## Expansions

Each expansion has an id a mod can name.

**Campaign** (`campaign`) needs 3.1c. It holds the Arm and Core campaigns
and their mission maps in `totala4.hpi`, and the two campaign endings in
`Data/3.zrb` and `Data/4.zrb`.

**Core Contingency** (`core-contingency`) needs the campaign. It holds
`ccdata.ccx` (its units), `ccmaps.ccx` (its maps) and `ccmiss.ccx` (its
campaigns).

**Battle Tactics** (`battle-tactics`) needs the campaign, and does not need
Core Contingency. It holds `btdata.ccx`, `btmaps.ccx` and `tactics1.hpi`
through `tactics8.hpi`.

**Music** (`music`) needs only 3.1c. It holds `music/0.mp3` through
`music/17.mp3`, the soundtrack. Turning it on plays that music. It does not
change units, maps or the rules, and a comparison of two games' rules
leaves it out.

`music/0.mp3` is the same recording as `music/9.mp3`. A source that has
one and not the other is copied as both. A soundtrack file the source
does not have at all, which the macOS installer does for `music/13.mp3`,
is left out and is not reported as a damaged file. The Music pill then
reads how many tracks that source held, such as "17 of 18 tracks". A
track that is there and does not match is still reported.

**Cavedog units** (`cavedog-units`) needs Core Contingency. It holds six
unit files, copied as `afark.ufo`, `aflea.ufo`, `ascarab.ufo`,
`cormabm.ufo`, `cornecro.ufo` and `corplas.ufo`: FARK, Flea, Scarab,
Hedgehog, Necro and Immolator. An install may spell those names with
capitals; the copy uses the names here.

**Cavedog tilesets** (`cavedog-tilesets`) needs only 3.1c. It holds
`worlds.hpi`, the terrain sections a map is built from. It is copied when
the install has it.

Turning an expansion on also turns on everything it needs. When 3.1c is
already the base game, turning the campaign on also turns on Core
Contingency and Battle Tactics, which were waiting only on the campaign.
The Cavedog units stay off until Core Contingency is on. Music and the
tilesets stay as they were: each was waiting on 3.1c, not on the campaign.

Turning an expansion off also turns off every expansion that needs it.
Turning the campaign off turns off Core Contingency, Battle Tactics and
the Cavedog units. Music and the tilesets stay on. Choosing the demo, or
otherwise leaving 3.1c, turns off all six.

## What a copy leaves out

`totala3.hpi` is part of the installer and is not a file of any bundle.
`Example.ufo`, and the other sample files that come with an install, are
not copied either.

Four maps that ship beside the game are not copied as loose files. Core
Prime already publishes each of them as its own map pack: Comet Catcher,
Luschinfloggen, Mounds of Mars, and PC Games' Evad River Delta.

## Installers

Three published Commander Pack installers are recognised without being
run, by the file's size and its SHA-256: the Windows installer
`setup_total_annihilation_commander_pack_3.1_(22139).exe`, the macOS
package `total_annihilation__commander_pack_en_1_3_15733.pkg`, and the
disk image for older Macs
`total_annihilation_commander_pack_2.0.0.10.dmg`. The 1997 demo installer
is separate from these three, and is recognised where it is today. An
attended start reads the Windows installer, when the installation is that
file, and copies the game out of it. The disk image is read in place: it is
not mounted. `--check-installer FILE` lists the bundles found in a
recognised installer and every file's SHA-256, and does nothing else. A
bundle with no file in the installer is left out.

## For a mod author

A mod names the id of each base game it runs on and each expansion it
needs, from the ids on this page. A mod that needs Core
Contingency names `totala` and `core-contingency`: the campaign comes with
Core Contingency, so the mod does not name the campaign as well unless it
uses the campaign's own files. The same ids are what a map pack names when
a map needs an expansion. Music is not a requirement of a mod's rules.
