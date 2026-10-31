# User preferences

This platform boundary stores the game's preference values as strings in a
per-user file outside the installation. The preference names, values and
defaults belong to the frontend.

| Platform | Location |
| --- | --- |
| macOS | User Application Support / `net.coreprime.open-annihilation/preferences.conf` |
| Windows | Local AppData / `CorePrime/Open Annihilation/preferences.conf` |
| Linux | `$XDG_CONFIG_HOME/open-annihilation/preferences.conf`, or `$HOME/.config/open-annihilation/preferences.conf` |

Beside the preferences file the game keeps files of its own, in the
same format, which it writes itself and the player never needs to edit:
`renderer-state.conf`, what it has seen of each graphics driver on this
machine (`src/app/include/oa/app/renderer_state.hpp`), and
`renderer-sentinel.conf`, the stage a start has reached, which a clean exit
deletes. Deleting either loses nothing but that record. With
`--preferences-file` neither of those two is read or written.
`mutators-sentinel.conf` names the mutators in play while a match with
mutators runs, and a clean match end or a clean exit deletes it. A start
that finds one keeps what it names and deletes the file, so the next start
does not ask again. It lives beside whichever preferences file the run
uses, including one `--preferences-file` names. Two runs whose
preferences files share a folder share this file, so runs at the same
time use separate folders.

Data the engine keeps for the player, such as game data it unpacks, goes in a
per-user data folder (`data_directory()`), beside the preferences file on
macOS and Windows and in the XDG data folder on Linux:

| Platform | Location |
| --- | --- |
| macOS | User Application Support / `net.coreprime.open-annihilation` |
| Windows | Local AppData / `CorePrime/Open Annihilation` |
| Linux | `$XDG_DATA_HOME/open-annihilation`, or `$HOME/.local/share/open-annihilation` |

The player's own files, their saved games, screenshots, films, recordings
and mods, go in a folder named `Open Annihilation` (`user_folder_name`) in their
Documents folder (`documents_directory()`, `default_user_folder()`), where
they can find them:

| Platform | Documents folder |
| --- | --- |
| macOS | the user's Documents directory, as Foundation resolves it (it does not follow `$HOME`) |
| Windows | the Documents folder (My Documents on Windows XP), wherever it has been redirected, through `SHGetFolderPathW(CSIDL_PERSONAL)` |
| Linux | the folder `XDG_DOCUMENTS_DIR` names in `user-dirs.dirs` under `$XDG_CONFIG_HOME`, or `$HOME/.config` when that is unset or relative; else `$HOME/Documents` |

On Linux, `xdg_documents_directory()` reads the `user-dirs.dirs` text as
`xdg-user-dirs` writes it: `XDG_DOCUMENTS_DIR="$HOME/name"` or an absolute
`"/path"`, a backslash keeping the character after it, the last such line
counting; any other line, a relative path or another variable is passed
over. The folder need not exist: the game makes what it needs when it first
needs it. What the folder holds, the one-time move of saved games into it
and the keys below are the game's (`src/app/include/oa/app/user_folder.hpp`).

Earlier versions named the macOS folder `com.coreprime.open-annihilation`. When
the engine resolves the folder and finds only that one, it renames it to
`net.coreprime.open-annihilation`, once, so the preferences file keeps
working and an earlier unpack of the demo (`demo-1997`) stays where a later
start can copy it once into `Games/totala-demo` in the player's Open
Annihilation folder. When both exist, it uses the new
folder and leaves the earlier one alone. When the rename fails, it uses the
earlier folder under its old name for that run, says so on standard error, and
tries again on the next start; the files are never copied, merged or replaced.
`apple_data_directory()` does this for a given Application Support folder,
which lets the test run it in a temporary one.

Apple's Foundation API resolves the application-support directory, including the
container location in a sandboxed application. Windows uses the Known Folder
API, rather than assuming a drive or user-profile layout. XDG overrides must be
absolute. Missing or invalid user-directory resolution produces an error, never
a fallback into game assets or the working directory.

The versioned UTF-8-compatible text representation quotes and escapes keys and
values. Its lines may end in LF or CR LF, so a file edited on Windows reads
as the game wrote it; the game writes LF. Reads are bounded and reject corrupt files and duplicate keys. Saves
write a complete temporary file beside the destination, flush it, and replace
the destination. A failed write preserves the last complete settings file;
concurrent game instances use last-writer-wins replacement, not merging.
`save(..., SyncFolder::yes)` also syncs the folder after the replace on
POSIX systems, so that a system crash leaves the old file or the new one;
Windows writes the replace through to the disk either way. `overwrite()`
writes the same format straight into the file, with no temporary file,
rename or flush, for a small file rewritten often whose loss in a crash is
acceptable: a write cut short leaves a file `load()` rejects.

The native application imports the earlier `open-annihilation.ini` from the game
directory only when the new preference file is absent. That migration reads the
legacy file without modifying it. Subsequent saves use the platform location.

Settings the engine adds for itself use keys with the `open-annihilation.` prefix
and no `|`, so they never collide with the game's `<section>|<name>` keys.
`open-annihilation.game-directory` holds the Total Annihilation folder chosen
in the first-start dialog as a UTF-8 path. It is written after that migration,
once the game starts from the folder.
`open-annihilation.mod-directory` holds the mod folder the player chose in
the Library to layer over that folder, as an absolute UTF-8 path; absent,
the game folder plays as it is, as after SWITCH OFF.
`open-annihilation.picked-mod-directory` holds the folder an earlier
version's Pick Folder... chose, as an absolute UTF-8 path, which the
Library lists while the folder still exists. It is kept only while
`open-annihilation.mod-directory` names the same folder: once another mod
or No Mod is chosen, it is erased. Absent, there is none.
`open-annihilation.base-game` holds the base game the player plays,
`totala` or `totala-demo`. Absent, or any other text, the choice is
`totala` with every copied expansion of it that can be on, which is what
a player of an earlier version plays. `open-annihilation.expansions.<game>`
holds the expansions that are on for that base game, their ids separated
by commas in the built-in order, as `campaign,core-contingency`. An empty
value means none are on. Absent, every copied expansion of that game that
can be on is on. An expansion that needs one which is not copied stays
off. Turning one off turns off every expansion that needs it. Turning one
on turns on what it needs and every copied expansion that was waiting only
on it. Choosing a base game turns on every copied expansion of it that can
be on and clears the mod. The change is kept for the next time the game
mounts its files, and it is refused while a match is running.
`open-annihilation.content-note` is `required-expansion` when a mod was
cleared because an expansion it needs went off. The sentence the player
reads is chosen from that token. Absent, no such note is waiting.
`--content GAME[:EXPANSION,...]` names the base game and expansions for
one run and writes nothing. An expansion the list needs is included; one
that was only waiting is not.
`open-annihilation.mutators.skirmish`,
`open-annihilation.mutators.multiplayer` and
`open-annihilation.mutators.campaign` each hold the last set that setup
started a game with, as the canonical loadout text: the mutators in
order, with versions, package SHA-256s and options. A setup's START
writes its key only when that game has mutators. `--mutator` and a
launch file never write them, and a game without mutators leaves them
as they are. Absent, or text that cannot be read as that loadout, the
setup has no set. Restore defaults does not erase them.
`open-annihilation.brain.default` holds the id of the brain the player
uses as the default computer player. Absent, empty, or an id that is not
installed, the built-in computer player is the default. Restore defaults
does not erase it. USE BY DEFAULT writes it. Uninstalling that brain
erases the key, so it does not keep naming a brain that is gone.
`open-annihilation.brains.skirmish`,
`open-annihilation.brains.multiplayer` and
`open-annihilation.brains.campaign` each hold the computer seats that
setup last started a game with, as the seat text: each seat's place and,
when it names one, its brain, level and options. A skirmish's START and a
campaign mission's start write their key when a seat names a brain, or
when the key is already there. A start whose seats name no brain, and
whose key is absent, writes nothing. The multiplayer key is the battle
room's and is written the same way when that setup starts a game. Absent,
or text that cannot be read as seats, the setup has no seats. Restore
defaults does not erase them.
`open-annihilation.user-folder` holds the folder the player keeps their
saved games, screenshots, films, recordings and mods in, as an absolute
UTF-8 path, in place of `Open Annihilation` in Documents; absent or
relative, the default holds, and `--user-folder` sets another for one run,
into which no saved games or recordings are moved. With
`--preferences-file` the default is the
`Open Annihilation` folder beside that file, so that a check never reaches
the player's Documents folder, and nothing beside that file is moved.
`open-annihilation.saves-moved` records that the saved games kept beside
the preferences file by earlier versions (`SAVEGAME`, and
`mods/<id>/SAVEGAME`) were moved into that folder's `Saves`, as the saved
games moved and the ones left where they were, `"12 0"`; while it is there
the move is not made again. `open-annihilation.saves-moved-notice` is `due`
while the main menu's notice of a move that moved or left a saved game
waits to be shown, and `told` once it has shown.
`open-annihilation.loose-saves-moved` and
`open-annihilation.loose-saves-moved-notice` do the same for the move of
3.1c's saved games loose in `Saves` into `Saves/default`.
`open-annihilation.recordings-moved` records, in the same way, that the
recordings kept beside the preferences file by earlier versions (`demos`,
and `mods/<id>/demos`) were moved into that folder's `Recordings`; no
notice follows that move.
A mod whose profile names a registry root of its own keeps the game's
`<section>|<name>` settings under `registry:<root>\<section>|<name>` keys
instead, as the mod keeps them under its own registry key, and its first run
seeds the values its profile names there.

`open-annihilation.install-id.<registry id>` holds that registry's install
ID: 32 lower-case hexadecimal digits in groups of 8, 4, 4, 4 and 12, such
as `7f3a90d2-4c18-4b0e-9a77-1d6e5ab0c91e`. It is a random 128-bit value, a
different one for each registry, so no two registries can tell that they
serve the same player. It is made the first time a download from that
registry needs one, not when the game starts, and it is sent only with
download requests. The value `off` turns that registry's ID off: the ID
that was stored is gone, and none is made while the key says `off`.
Absent, or any other text, means none has been made yet.

`open-annihilation.language-notice-dismissed` holds the tags of languages
the player answered with Use English when the game asked, at the first
start, about a language the operating system wanted that was not installed.
The tags are separated by commas. A tag in the list is not asked about
again. Absent, every such language is still asked about.

The Open Annihilation settings (`oa/ui/engine_settings.hpp`) keep these keys,
written when the player changes a setting and erased again by the settings
dialog's Restore defaults. Values are decimal, but for the screen size's; a
key that is absent or not a whole number gives the default, and a value out
of range is clamped. A switch is On for any number above 0: a word such as
`off` is not a number, so it gives the default; write `0` to turn one off.

| Key | Values | Absent means |
| --- | --- | --- |
| `open-annihilation.path-search-nodes` | path nodes a game tick, 1333 to 10664 | 1333 |
| `open-annihilation.wheel-zoom` | 0 or 1 | 1 |
| `open-annihilation.access.sticky-modifiers` | 0 or 1 | 0 |
| `open-annihilation.access.pointer-keys` | 0 or 1 | 0 |
| `open-annihilation.access.hold-to-confirm` | 0 or 1. Self-destruct, surrender and quitting need the long-press time, and a ring fills while the press is held | 0 |
| `open-annihilation.access.edge-scroll` | 0 or 1. Off stops the view scrolling while the pointer rests at the window's edge. Arrow keys and the touch edge band while a drag is held keep moving the view | 1 |
| `open-annihilation.access.move-overlays` | 0 or 1. On, the keys overlay, the chat line and the latch chips show at the places `open-annihilation.overlay.<overlay>.<class>` holds; off, where they lay themselves out. Settings › Accessibility reads and writes it | 0 |
| `open-annihilation.access.order-wheel` | `off`, `hold-right` or `hold-middle`. Holding that mouse button on the battlefield opens the order wheel; a short click or a drag still does what the button does. Any other text reads as `off` | `off` |
| `open-annihilation.middle-click-pan` | 0 or 1 | 1 |
| `open-annihilation.max-zoom-out` | `automatic`, `whole-map`, `1/32`, `1/16`, `1/8`, `1/4` or `1/2`; any other text reads as `automatic` | `automatic` |
| `open-annihilation.max-zoom-in` | `1`, `2`, `3` or `4` times the game's scale; any other text reads as `4` | `4` |
| `open-annihilation.view-past-map-edge` | `off`, or `25` or `50` percent of the battlefield past the map's edges; any other text reads as `50` | `50` |
| `open-annihilation.group-keys-centre` | `double-press`, `never` or `every-press`; any other text reads as `double-press`. Whether a group's key, which selects the group, also moves the view to it: on the second of two presses of the same group's key within the double-press time, never, or when the group is already exactly the selection, however long after. It applies at once | `double-press` |
| `open-annihilation.double-click-selects` | `on-screen` or `on-map`; any other text reads as `on-screen`. Where a double-click on one of the player's units, a controller's Select type, the touch radial's Type and a double tap select the units of its type: those on screen, or every one on the map, as 3.1c's double-click does. With ui.selection-shortcuts on, `on-screen` is the hack's double-click and `on-map` the game's. It applies at once | `on-screen` |
| `open-annihilation.focus-lost` | `pause` or `keep-playing`; any other text reads as `pause`. What a game played alone on this machine, a campaign mission or a skirmish against computer players, does while the window does not have the focus: `pause` pauses it and lets it go on half a second after the window has the focus again, leaving a pause the player chose as it was; `keep-playing` plays on. A multiplayer game and a replay are never paused. It applies at once | `pause` |
| `open-annihilation.window-edge` | `capture`, `sticky` or `free`; any other text reads as `capture`, and an earlier 0.8 build's `sticky` and `free` keep their meaning. While a match's battlefield shows in a focused window, Capture keeps the pointer in the window until a menu, Escape or a lost focus, Sticky holds it on the window's edges until a push or a flick as firm as `window-edge-stickiness` lets it leave, and Free leaves it free | `capture` |
| `open-annihilation.window-edge-stickiness` | a decimal from `0.0` to `5.0`, written with one decimal; another number is rounded to the nearest tenth and held to that range, and any other text reads as `1.0`. How firmly Sticky holds: the push and the flick that let the pointer go grow with it, and `0.0` never holds | `1.0` |
| `open-annihilation.escape-opens-menu` | 0 or 1 | 1 on macOS with the player's own file, else 0 |
| `open-annihilation.unit-limit` | units per player, 20 to 1500, or to a mod's higher maximum (`oa::data::limits::UnitsPerPlayer`) | the game folder's `totala.ini` `[Preferences] UnitLimit`, clamped to 20 to 500, with the player's own file; else 250 |
| `open-annihilation.max-fps` | frames a second, 30 to 120 | 120 |
| `open-annihilation.anti-aliasing` | 1 (off), 2, 3, 4, 8 or 16 | 1 |
| `open-annihilation.frame-stats` | 0 or 1 | 0 |
| `open-annihilation.screen-size` | `desktop`, or a size `WIDTHxHEIGHT` from `640x480`, each side at most 8192, such as `2560x1440` | `desktop`; with the player's own file on a light machine, `800x600`, or `640x480` on a smaller desktop |
| `open-annihilation.window-place` | `window WIDTHxHEIGHT at X,Y on DX,DY,DW,DH` for a window, or `full-screen on DX,DY,DW,DH` for full screen, with ` window WIDTHxHEIGHT at X,Y` after the display when a window was left too. Sizes are 1 to 16384 and coordinates -100000 to 100000. Any other text is ignored | the platform's first start: full screen on Windows, a window elsewhere. Restore defaults does not erase it |
| `open-annihilation.hardware-acceleration` | `off`, `basic` or `full`; a whole number from an earlier version reads as `off` at 0 or below, else `full` | `full` with the player's own file, else `off` |
| `open-annihilation.vertical-sync` | 0 or 1 | 0 |
| `open-annihilation.smooth-motion` | 0 or 1 | 1 |
| `open-annihilation.menu-scaling` | `sharp`, `whole-steps` or `unfiltered` | `sharp` |
| `open-annihilation.native-density` | 0 or 1 | 0; 1 where the platform opens every window at native density |
| `open-annihilation.explosion-flash` | `off`, `reduced` or `full` | `full` |
| `open-annihilation.fog-of-war` | `classic` or `smooth`; any other text reads as `smooth` | `smooth` |
| `open-annihilation.new-unit-shadows` | 0 or 1; a number above 0 reads as 1, any other number as 0. With 0, where the graphics card draws the battlefield, each unit and wreck casts the game's own shadow with its draw | 1 |
| `open-annihilation.zoomed-out-units` | `rendered` or `dots`; any other text, `icons` among them, reads as `rendered` | `rendered` |
| `open-annihilation.zoomed-out-after` | `1/2`, `1/3`, `1/4`, `1/6`, `1/8`, `1/12` or `1/16`; any other text reads as `1/6` | `1/6` |
| `open-annihilation.allow-full-3d` | 0 or 1 | 1 |
| `open-annihilation.unlocked-camera` | 0 or 1 | 0 |
| `open-annihilation.weapon-lighting.intensity`, `open-annihilation.nanolathe-lighting.intensity`, `open-annihilation.wreck-glow.intensity`, `open-annihilation.heat-distortion.intensity` | percent, 0 to 200, held to that range and rounded to the nearest 10; any other text is ignored. Without it, the effect's earlier switch (`open-annihilation.weapon-lighting`, `.nanolathe-lighting`, `.wreck-glow` or `.heat-distortion`, 0 or 1) reads as 0 or 100 | with `open-annihilation.hardware-acceleration` at `full`, 20 for weapon lighting and nanolathe lighting, 30 for wreck glow and 40 for heat distortion; else 0 |
| `open-annihilation.water-ripples.intensity`, `open-annihilation.coastal-waves.intensity`, `open-annihilation.underwater-shimmer.intensity`, `open-annihilation.naval-reflections.intensity` | percent, 0 to 200, held to that range and rounded to the nearest 10; any other text is ignored | 60 for water ripples, 30 for coastal waves, and 100 for underwater shimmer and naval reflections |
| `open-annihilation.window-frame` | `hidden-in-play` or `always-shown`; any other text reads as `hidden-in-play` | `hidden-in-play` |
| `open-annihilation.hud-scaling` | 0 or 1 | 1 |
| `open-annihilation.interface-size` | `automatic`, or the percent `100`, `150`, `200`, `300` or `400`; `150` read while `open-annihilation.modern-fonts` reads 0 reads as `100`; any other text, `0` among them, reads as `automatic` | `automatic` |
| `open-annihilation.ui.window-size` | `auto`, `compact`, `regular` or `large`: the size class Settings, the Library, the key editor and OA's other windows take, in the menus and in a match. Auto takes the largest whose dialog fits the window with 24 points round it; a chosen class is used wherever its dialog fits whole, and the largest that fits elsewhere. Absent, or any other text, is `auto`; Settings erases the key for Auto | `auto` |
| `open-annihilation.window.<window>.<class>` | where the player moved a window, as `x,y`, the offset in points from its centred place; `<window>` is `settings`, `key-editor`, `library`, `map-browser`, `mutator-picker` or `brain-picker`, and `<class>` `compact`, `regular` or `large`. `0,0` is never written: a window put back in the middle erases its key, and Reset window positions erases them all | absent: centred |
| `open-annihilation.overlay.<overlay>.<class>` | where the player put an overlay with Arrange overlays (Settings › Accessibility › Overlay places): each axis measured from the near edge, the far edge or the middle of the battlefield, on an 8-point grid, as `l16,b8` (16 points from the left, 8 from the bottom); `<overlay>` is `keys-overlay`, `chat-line` or `latch-chips`, `<class>` `compact`, `regular` or `large`. Read only while `open-annihilation.access.move-overlays` is 1, and kept while it is 0. Text that does not read is ignored. Reset in Arrange overlays erases them; Reset window positions leaves them | absent: where the overlay lays itself out |
| `open-annihilation.modern-fonts` | 0 or 1 | 1 with the player's own file, else 0 |
| `open-annihilation.ui.look` | `modern` or `classic`: the letters of Open Annihilation's own screens and overlays, Barlow or the game's own fonts; any other text reads as the default | `modern` with the player's own file, a file written before the key existed among them; else `classic` |
| `open-annihilation.text-outline` | 0 or 1 | 1 |
| `open-annihilation.text-shadow` | 0 or 1 | 1 |
| `open-annihilation.text-background` | 0 or 1 | 0 |
| `open-annihilation.text-size` | percent of the game fonts' sizes, 50 to 300 | 80 |
| `open-annihilation.language` | `system`, or a language's BCP-47 tag (`en`, `de`, `es`, `fr`, `it`) | `system` with the player's own file, else `en` |
| `open-annihilation.language-notice-dismissed` | comma-separated BCP-47 tags of languages the player chose Use English for, when the game asked about a language the operating system wanted that was not installed | none dismissed; each such language is still asked about |
| `open-annihilation.first-run` | `done` when the welcome setup is finished or skipped; absent, or any other word, means it has not been finished. Show again erases the key | absent, so the welcome shows once |
| `open-annihilation.content-updates` | `automatically`, `library` (only when the Library opens) or `never`; any other text reads as `automatically` | `automatically` |
| `open-annihilation.update-check` | `automatically`, `when-asked` (only when asked) or `never`; any other text reads as `automatically` | `automatically` |
| `open-annihilation.update-checked` | seconds since 1970 of the last check for a new release that reached the server; absent, or any text that is not a whole number, means that has not happened | absent |
| `open-annihilation.update-skipped` | the highest release number the player skipped; absent, or any text that is not a whole number, means none | absent |
| `open-annihilation.update-download` | 0 or 1 | absent reads as 0: a found update is not fetched until UPDATE |
| `open-annihilation.update-install-on-quit` | the release number that installs the next time the game quits; absent, or any text that is not a whole number, schedules nothing | absent |
| `open-annihilation.update-held` | the release number of a release that did not install; it and older ones are not offered, downloaded or installed by themselves until the player tries again; absent holds nothing | absent |
| `open-annihilation.update-install` | `ask` or `when-i-quit`: how a ready release is installed; any other text reads as `ask` | `ask` |
| `open-annihilation.file-types.oamod` | `on` or `off`. Only the word `off` turns the switch off; any other text reads as on. Not `0` and `1` | on: this copy opens mod packages |
| `open-annihilation.file-types.oamap` | `on` or `off`, as the mod packages' key | on: this copy opens map packs |
| `open-annihilation.file-types.oalang` | `on` or `off`, as the mod packages' key | on: this copy opens language packs |
| `open-annihilation.file-types.oareg` | `on` or `off`, as the mod packages' key | on: this copy opens registry files |
| `open-annihilation.file-types.oamut` | `on` or `off`, as the mod packages' key | on: this copy opens mutator packages |
| `open-annihilation.file-types.oabrain` | `on` or `off`, as the mod packages' key | on: this copy opens brain packages |
| `open-annihilation.file-types.oakeys` | `on` or `off`, as the mod packages' key | on: this copy opens key layout packages |
| `open-annihilation.file-types.oalaunch` | `on` or `off`, as the mod packages' key | on: this copy opens launch files |
| `open-annihilation.base-game` | `totala` or `totala-demo`; any other text reads as `totala` | `totala`, with every copied expansion of it that can be on |
| `open-annihilation.expansions.<game>` | comma-separated expansion ids, in the built-in order; empty means none on | every copied expansion of that game that can be on |
| `open-annihilation.content-note` | `required-expansion` when a mod was cleared because an expansion it needs went off | no note waiting |
| `open-annihilation.access.slow-keys-ms` | milliseconds a key is held before it counts, 0 to 1000. 0 waits nothing. Absent, or any text that is not a whole number, reads as 0 | 0 |
| `open-annihilation.access.bounce-keys-ms` | milliseconds during which a second press of the same key is ignored, 0 to 1000. 0 ignores nothing. Absent, or any text that is not a whole number, reads as 0 | 0 |
| `open-annihilation.access.double-press-ms` | milliseconds two presses may be apart and still be a double press. 0 uses the system's double-click time; any other number is held to 200 to 1000. Absent, or any text that is not a whole number, reads as 0 | 0, the system's |
| `open-annihilation.access.long-press-ms` | milliseconds a hold waits, 200 to 1000. Absent, or any text that is not a whole number, reads as 500 | 500 |
| `open-annihilation.access.repeat-delay-ms` | milliseconds before a repeating key's first repeat. 0 uses the system's repeat; any other number is held to 150 to 1000. Absent, or any text that is not a whole number, reads as 0 | 0, the system's |
| `open-annihilation.access.repeat-rate` | repeats a second after that delay. 0 uses the system's repeat; any other number is held to 2 to 30. Absent, or any text that is not a whole number, reads as 0 | 0, the system's |
| `open-annihilation.access.drag-threshold` | pixels of the 640×480 game a drag may travel and still be a click, 32 to 96. The tick limit widens in proportion. Absent, or any text that is not a whole number, reads as 32 | 32 |
| `open-annihilation.keys.layout` | an installed layout's id; `oa-default` is the game's own layout. Any other text is an id. A layout that is not installed is left unused for that run and `oa-default` is played instead | `oa-default` |
| `open-annihilation.keys.layout.<mod id>` | `same`, or an installed layout's id for that mod. `same` uses `open-annihilation.keys.layout` | `same` |
| `open-annihilation.keys.show-on-buttons` | 0 or 1. Settings › Controls reads and writes it. Absent reads as on | 1 |
| `open-annihilation.keys.imported` | comma-separated ids of the mods whose stored keys were carried into the player's own changes. A mod is listed once, the first time it is played with a build tool, click snap, a build preview or the options dialog, and not under `--keys` or `--keys-reset` | none imported |
| `open-annihilation.keys.keyboard` | `system`, `ansi`, `iso` or `jis`. Absent reads as `system`: the keyboard picture follows the system, or ANSI where the system does not say | `system` |
| `mutators.clash-default` | `add` or `replace`; any other text reads as `replace`. The clash mode a mutator uses when the player and its manifest leave it unset. `Runtime::apply_match_mutators` reads it, and so does the battle room on the host, which sends each mode with its list; a guest plays the host's modes and never reads its own | `replace` |

The eight file-type keys are words, `on` and `off`, not the `0` and `1` of
the other switches. Settings › System shows a switch for each on Windows,
Linux and macOS, and OK registers, withdraws or changes the default opener
of a type whose switch moved, that type alone. A start registers the types
whose key is not `off` and withdraws the rest on Windows and Linux. On macOS the switch shows whether this copy
is the default opener now, and a start does not replace a choice made in
Finder. On iOS the page shows a text row instead of the switches, because
there is no way to choose a default opener.

Settings no longer shows Select groups without Alt. The key editor's
Number keys select groups writes the same `Total Annihilation|SwitchAlt`,
which `+switchalt` also sets. `+stats`
without an argument shows or hides the performance statistics and writes
`open-annihilation.frame-stats`; `+stats 1` and `+stats 0` write nothing.
`--max-fps` sets the frame rate for its run and writes nothing, and so do
`--hardware-acceleration[=off|basic|full]` and `--no-hardware-acceleration`
for Hardware acceleration. With
`--preferences-file` every default is the game's own behaviour on every
platform and `totala.ini` is not read.

Platform references:

- [Apple Application Support directory](https://developer.apple.com/documentation/foundation/url/applicationsupportdirectory)
- [Windows Known Folder API](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath)
- [Windows SHGetFolderPathW](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetfolderpathw)
- [XDG Base Directory Specification](https://specifications.freedesktop.org/basedir/0.8/)
- [xdg-user-dirs](https://www.freedesktop.org/wiki/Software/xdg-user-dirs/)
