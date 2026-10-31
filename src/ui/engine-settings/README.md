# Open Annihilation settings

The settings Open Annihilation adds to the game, and the dialog that shows
them. The application opens the dialog from the OA button on the main menu
and in the in-game menu's column, and with Cmd+, on macOS (also the
application menu's Settings… item) or Ctrl+, elsewhere.

## The settings

`engine_settings.hpp` holds the settings' values (`EngineSettings`), their
defaults (`default_settings`), the preferences keys they are stored under
(`key::`), reading and writing them (`read_settings`, `write_settings`) and
the locks a running game puts on them (`settings_locks`).

| Section | Setting | Range | Default | Key |
|---|---|---|---|---|
| (the Library) | Mod | No Mod, or one of the mod folders the game finds | No Mod | `open-annihilation.mod-directory` (the folder's path; absent for No Mod) |
| | A mod folder chosen outside the folders the game finds, kept while it is the Mod | any folder | none | `open-annihilation.picked-mod-directory` (the folder's path; absent for none, erased once another mod or No Mod is stored) |
| (the Library) | Base game | Total Annihilation, or the demo | Total Annihilation, with every copied expansion of it that can be on, when the key is absent | `open-annihilation.base-game` (`totala` or `totala-demo`) |
| | Expansions of that base game | on or off for each expansion that is copied | every copied expansion that can be on, when the key is absent | `open-annihilation.expansions.<game>` (comma-separated ids; empty means none on) |
| Controls | Key layout, a card at the section's top ([The key layout card](#the-key-layout-card)): the layout in use, how many of its keys you changed, where it applies, with Change layout… (the Library on Keys, on the main menu) and Edit keys…, which opens the key editor over Settings | the layout's name, whole; a badge "N changes of your own" ("1 change of your own"); "Applies to every game." or "Applies with {mod} only." | OA default, the layout in use | the layout in use is `open-annihilation.keys.layout`, or `open-annihilation.keys.layout.<mod id>` while that mod chooses one |
| | Key layout with <mod>, a drop-down, only while a mod is played. Same as the game uses Key layout. Installed layouts follow, those made for the mod first and marked made for it | Same as the game, or a layout id | Same as the game | `open-annihilation.keys.layout.<mod id>` (`same`, or a layout id). A choice is written at once, and Cancel writes the choice the dialog opened with |
| | Show keys on buttons: order buttons and build cells show their key in a corner | Off, On | Off | `open-annihilation.keys.show-on-buttons` (`0` or `1`; absent reads as off) |
| | Mouse wheel zoom | Off, On | On | `open-annihilation.wheel-zoom` |
| | Maximum zoom out, a drop-down: how far out the wheel, a pinch, the touch zoom buttons and a controller zoom | Automatic (half the game's scale, a sixth while Full draws the battlefield), Whole map, 1/32, 1/16, 1/8, 1/4, 1/2; no choice but Automatic goes past the whole map | Automatic | `open-annihilation.max-zoom-out` (`automatic`, `whole-map`, `1/32`, `1/16`, `1/8`, `1/4` or `1/2`) |
| | Maximum zoom in, a drop-down: how close the same zooms go | None (the game's own scale), 2x, 3x, 4x | 4x | `open-annihilation.max-zoom-in` (`1`, `2`, `3` or `4`) |
| | View past the map's edge, a strip: how much of the battlefield the view may show past the map's edges; at 25% and Off a zoom keeps to it too | Off (the view stays on the map, as 3.1c's does), 25%, 50% (the map's edge reaches the battlefield's middle) | 50% | `open-annihilation.view-past-map-edge` (`off`, `25` or `50`) |
| | Middle-click pans: a middle click on the battlefield scrolls the view towards the pointer until another press or a key | Off, On | On | `open-annihilation.middle-click-pan` |
| | Double-click selects, a drop-down: where a double-click on one of the player's units, a controller's Select type, the touch radial's Type and a double tap select the units of its type. On screen selects those the battlefield shows; On the map every one on the map, as 3.1c's double-click does. With ui.selection-shortcuts on, On screen is the hack's own double-click, the selected types on screen, and On the map is the game's. It applies at once | On screen, On the map | On screen | `open-annihilation.double-click-selects` (`on-screen` or `on-map`) |
| | Number keys select groups is the key editor's switch, not a row of this section | Off, On | Off | 3.1c's SwitchAlt |
| | Window edge, a strip: what a window does with the pointer while a match's battlefield shows. Capture keeps it in the window until the game menu, the pause screen, Settings, Escape or a lost focus, and a click on the battlefield captures it again. Sticky holds it on the edges so the view edge-scrolls, until a firm push or a flick lets it leave. Free leaves it free. Full screen holds the pointer either way | Capture, Sticky, Free | Capture | `open-annihilation.window-edge` (`capture`, `sticky` or `free`) |
| | Edge stickiness, a slider: how firmly Sticky holds, the push and the flick growing with it; 0.0 never holds. Locked Needs Sticky (`Lock::needs_sticky`, which the dialog sets itself in `shown_locks`) while Window edge shows Capture or Free, when it keeps its value | 0.0 to 5.0 in steps of 0.1 | 1.0 | `open-annihilation.window-edge-stickiness` (a decimal, `1.0`) |
| | Group keys centre the view, a drop-down: whether a group's key, which selects the group, also moves the view to the group's centre. On a double press moves it on the second of two presses of the same group's key within the double-press time with no other key between; Never never moves it; On every press moves it when the group is already exactly the selection, however long after. It applies at once | On a double press, Never, On every press | On a double press | `open-annihilation.group-keys-centre` (`double-press`, `never` or `every-press`) |
| Engine Limits | Unit limit | 50 to 1500 per player, steps of 50, or on to a mod's higher maximum | the installation's `totala.ini` UnitLimit, else 250 or a mod's default | `open-annihilation.unit-limit` |
| | Pathfinding cycles | 1× to 8× of 1333 path nodes a tick, or of a mod's budget | 1× | `open-annihilation.path-search-nodes` |
| | Skirmish players: how many player rows the skirmish setup shows. The dialog's working copy of the setup's own count; OK stores it only when the slider moved, and the setup's key codes write the same count | 2 to 10; a stored count outside that shows as it is until the slider moves | 10 when none is stored | `NumSkirmishPlayers` |
| | Set by the mod: the heading over Weapon types and Sides. It keeps no setting | | | |
| | Weapon types: how many weapon types the mod allows. In a match it shows how many of that limit the match loaded, as "312 of 4096". The same for every player. It takes no input | the mod's limit; the base game allows 256 | the mod's limit | |
| | Sides: how many sides the mod allows. In a match it shows how many of that limit the match loaded, as "7 of 10". The same for every player. It takes no input | the mod's limit; the base game has 5 | the mod's limit | |
| Language & Text | Language | System default, English, Deutsch, Español, Français, Italiano, and any installed pack | System default with the player's own preferences file; English with `--preferences-file` | `open-annihilation.language` (`system` or a language's tag, as `de`) |
| | Use modern fonts for game text; On, locked, while a language drawn only in them is chosen | Off, On | On with the player's own preferences file; Off with `--preferences-file` | `open-annihilation.modern-fonts` |
| | Text size, of the modern fonts | 50% to 300% of the game fonts' sizes, steps of 10% | 80% | `open-annihilation.text-size` |
| | Font outline | Off, On | On | `open-annihilation.text-outline` |
| | Font shadow | Off, On | On | `open-annihilation.text-shadow` |
| | Game text background | Off, On | Off | `open-annihilation.text-background` |
| | Enable Unicode Multiplayer Chat; On, locked, while a language whose pack asks for it is chosen | Off, On | Off | `open-annihilation.unicode-chat` |
| Graphics | Maximum frame rate | 30 to 120, steps of 5 | 120; 60 on a Raspberry Pi or a light machine with the player's own preferences file; a Steam Deck's screen rate (60 on the LCD model, 90 on the OLED) on a Deck with the player's own preferences file | `open-annihilation.max-fps` |
| | Enhanced anti-aliasing | Off, 2×, 4×, 8×, 16×; a stored level between reads as the one below it, a stored 3 as 2× | Off, a Raspberry Pi and a light machine included | `open-annihilation.anti-aliasing` |
| | Screen size, when OK is pressed | Desktop, then the sizes the display offers from 640×480 (`Dialog::offered_screen_sizes`), in a window shown at the window's own size until moved, as Custom where the display offers no such size; Desktop, 640×480, 800×600, 1024×768 and 1280×1024 where the game does not say | Desktop; 800×600 on a light machine with the player's own preferences file, 640×480 when its desktop is smaller | `open-annihilation.screen-size` (`desktop`, or a size such as `2560x1440`) |
| | Hardware acceleration | Off, Basic, Full | Full with the player's own preferences file on every machine; Off with `--preferences-file` | `open-annihilation.hardware-acceleration` (`off`, `basic` or `full`) |
| | Vertical sync | Off, On | Off | `open-annihilation.vertical-sync` |
| | Smooth motion: whether the frames between two ticks show units, their pieces, projectiles, debris and particles part of the way from one tick to the next, or each frame the latest tick whole | Off, On | On | `open-annihilation.smooth-motion` |
| | Native pixel density, from the next start | Off, On; always On where the platform opens every window at native density | Off | `open-annihilation.native-density` |
| | Explosion flash; a mod's profile may draw the flashes lower still (`ui.explosion-flash`), and the lower of the two is drawn | Off, Reduced, Full | Full | `open-annihilation.explosion-flash` (`off`, `reduced` or `full`) |
| | Fog of War: how the graphics card draws the fog's edges (Hardware acceleration Full); the processor always draws them Classic | Classic, Smooth | Smooth | `open-annihilation.fog-of-war` (`classic` or `smooth`; any other word reads as Smooth) |
| | Zoomed out units: how units are drawn farther out than After zoom | Rendered, Dots; Icons shown faded, not offered | Rendered | `open-annihilation.zoomed-out-units` (`rendered` or `dots`; any other word, `icons` among them, reads as Rendered) |
| | After zoom, a drop-down, locked "Needs Dots" while Zoomed out units is Rendered | 1/2, 1/3, 1/4, 1/6, 1/8, 1/12, 1/16 of normal size | 1/6 | `open-annihilation.zoomed-out-after` (`1/2`, `1/3`, `1/4`, `1/6`, `1/8`, `1/12` or `1/16`) |
| | Allow full 3D mode: whether the camera may turn away from the classic view, with Full hardware acceleration | Off, On | On | `open-annihilation.allow-full-3d` |
| | Unlocked camera, locked "Needs Allow full 3D mode" while Allow full 3D mode is Off | Off, On | Off | `open-annihilation.unlocked-camera` |
| | Coloured weapon lighting, in the classic and the rotated view | 0–200%, steps of 10% | 20% with Hardware acceleration Full, else 0%; leaving Full sets 0%, reaching it 20% | `open-annihilation.weapon-lighting.intensity`; without it, an earlier `open-annihilation.weapon-lighting` switch reads as 0% or 100% |
| | Nanolathe glow, in the classic and the rotated view | 0–200%, steps of 10% | 20% with Hardware acceleration Full, else 0%; leaving Full sets 0%, reaching it 20% | `open-annihilation.nanolathe-lighting.intensity`; without it, an earlier `open-annihilation.nanolathe-lighting` switch reads as 0% or 100% |
| | Wreck glow, in the classic and the rotated view | 0–200%, steps of 10% | 30% with Hardware acceleration Full, else 0%; leaving Full sets 0%, reaching it 30% | `open-annihilation.wreck-glow.intensity`; without it, an earlier `open-annihilation.wreck-glow` switch reads as 0% or 100% |
| | Heat distortion, in the classic and the rotated view | 0–200%, steps of 10% | 40% with Hardware acceleration Full, else 0%; leaving Full sets 0%, reaching it 40% | `open-annihilation.heat-distortion.intensity`; without it, an earlier `open-annihilation.heat-distortion` switch reads as 0% or 100% |
| | Water ripples, locked "Needs Full acceleration" while Hardware acceleration is not Full | 0–200%, steps of 10% | 60% | `open-annihilation.water-ripples.intensity` |
| | Coastal waves, locked as Water ripples is | 0–200%, steps of 10% | 30% | `open-annihilation.coastal-waves.intensity` |
| | Underwater shimmer, locked as Water ripples is | 0–200%, steps of 10% | 100% | `open-annihilation.underwater-shimmer.intensity` |
| | Naval reflections, locked as Water ripples is | 0–200%, steps of 10% | 100% | `open-annihilation.naval-reflections.intensity` |
| | New unit shadows, Graphics' last row: the shadows of units and wrecks Open Annihilation draws where the graphics card draws the battlefield (Hardware acceleration Full), or the game's own; the processor always draws the game's own | Off, On | On | `open-annihilation.new-unit-shadows` (`0` or `1`) |
| User Interface | Menu scaling | Sharp, Whole steps, Unfiltered | Sharp | `open-annihilation.menu-scaling` (`sharp`, `whole-steps` or `unfiltered`) |
| | Window frame: whether a window shows its title bar and borders while a game is played; the game menu, a panel it opens and every other screen show them | Hidden in play, Always shown | Hidden in play | `open-annihilation.window-frame` (`hidden-in-play` or `always-shown`) |
| | HUD scaling: whether a game's side column and bars grow with the window, up to twice the original game's size, or keep its size | Off, On | On | `open-annihilation.hud-scaling` |
| | Interface size, a drop-down, when OK is pressed: how large Open Annihilation's own screens (Settings, its notices and its prompts) are drawn, held to the largest step at which Compact's dialog fits the window, and in a match Settings no larger than the side column's scale ([Sizes](#sizes)) | Auto (the window's own scale), 100%, 150% (offered only while the dialog shows Use modern fonts for game text On), 200%, 300%, 400% | Auto | `open-annihilation.interface-size` (`automatic`, `100`, `150`, `200`, `300` or `400`; `150` read with the modern fonts Off reads as 100%; any other text, `0` among them, reads as Auto) |
| | Window size, a drop-down, when OK is pressed: the size class Settings, the Library, the key editor and the pickers take. Auto takes the largest class whose dialog fits the window with 24 points round it; a chosen class shows wherever its dialog fits whole, and is held to the largest that does elsewhere. The OA layer reads the same key (`src/app/window_placement.hpp`) | Auto, Compact, Regular, Large | Auto | `open-annihilation.ui.window-size` (`auto`, `compact`, `regular` or `large`; absent or any other text is Auto) |
| | Reset window positions: Reset puts every window the player moved back in the middle, at once (`Dialog::window_reset_asked`); the row then says so (`Dialog::window_places_reset`). Cancel does not move them again | | | none. The host forgets every `open-annihilation.window.*` key |
| | Escape opens the game menu | Off, On | On on macOS, Off elsewhere and with `--preferences-file` | `open-annihilation.escape-opens-menu` |
| | Look, the section's last row, as it is chosen, the open dialog among them: the letters of Open Annihilation's own screens and overlays (Settings, the Library, the key editor, the pickers, the notices and prompts, the overlays in a match). Modern draws them in Barlow, tracked titles and headings in Barlow Condensed, at the kit's type scale, with DejaVu Sans, the endonym face and Noto Emoji drawing what Barlow lacks; Classic in the game's own two fonts. The 3.1c game's own screens keep the game's fonts either way. Cancel puts back the look the dialog opened with | Modern, Classic | Modern with the player's own file, one written before the setting existed among them; Classic with `--preferences-file` | `open-annihilation.ui.look` (`modern` or `classic`; absent or any other text is the default) |
| Accessibility, listed in every dialog of the engine's settings, after Controls and before Touch, with its practice pad ([The practice pad](#the-practice-pad)) | Hold keys that latch: how many latchable actions the layout in use has turned on, of how many the game offers, and Choose…, which opens the key editor on Latchable. Edit keys… on the key layout card still opens on All | the count, as "2 of 7", and Choose… | | none. The count is the host's (`Dialog::latched_count`) |
| | Sticky modifiers: a tapped Shift, Ctrl or Alt holds for the next key or click; a second tap locks it | Off, On | Off | `open-annihilation.access.sticky-modifiers` |
| | Hold to confirm: self-destruct, surrender and quitting need a long press, and a ring fills while it is held | Off, On | Off | `open-annihilation.access.hold-to-confirm` |
| | Long press: how long a hold is, for Hold to confirm and for keys bound to a hold | 200 to 1000 ms, steps of 50 | 500 ms | `open-annihilation.access.long-press-ms` |
| | Slow keys: a key counts only after it is held this long | Off, then 50 to 1000 ms, steps of 50 | Off | `open-annihilation.access.slow-keys-ms` |
| | Bounce keys: a second press of the same key this soon is ignored | Off, then 50 to 1000 ms, steps of 50 | Off | `open-annihilation.access.bounce-keys-ms` |
| | Order wheel: holding that mouse button opens the order wheel at the pointer | Off, Hold right, Hold middle | Off | `open-annihilation.access.order-wheel` (`off`, `hold-right` or `hold-middle`) |
| | Pointer keys: the numpad moves the pointer; 5 clicks, 0 right-clicks | Off, On | Off | `open-annihilation.access.pointer-keys` |
| | Double press: how close two presses, or two clicks, are and still count as one double | System (the system's double-click time), then 200 to 1000 ms, steps of 50 | System | `open-annihilation.access.double-press-ms` |
| | Key repeat delay: how soon a held camera, speed or page key repeats | System (the system's repeat), then 150 to 1000 ms, steps of 50 | System | `open-annihilation.access.repeat-delay-ms` |
| | Key repeat rate: how fast that key then repeats | System (the system's repeat), then 2 to 30 a second | System | `open-annihilation.access.repeat-rate` |
| | Drag threshold: how far a press may move and still be a click | 32 to 96 px, steps of 8 | 32 px | `open-annihilation.access.drag-threshold` |
| | Edge scrolling: Off stops the view scrolling while the pointer rests at the window's edge. Arrow keys, and the touch edge band while a drag is held, keep moving the view | Off, On | On | `open-annihilation.access.edge-scroll` |
| | Move overlays: On, the keys overlay, the chat line and the latch chips show where the player put them; Off, where they lay themselves out, and the places stay kept | Off, On | Off | `open-annihilation.access.move-overlays` |
| | Overlay places: "Your places" or "The game's places", and ARRANGE… (`DialogAction::arrange_overlays`), pressable while Move overlays is on: the host opens a picture of the match over the dialog where the overlays are placed | | | none. The places are the host's `open-annihilation.overlay.*` keys |
| Touch, listed only while the game has touch controls | One-finger drag | Automatic (a selection box on a tablet, scrolling on a phone), Box, Scroll | Automatic | `open-annihilation.touch-drag` (`automatic`, `box` or `scroll`) |
| | Hold delay | 250 to 700 ms, steps of 50; a stored delay is held to the range and put on its nearest step | 350 ms | `open-annihilation.touch-hold-delay` |
| | QUEUE and ADD | Stay on, One action | Stay on | `open-annihilation.touch-latches` (`stay-on` or `one-action`) |
| | Haptics | Off, On | On | `open-annihilation.touch-haptics` |
| | Left-handed layout | Off, On | Off | `open-annihilation.touch-left-handed` |
| | Control size, also in Controller | Standard, Large, Larger (the touch layer's points 1, 1.25 or 1.5 times) | Standard; Larger on a Steam Deck with the player's own preferences file | `open-annihilation.touch-control-size` (`standard`, `large` or `larger`) |
| Controller, listed only once a gamepad has sent input in the run | Scheme | Trackpads, Sticks | Trackpads | `open-annihilation.pad-scheme` (`trackpads` or `sticks`) |
| | Right trackpad | Relative, Absolute (the pointer's two ways) | Relative | `open-annihilation.pad-right-trackpad` (`relative` or `absolute`) |
| | Pointer speed | 50% to 300%, steps of 10% | 100% | `open-annihilation.pad-pointer-speed` |
| | Pointer acceleration | Off, Low, High | Low | `open-annihilation.pad-acceleration` (`off`, `low` or `high`) |
| | Trackpad glide | Off, On | Off | `open-annihilation.pad-glide` |
| | Right stick | Zoom (zoom and build pages), Pointer, Nothing | Zoom | `open-annihilation.pad-right-stick` (`zoom`, `pointer` or `nothing`) |
| | Magnetism (stick pointer) | Off, On | On | `open-annihilation.pad-magnetism` |
| | Gyro pointer | Off, While the right pad is touched, While the right stick is touched, Always | Off | `open-annihilation.pad-gyro` (`off`, `right-pad`, `right-stick` or `always`) |
| | Gyro speed | 50% to 400%, steps of 10% | 100% | `open-annihilation.pad-gyro-speed` |
| | Haptics | Off, Light, Strong | Light | `open-annihilation.pad-haptics` (`off`, `light` or `strong`) |
| | Button prompts | Automatic, Steam Deck, Xbox, PlayStation, Nintendo, Off | Automatic | `open-annihilation.pad-prompts` (`automatic`, `steam-deck`, `xbox`, `playstation`, `nintendo` or `off`) |
| | Left-handed | Off, On | Off | `open-annihilation.pad-left-handed` |
| | Control size, Hold delay, QUEUE and ADD | Touch's own rows, shared | | as in Touch |
| Game files, listed only in the main menu's dialog where the platform brings game files in | Include in device backups | Off, On | Off | `open-annihilation.game-files-backed-up` |
| Downloads, listed in every dialog of the engine's settings, before Updates | Check for content updates | Automatically, Only when I open the Library, Never | Automatically | `open-annihilation.content-updates` (`automatically`, `library` or `never`); takes effect at OK, and Cancel and Restore defaults apply |
| Updates, listed in every dialog of the engine's settings, after Downloads and before Game Content | The update card, the update centre's state as the host fills it (`UpdatesOffer::card_status` and the rest): "Installed {version}", the state, its detail, a bar while a download moves (amber while paused), up to three of the state's actions as buttons (`UpdateCardAction`, `DialogAction::update_action` with `Dialog::update_act`) and a note. With no card the row is the older status line with Check now, Update…, Restart to update or Retry | the buttons act at once and the dialog stays open | | none. The lines come from the host's offer (`Dialog::updates`) |
| | Download automatically: a release that can be installed here starts downloading at the settled main menu | Off ("Off: OA asks before downloading."), On | Off | `open-annihilation.update-download` (`0` or `1`); takes effect at OK |
| | Check for updates | Automatically, Only when I ask, Never | Automatically | `open-annihilation.update-check` (`automatically`, `when-asked` or `never`); takes effect at OK, and Cancel and Restore defaults apply. Check now makes no request while the choice is Never |
| | Install updates: how a ready release is installed | Ask each time, When I quit | Ask each time | `open-annihilation.update-install` (`ask` or `when-i-quit`; any other text reads as `ask`); takes effect at OK, and Cancel and Restore defaults apply |
| | Previous version · {version}, while a version is kept: Roll back… asks the roll back's question (`updates::roll_back_question`) | | | none. Locked during a game |
| System, listed in every dialog of the engine's settings, after Game Content and before Developer | Your files: the player's own folder and buttons that open its Saves, Screenshots and Mods folders | changes no setting | | `open-annihilation.user-folder`, read at start (`src/app/include/oa/app/user_folder.hpp`) |
| | File Type Registration, then one switch for each of the eight types, the launch file's last, on Windows, Linux and macOS | Off, On | On | `open-annihilation.file-types.oamod`, `.oamap`, `.oalang`, `.oareg`, `.oamut`, `.oabrain`, `.oakeys`, `.oalaunch` (`on` or `off`; only `off` turns one off). On Windows and Linux OK registers a type turned on and withdraws one turned off, at once; on macOS it changes the default opener of a switch that moved. Each switch acts on its own type alone (`engine_settings/file_types.hpp`) |
| | File types, the text row on iOS in place of the switches | changes no setting | | none |
| Developer | Enable Developer Mode | Off, On | Off | `open-annihilation.developer-mode` |
| | Show performance statistics | Off, On | Off | `open-annihilation.frame-stats` |
| | UI kit gallery: SHOW opens the gallery of every part of OA's own screens (`DialogAction::show_gallery`) | | | none |
| | The overrides of the profile's standard hacks, in Developer Mode's list | any hack on or off, with its parameters | none | `open-annihilation.hack-overrides.<id>`, the id of the profile the game plays, `ta-3.1c` without a mod, `folder:<path>` for a mod folder without a profile |

Mouse wheel zoom, while on, also leaves a profile's megamap off for the
player: the wheel zooms the battlefield in its place, out to the whole map
with Maximum zoom out at Whole map
([ui.megamap](../../../docs/mods/standard-hacks/ui.megamap.md)). Turning it
on during a match closes an open megamap.

Maximum zoom out and Maximum zoom in hold every zoom of the player's view,
with Mouse wheel zoom off too, since a pinch and the touch zoom buttons
zoom then; a change brings a view past the new limits within them at once.
Neither changes the match: they are never locked.

The Touch section is the same everywhere, every machine and preferences
file alike. A stored One-finger drag or QUEUE and ADD that is none of their
words, like any value that is no whole number for the others, reads as the
default. The touch controls read the settings in effect at every finger and
every frame, so a change in the dialog takes effect at once. One-finger
drag says what a finger dragged on the battlefield does; a hold followed by
a drag draws a selection box whatever it says, and two fingers scroll. The
hold delay is how long a finger stays down before it counts as a hold, which
opens the order menu, gives a build button's right press or shows a
control's help. QUEUE and ADD says whether a tapped QUEUE, ADD or x5 stays on
until it is tapped again or turns off after the next order or selection.

The Controller section is the same everywhere too. A stored choice that is
none of its words reads as the default; Pointer speed and Gyro speed are
held to their ranges and put on their nearest steps, half a step up. The
gamepad reads the settings in effect every frame, so a change takes effect
at once. While the gamepad reaches the game through Steam Input, the
section's first row says so in amber: "Steam Input is on: the trackpads and
back grips reach the game as Steam's mouse and keys. Turn Steam Input off
for Open Annihilation in Steam's controller settings to use them here."

A Steam Deck (`oa/platform/machine.hpp`, `running_steam_deck_model`: Linux
names the maker Valve and the product Jupiter, the LCD model, or Galileo,
the OLED, in `/sys/class/dmi/id`) starts, with the player's own preferences
file, at its screen's rate, 60 or 90 frames a second, with Control size
Larger, so that the touch controls come close to a tablet's in size; nothing
else changes there. Maximum frame rate's hint has a second line on a Deck,
"Steam Deck: starts at the screen's 90 fps." (`Dialog::steam_deck_panel_hz`,
`Inputs::steam_deck_panel_hz`). A stored value always wins, and Restore
defaults puts the Deck's defaults back.

A mod's limits (`oa::data::limits`, handed in through `Inputs::units_per_player`
and `match_path_search_nodes`) change three things: the unit limit a player
starts with, the range a stored or installation limit is clamped to (the
mod's minimum and maximum; 20 to 500 for an installation's limit in 3.1c)
with the slider's highest stop, and the credit the Pathfinding cycles
multiply. Without a mod each is as above.

A light machine (`oa/platform/machine.hpp`, `light_machine`) has one
logical processor, a 32-bit x86 processor without SSE2 (a Pentium III or an
Athlon XP), or less than 512 MiB of physical memory; with the player's own
preferences file it starts at 800×600, 60 frames a second and no enhanced
anti-aliasing, which a machine of the game's own time keeps up with. The
screen size is read before the window opens (`src/app/screen_size.cpp`): a
size other than Desktop opens the window at that size, and full screen
shows it through the display's mode of that size, or draws the game at it
and scales it to the screen. A size chosen applies when OK is pressed
(`src/app/screen_mode.hpp`), never while the slider moves: the dialog holds
the setting as it was until then, and Cancel leaves it. The game sets the
slider's stops once the dialog has opened: Desktop, then the sizes the
display the window is on offers
([display modes](../../platform/display-modes/README.md)), and the size
shown where the display does not offer it, so that the slider shows it
until it is moved. In a window that is the window's own size
(`Dialog::window_screen_size`), which the slider shows until its knob
moves, whatever the setting, as "Custom" where it is not one of the
display's (`Dialog::custom_screen_size`); in full screen it is the size
stored. The preferences keep any size from 640×480 to 8192×8192
(`screen_size_from_text`), so that a size the display offers is kept as
chosen.

The game counts a machine as a Raspberry Pi when Linux names its board's
model, in `/proc/device-tree/model`, starting "Raspberry Pi"
(`oa/platform/machine.hpp`). Its graphics keep up with 60 frames a second at
the game's resolutions, so it starts there; the player can raise the rate and
turn on anti-aliasing in the dialog like anywhere else, and Restore defaults
puts the Pi's defaults back.

Enhanced anti-aliasing's levels keep one key and one strip in every tier,
but mean two things. Off and Basic draw units finer on the processor at
the level's factor, and the row's hint says so, warning of the processor
cost from 8x. While frames are drawn in Full the processor's anti-aliasing
never runs: the graphics card draws the whole battlefield finer and scales
it down, at the level's samples across, 2, 4, 8 or 16, within what the
renderer's texture limit and the memory allow at the window's size, and
the hint says where they allow fewer. The host tells the dialog the factor in use
(`AccelerationStatus::full_supersample`, 0 outside Full), and the hint
then says what the level does in Full at that factor, 1:1 at Off.

Hardware acceleration has three levels (`HardwareAcceleration`). Off is the
game as it always drew: the processor draws and scales every frame. Basic
lets the graphics card scale and compose the frames where it is able to;
the processor still draws every pixel the game decides. Full lets the
card also draw the battlefield. Its default depends only on the preferences
file, never on the machine: whether the card is used is decided apart, and
the card is used only with a renderer able to and 2 GiB of memory, which a
machine sold with 2 GB counts as having. Its two hint lines are its status
(`AccelerationStatus`), which the host gives the dialog when it opens and
again each frame: what runs, or why not, and what draws the view, what the
player can do, or what the card does on this machine. Under 2 GiB it says
the machine needs more memory, whatever the setting; otherwise, the
setting or a flag turned it off, the environment names a render driver, a
shared game or a replay waits for its end, naming the level that then
takes effect, the graphics driver failed or the game stopped while using
it, in this run or as the game's records of earlier ones say, no usable
graphics card was found, the card lacks a feature, the game cannot save the
files that guard trying it, the card is in use, on another driver where a
record passed over one, or,
on a renderer nothing has looked at yet, it takes effect from the next
start. Where Full was asked for and Basic draws in its place, the first
line says why: Full's trial could not be written, there is too little
memory for Full, Full stopped for this run, Full
failed before on this driver, the card lacks a feature Full needs, or a
shared game or a replay waits for its end; and Full in use says the card
draws the view, smoothed at every zoom, with its anti-aliasing where there
is any. A driver a record
passed over at this start shows whatever the setting. The key reads `off`, `basic` or `full`; a whole number, as the
setting's earlier On and Off switch wrote it, reads as Full above 0 and Off
otherwise, and any other text gives the default.

Vertical sync has each frame wait for the display, so that no frame tears;
while it is in effect the frame rate keeps just below the display's. Off,
the renderer is never asked, and the game paces its frames as without the
setting.

Smooth motion is never locked, and takes effect at once, in a game too.
On, the application draws each frame the fraction of the way between two
ticks its time stands for; Off, every frame shows the latest tick whole,
as the game drew it, so the frames between two ticks repeat the tick's
picture (`Runtime::smooth_motion`, `Runtime::present_frame_between_ticks`).
The frame rate, the camera and the interface are the same either way, and
it changes nothing a match shares, saves or records.

New unit shadows is never locked, and takes effect at once, in a game too.
It changes only what the graphics card draws (`ModelFrameInputs::new_unit_shadows`
in `src/app/runtime_full.hpp`, `renderer_live_scene::Inputs::new_unit_shadows`):
On, every unit's, wreck's and shot's shadow is laid on the ground before
anything stands on it, so that overlapping shadows darken once and a
unit's shadow lies under the trees and units beside it, and an aircraft's
shadow has a soft edge; Off, each casts the game's own shadow with its
unit or shot, a unit's the silhouette the processor blends
(`ground_shadow` in `src/present/model`), over whatever was drawn before
it, as the trees' and rocks' own shadows are drawn. The processor draws the game's own either
way, and nothing a match shares, saves or records changes.

Language says which language the game shows its text in. System default
is the language the operating system's preferred locales choose, which the
drop-down names in itself: "System default (Deutsch)". The other choices
are the languages the game knows and draws, each named in itself, English
first and the others in the order of their names ([the languages
page](../../../docs/languages.md) says what each shows). The key holds
`system` or the language's tag; a file without it, as an earlier version
wrote, reads as System default with the player's own preferences file, and
a value the game does not know reads as the default but stays in the file
until the setting is changed. A named preferences file starts in English,
the game's own default, so that a check plays alike on every machine. The
language changes only what players read, so no game locks it; 3.1c's
command line naming a language (`open-annihilation german`) decides it for
the run, and the row shows "Set on the command line". A new choice shows at
once in what is drawn each frame; screens and panels already open show it
once they open again.

Languages a catalogue offers, and the game does not have yet, are listed
under AVAILABLE TO DOWNLOAD. The host fills `Dialog::available_languages`.
Each `AvailableLanguage` carries the tag, the endonym, the English name,
the aside, whether the endonym draws, whether the row can be chosen, and
where its download stands. `src/language_page.cpp` builds the menu from
that list. Choosing one asks the host to queue the download, and the
closed field then shows the language and its progress. An available item's
automation name is `oa.settings.language.` and the tag in lower case:
`oa.settings.language.zh-hans`, `oa.settings.language.pt-br`. The divider
above those rows has no automation name.

While the host says the Library can open over the dialog
(`Dialog::library_link`, which the game sets on the main menu's engine
settings alone), MORE IN THE LIBRARY… stands right under Language's row, a
link (`Setting::more_in_library`, named `settings.more-in-library`) whose
hint says "More languages are in the Library.". Its press sets
`Dialog::library_tab` to Languages and asks the host for the Library
(`DialogAction::open_library`); the dialog stays open under it. In a match
and in Language & Text alone it is not listed, and Language's rows stand as
`page_settings` gives them.

The rest of Language & Text says how game text is drawn. Use modern fonts
for game text draws it in modern fonts, which hold the letters of many
languages, in place of the game's own 8-bit fonts; Text size scales every
size those fonts are drawn at together, from half the game fonts' sizes to
three times them; Font outline gives each letter of that text a dark
outline, Font shadow a dark shadow, and Game text background lays a
shaded box behind each line. `text_style` turns them into the one record
the text drawing reads each frame (`oa::present::TextStyle`,
`oa/present/text_style.hpp`), which the application gives through
`Runtime::text_style`. They change only what is drawn: the simulation, a
saved game and what a shared game sends are the same whatever they hold,
so no game locks them. Modern fonts are On by default with the player's
own preferences file, and Off with `--preferences-file`; the text starts at
80%, a fifth smaller than the game's fonts. Both are deliberate departures
from the game's look.

Text size applies to the modern fonts alone: the game's own fonts have
fixed sizes. While the dialog shows Use modern fonts for game text Off, the
slider is locked, faded with a padlock and "Needs modern fonts"
(`Lock::needs_modern_fonts`, which the dialog sets itself in
`Locks::text_size`), and its second hint line says the game's own fonts
have fixed sizes; turning the modern fonts On lifts the lock at once.
Restore defaults resets the size whatever the switch shows.

Zoomed out units' strip shows three levels and offers the first two
(`Strip::offered`, `offered_levels`): Icons is drawn faded into the panel,
as a locked row is, and a click on it, or Right from Dots, changes
nothing. After zoom says where Dots begins, so while the dialog shows
Rendered it is locked, faded with a padlock and "Needs Dots"
(`Lock::needs_dots`, which the dialog sets itself in
`Locks::zoomed_out_after`); choosing Dots lifts the lock at once.

Window frame is never locked, and takes effect at once: the application
shows or hides the window's title bar and borders each frame
(`Runtime::apply_window_frame`, `window_frame_request` in
`src/app/include/oa/app/full_screen.hpp`). At Hidden in play a window
hides them while a game is played and shows them while the game menu or a
panel it opens is up, and on every other screen, so that the window can
be moved and closed there; at Always shown it shows them everywhere. Full
screen, a window still switching to or from it, and a run without a
window are left as they are. The window's contents keep their size and
place as the frame comes and goes (`set_window_frame`), so the screen
size the dialog shows does not change and no size reads as Custom.

HUD scaling is never locked, and takes effect at once, in a game too: the
application lays the game out again (`Runtime::apply_output_mode`). On,
the side column, its minimap and build menu, and the top and bottom bars
are drawn at the window's scale, up to `kMaxChromeScale`; Off, at the
original game's size on every window (`Runtime::match_chrome_most_scale`).
The touch controls' layouts keep their own sizes either way.

Interface size is never locked: it changes nothing a match or a replay
shares. A size chosen in its drop-down waits for OK, as Screen size does,
so the dialog keeps the size it opened at while it is open; on OK the
OA layer lays its screens out again at the new size from the next frame
(`Runtime::apply_engine_settings`). It changes no game text (Text size),
no part of a game's interface (HUD scaling) and no 3.1c menu (Menu
scaling). In a match the in-game Settings starts from it too, held to the
side column's scale ([Sizes](#sizes)).

150% is drawn in the modern fonts, as the game's bitmap fonts take only
whole steps, so the drop-down offers it, between 100% and 200%, only while
the dialog shows Use modern fonts for game text On (the switch, or a
language that locks it On). Turning the switch Off while 150% is chosen
sets Interface size to 100% in the dialog, and turning it On again leaves
100%. A stored `150` read while the modern fonts read Off reads as 100%
(`interface_size_with_fonts`), and the OA layer draws 150% only while the
modern fonts in effect are On and open. At 150% the screens are laid out
for the window's points at one and a half and drawn on the kit canvas's
smooth form: every rectangle's edge on a whole pixel, the text in the
modern fonts at one and a half times its size (`src/ui/kit/README.md`).

A language drawn only in the modern fonts (`TextNeeds::modern_fonts`,
Simplified Chinese) turns Use modern fonts for game text On when it is
chosen, and while it is chosen the switch shows On with a padlock and "Set
by the language" (`Lock::set_by_language`, in `Locks::modern_fonts`).
Enable Unicode Multiplayer Chat is locked the same way (`Locks::unicode_chat`)
while the language chosen is one of `Dialog::unicode_chat_languages`, the
tags whose language packs say `unicode: true`, which the host sets as the
dialog opens; the stored value is the player's own and returns with
another language. A stored size
is clamped to 50% to 300%, and one between the slider's steps is kept as
stored; a file without the key reads as 80%. How each place in the game
draws the size is in the application's README and on the
[ui.text-rendering](../../../docs/mods/standard-hacks/ui.text-rendering.md)
page.

The Mod, the mod folder the game plays, layered over the game folder as
`--mod-dir` would play it, has no section of the dialog: the application's
Library chooses it, with Play now, and Switch off for No Mod, stores it
with `write_settings`, and reloads the game for it. The dialog keeps it as
it is, through Restore defaults too. A stored mod folder the game folder
does not offer reads as the stored one (`EngineSettings::picked_mod_folder`);
a file without either key, as earlier versions wrote, reads No Mod. The
picked folder is remembered only while it is the Mod
(`remembered_picked_folder`): a picked folder's key naming any other folder
reads as none, and `write_settings` erases it, so that a switch to another
mod or No Mod forgets the folder. Developer Mode keeps its overrides under
the id of the profile the game plays, and No Mod the plain 3.1c baseline's
(`ta-3.1c`), which no mod's profile can take. A mod folder without a
profile keeps its own under `folder:` and its path (`folder_overrides_id`
in the application), which no profile's kebab-case id can be either.

Developer Mode is described [below](#developer-mode).

With every default the game plays as it does without the settings. Pathfinding
cycles, Unit limit and Mod are locked during a game; a shared game or a replay
always plays at 1× pathfinding and the host's unit limit. Vertical sync is
locked during a shared game or a replay, its value set before the game kept
in effect; Hardware acceleration never is, so that it can always be set to
Off, and set to Basic or Full there it takes effect from the next game.

`engine_settings/file_types.hpp` turns System's eight file-type switches
into the file types they choose, each switch its own type alone:
`file_type_set` is the set a registration acts on (its complement is the
set a withdrawal acts on), `set_file_type_switches` shows a set the system
reports, and `moved_file_type_switches` gives the default-opener change of
each switch that moved, in System's order. The application registers,
withdraws and asks macOS through `oa/platform/file_types.hpp` with them.

## The dialog

`engine_settings/dialog.hpp` holds the dialog (`Dialog`), what pointer, wheel
and key events do to it (`dialog_pointer_down` and the others, `dialog_wheel`,
`dialog_key`), where its parts lie (`dialog_layout`), and how it and the OA
button are drawn (`draw_dialog`, `draw_oa_button`, and `draw_oa_mark` for
the mark alone on other screens) without the game's art, in
the game's own fonts (`load_dialog_fonts`): its button font for labels,
values, the section list and the title, and its smaller label font for the
section heading, hints, locks, captions and the version, each readied for
text in one colour. Every setting's row is declared once, in the rows'
table (below), and `src/geometry.hpp` places every part, so a control is
pressed where it is drawn. The dialog is drawn from one display list
(`geometry::dialog_list`, below), which `draw_dialog` paints with the
kit's `paint`. The window's face and raised edge, its header,
its section list, a section's heading, a row's rule, label and hints, a
locked row's fade and the footer's band are drawn by `oa/ui/kit/chrome.hpp`.
The buttons, the switch, the level strip, the slider, the drop-down and its
menu, the lock, the scroll bar, the focus ring and the OA mark are drawn by
`oa/ui/kit/components.hpp`. The dialog places each one and passes the
caption it has already looked up. `Dialog::section_hooks` (`SectionHooks`) lets the
dialog's tests and the game's checks show rows and locks of their own in
place of a section's, on Developer in place of its list and the list's
footer too; a host never sets it.

A registered page (`set_registered_pages`) is listed after the engine's own
sections, under a divider that names its owner, in the order it was
registered. Pages of one owner share that divider. The divider is a margin,
a one-pixel rule, a margin and the owner's name in the small font. The page
itself is laid out from its preference rows (`PreferenceRow`), at the
dialog's size class, the same way the engine's rows are. OK writes each key
whose stored text changed and the host calls the page's `applied` once the
preferences are saved. Cancel drops the changes. Restore defaults puts the
open registered page's rows back to their defaults. A mod's options dialog
lists none of them. With no registered page the section list is the engine's
own, Developer pinned at the foot.

The host hands both drawing functions an RGBA picture. Settings passes the
settings icon, the plates over two gears. Notices, prompts and the other
OA headers pass the app icon, the plates alone. They scale it with
`draw_picture` to its place at the surface's own resolution, an area
filter, so a dialog drawn twice as large shows the icon twice as sharp and
a clear corner stays clear. The OA button is a bevelled square with the
icon 3 source pixels inside its edge: lit with a green ring inside the
bevel under the pointer, and, while held, its bevel sunk and the icon a
pixel right and down. Without the icon, an empty picture, the header and
the button draw the OA mark instead: green "OA" letters in a green outlined
square. `draw_oa_mark` draws the picture it is given filling a square, or
the mark the OA button shows at rest. The Game files screen passes the app
icon, so its badge is the plates, not the gears.

Where a game font has no ellipsis, as neither of the dialog's has, the
dialog draws and measures each ellipsis in a text as three full stops in
the game font, so that a caption such as Manage… keeps the game font's
look and fits its button like every other caption; the mark between a
location's folders (›) is drawn as a greater-than sign the same way, and
a no-break space as the font's space. The texts themselves, and their
translations, keep their characters; every other character the game font
lacks is drawn in the modern fonts. A line that holds a letter the game
font lacks, as a Ukrainian line's Cyrillic, is drawn whole in the modern
fonts, its digits, Latin words, marks and spaces with it, so that no word
of it changes typeface part way.

At Compact it is 480 by 324 source pixels, a dark gunmetal panel with a
one-pixel raised edge and hairline rules, and one green accent for what is
selected, laid out on Kit 2 ([Sizes](#sizes)):

- the header, 26 pixels high at Compact and 36 at Regular and Large: the
  icon it was given, 20 by 20 source pixels, "OPEN ANNIHILATION SETTINGS" and the version, with "Shared game - still
  running" in amber while a shared game keeps running;
- the sections down the left in four groups, a hairline between two: what
  the player plays with (Controls, Accessibility, Touch while the game has
  touch controls, Controller once a gamepad has sent input), how the game
  looks (Graphics, User Interface, Language & Text), what it holds and
  fetches (Game files where the host lists it, Downloads, Updates, Game
  Content), and the machine and the engine (Engine Limits, System,
  Developer); each entry a line icon (`src/section_icons.cpp`) and its
  name, the open one on the selected ground with the lime marker, Updates
  with the update's progress at its right while the host gives one
  (`Dialog::updates_badge`);
- the open section's heading and rows: a label, a hint of one or two lines,
  and an Off/On switch, a level strip (Off, 2x, 4x, 8x, 16x for Enhanced
  anti-aliasing; Off, Basic, Full for Hardware acceleration; Automatic,
  Box, Scroll for One-finger drag; Stay on, One action for QUEUE and ADD;
  Standard, Large, Larger for Control size; and the Controller section's
  Scheme, Right trackpad, Pointer acceleration, Right stick and Haptics),
  a slider with stops and its value under the hint, or a drop-down under
  the hint (Language, Gyro pointer, whose field is wider for its longest
  choice, and Button prompts): a field showing the choice, with an arrow at
  its right; a text row shows only text under its label (Where the files
  are, and Controller's Steam Input notice of up to four lines);
- the footer, Kit 2's band: RESTORE DEFAULTS at its left, in the small
  font's tracked capitals as a window's own action keeps them, then how
  many settings the dialog shows changed ("1 setting changed", "{count}
  settings changed", `changed_settings_count`; nothing while none did),
  right against Cancel and OK.

A section holds any number of rows. They lie in a view under the section's
heading, from the first row's line at pixel row 54 down to the pixel row
above the footer's line, 220 pixels high at Compact; the header, the list, the heading
and the footer never move. A section whose rows, with 8 clear pixels under
the last row's line, are taller than the view scrolls by whole source
pixels, and shows a scroll bar in the margin right of its rows: a well like
a switch's, its thumb as tall as the view's share of the section and never
under 16 pixels. At Compact, Graphics is taller than its view by 988
pixels, Controls, with the key layout card, by 383, User Interface, with
Window size and Reset window positions, by 230, Language & Text by 204,
Engine Limits by 149 and Touch by 137; a section that fits draws as if
there were no scrolling, with no bar. While the dialog's words are drawn in the modern fonts, as in
Simplified Chinese, whose ideographs stand as tall as a hint line, the
lines of a hint or a notice lie three pixels further apart
(`tall_hint_line_gap`), clear of each other: each row with such lines is
taller by three pixels a line after the first, a section that scrolls
scrolls further than the pixels given above, and its scroll ends only
where the view's top edge cuts no hint line (`open_rows`). Each section
keeps its offset while the dialog is open, and every section starts at
its top each time it opens. A row the view cuts shows the part inside it
and takes a press only there;
while the section is scrolled from its top, the view's first pixel row keeps
a hairline, the same as a row's own line. `dialog_layout` lists only the
parts wholly in the view.

The mouse wheel over the dialog scrolls the section 24 pixels a notch,
carrying a fraction of a pixel to the next turn; what is carried towards an
end the section has reached is dropped, and all of it when another section
shows. The scroll bar takes a press anywhere in the margin, on its thumb to
drag it or on its well to bring the thumb's middle there and drag it from
there; the thumb follows the pointer's row only. The bar takes no keyboard
focus, and a press on it leaves the focus where it is. While a press is held
the wheel and the scroll keys do nothing, so only a drag of the scroll bar
scrolls then. No scroll changes a setting or moves the focus.

Controls are numbered: the sections' entries 0 to 13, each its place among
the sections with Touch, Controller, Game files, Downloads, System, Game
Content, Updates and Accessibility whether or not the first three are
listed (Touch 5, Controller 6, Developer 7, Game files 8, Downloads 9,
System 10, Game Content 11, Updates 12, Accessibility 13, so a dialog
without Touch has no control 5, one without Controller no control 6, and
one without Game files no control 8). Touch, Controller and Developer keep
their numbers in every dialog (a mod's options' 0 to 4). Restore defaults
is 14, Cancel 15, OK 16, the scroll bar 17, and the open section's rows
from 18, with no upper end. The list draws its entries by their place in
the list it shows, and the focus walks that list. Downloads, Updates, Game
Content, System and Accessibility are always in that list. The list
stands in four groups, a hairline between each: Controls, Accessibility,
Touch and Controller; Graphics, User Interface and Language & Text; Game
files, Downloads, Updates and Game Content; Engine Limits, System and
Developer (`geometry::nav_group`, `nav_layout`). Each entry draws its icon
(`section_icons.cpp`) left of its name, lit while the entry is open or
under the pointer; where a section's name in the language needs the
icons' room, the list draws its names without icons, from the icons'
column, and grows only for what is still missing (`fitted_sizes`). Updates shows `Dialog::updates_badge`, the host's short
word for an update under way, at its right where the name still fits
whole. At Compact a list of twelve entries or more is taller than the rail
and scrolls by the wheel, the open entry (or the focused one) kept in view;
a thin bar between the entries and the list's rule shows that it scrolls,
its thumb where the list is scrolled to (`nav_bar`).

The Touch section is listed only while the game has touch controls:
`Dialog::touch`, which the host gives `open_dialog` and keeps with
`set_touch_controls` each frame, so that Touch shows from the moment a
finger turns the touch controls on (`dialog_pages(kind, touch)`). A dialog
asked to open on Touch without them opens on its first section, Controls.

The Controller section is listed only once a gamepad has sent input in the
run: `Dialog::controller`, which the host gives `open_dialog` and keeps with
`set_controller_section` each frame, beside `Dialog::steam_input`, so that
Controller shows from the moment a gamepad is used and its Steam Input
notice while it applies (`dialog_pages(kind, touch, game_files,
controller)`). It stands after Touch and before Game files. A dialog asked
to open on Controller without it opens on its first section, Controls; one
that stops listing it while it shows it shows Controls, and the focus leaves its
controls. The notice coming or going while Controller shows moves each row
by one, and the focus with its row. Its rows: Scheme, Right trackpad,
Pointer speed, Pointer acceleration, Trackpad glide, Right stick, Magnetism
(stick pointer), Gyro pointer, Gyro speed, Haptics, Button prompts,
Left-handed, then Touch's own Control size, Hold delay and QUEUE and ADD.

The Game files section is listed, between Controller (or Touch, or User
Interface) and Downloads, only where the host says so (`Dialog::game_files`, given to
`open_dialog`): the main menu's dialog of a game whose platform brings game
files in. A dialog asked to open on Game files without it opens on its
first section, Controls. Its three rows:

- Installed: what is installed and, under it, the sizes line, both the
  host's texts (`Dialog::game_files_summary`, `game_files_sizes`), with
  Manage… at the label line's right, a green button like OK with its
  caption, drawn "Manage...", centred inside it. Manage… is a button: a click on it, or
  Space while it has the focus, returns `DialogAction::manage_game_files`,
  and the host opens the Game files screen with the dialog left open under
  it. It takes no steps: Left moves the focus to the section's entry at its
  height, and Right finds nothing.
- Include in device backups: a switch, Off by default, whose hint names
  the device ("After restoring this tablet from a backup, add the game
  files again.", from `Dialog::game_files_device`, "device" without one).
  It is put in effect as it changes; Restore defaults turns it Off, and a
  dialog that does not list Game files keeps it through Restore defaults.
- Where the files are: the host's text (`Dialog::game_files_location`) in
  up to two lines of at most 50 characters, broken between words, after
  the mark between a location's folders where one stands in a line's
  second half. It takes no press and no focus.

The host's texts keep to their lines' columns as they are drawn.

The Downloads section is listed in every dialog of the engine's settings,
after Game files where that is listed and otherwise after Controller, Touch
or User Interface, and before the line above Developer. Its rows come from the
host's offer (`set_downloads_offer`), not from a fixed list of settings:

- Check for content updates: a drop-down of Automatically, Only when I
  open the Library, and Never. It is a setting (`content_check`). A change
  takes effect at OK. Cancel puts the opened choice back, and Restore
  defaults sets Automatically.
- One install ID for each registry the offer names, built-in first. The
  switch is labelled Install ID, or Install ID · {name} when more than one
  registry uses an ID. Its hint names that registry. While no install ID
  exists, "Made at your first download" is that hint's second line, and
  there is no value row and no RESET. Once one exists, a row labelled
  "Your install ID" shows the first 8 hex digits, a gap, and the last 4
  (7f3a90d2 · · · c91e), or Off while the switch is off, with RESET beside
  the value. RESET is disabled while the switch is off. Turning the switch
  off or on takes effect at OK and Cancel puts it back. RESET acts at
  once, names the registry on `Dialog::install_id_registry`, and is not
  undone by Cancel.
- Downloaded files: the size kept for resuming and repairs, or Nothing
  kept. EMPTY acts at once. It is disabled when nothing is kept, and while
  a download is queued, running or paused the value names that the files
  are in use and EMPTY stays disabled.
- Registries in the Library… (`settings.registries-in-library`), on the
  main menu only (`Dialog::library_link`): a link whose hint reads
  "Registries are in the Library." It asks the host to open the Library
  on the Registries tab (`DialogAction::open_library`,
  `Dialog::library_tab` set to `LibraryTab::registries`). The registries
  themselves are drawn by the Library (`oa-ui-library`), not by this
  dialog.

In a game the page still shows. RESET and EMPTY stay where they are,
drawn faded, and an accent button loses its accent. The padlock and
"Locked during a game" stand at the right of the label line, immediately
left of the button when the button sits on that line. When the label, the
lock and the button do not fit on that line, the lock moves to the start
of the line under the label and the button stays. Downloaded files
shows no size line while locked: its label, the lock and the faded EMPTY.
The check and the install-ID switch stay editable. The link is not listed.

The Updates section is listed in every dialog of the engine's settings,
after Downloads and before Game Content. Its two rows come from the host's
offer (`Dialog::updates`), not from a fixed list of settings:

- The version line (`settings.update-status`): the label is "Open
  Annihilation" and the version, and the hint is the status (up to date,
  checking, a release out, could not check, not checked yet, or "OA never
  checks"). Check now (`settings.update-status.check-now`) asks the host
  at once (`DialogAction::check_updates`). While a release is offered the
  same button is Update… (`settings.update-status.update`,
  `DialogAction::open_release`). The button is hidden while Check for
  updates is Never and greyed while a check is running.
- Check for updates (`settings.update-check`): a drop-down of
  Automatically, Only when I ask, and Never. It is a setting
  (`update_check`, stored as `open-annihilation.update-check`). A change
  takes effect at OK. Cancel puts the opened choice back, and Restore
  defaults sets Automatically. Check now makes no request while the
  choice showing is Never.

`open_language_text_dialog` opens a dialog of the third kind,
`DialogKind::language_text`: Language alone, its entry at the top
of the list, with Restore defaults, which restores that section's
settings alone (each locked one kept), Cancel and OK. The Game files
screen opens it before the game's files are installed: given
`DialogFonts` that hold no glyphs, every text of the dialog is drawn and
measured in the modern fonts (the game-text hooks the host installs), the
regular font's in the message face and the small font's in the status
face, each at the game fonts' size and centred on its box as a game font's
capitals are. A character a game font lacks is drawn in the modern fonts
at the game fonts' size.

A finger's press (`dialog_finger_down`) that lands on no control takes the
nearest control within a reach the host gives, in the dialog's own pixels,
pressed at its pixel nearest the finger: the kit's reach (`kit::reach`) over
the dialog's display list. While a drop-down's list is open it reaches the
list's items alone. The press's moves and its release are moved as
far as the press was (`Dialog::finger_shift_x`, `finger_shift_y`), so that
a release where the finger landed acts on the control it took: a switch's
nearer half, a strip's nearer level, a slider dragged along its track. A
mouse press is never moved. The game's hosts give it the touch controls'
pick distance, 22 points, as the dialog's pixels on the screen shown.

The game fonts have no "×" or "·", so the dialog writes "x" and "-". Its
texts are UTF-8: each character the game font has a glyph for, a
language's "ç" or "ñ" among them, is drawn with it, and any other in the
modern fonts. The dialog's own words pass through the interface catalogue
(`oa/data/languages/interface_text.hpp`), which shows them in the language
chosen once a translation is given for it; none ships yet, so they show in
English.

A locked setting is faded, takes no press and no keyboard focus, and shows a
padlock with "Locked during a game", "Set by the host", "Set on the command
line" (the frame rate under `--max-fps`, Hardware acceleration under
`--hardware-acceleration`, in any of its forms, or
`--no-hardware-acceleration`, Native pixel density under
`--native-density`), "Not available here" (Hardware
acceleration when nothing in the game could help the run, and Vertical
sync on SDL's software renderer or where each change would reset the
graphics device), "Needs modern fonts" (Text size while the modern fonts
are Off), "Needs Dots" (After zoom while Zoomed out units is Rendered) or
"Always on here" (Native pixel density where the platform
opens every window at native density). Every locked row uses one look.
The control stays where it is, drawn in that fade, and an accent button
loses its accent. The padlock stands at the right end of the label line,
immediately left of the control when the control sits on that line (a
button or a switch). When the label, the padlock and that control do not
fit on one line, the padlock moves to the start of the line under the
label, before the hint, and the control stays. A control that does not
fit beside its label stands on the next line, and the padlock stays on
the label line unless the label and the padlock do not fit there either.
A row whose hint lines are its status fades its label line and a lock or
a control that dropped under it, and stops at the status, so that the
status keeps its strength.

Changes show at once; OK keeps them, Cancel puts back what the dialog opened
with, Restore defaults resets every setting that is not locked. A click on a
switch's half sets it, and a click on a strip's level chooses it; a press on
a slider moves its knob to the nearest stop and drags it. A click on a
drop-down's field opens its list under the field, or over it where the list
would reach below the footer's line: an item a line, as many as eight, the
chosen one marked as the section list marks its open section and the one
under the pointer lit. A click on an item chooses it and closes the list; a
press anywhere else, the field included, closes the list and does nothing
more. A longer list scrolls with the wheel, a notch an item, and shows a
thumb at its right edge; the wheel over an open list never scrolls the
section. Every switch reads
and sets its value through one table, and every strip through another. Each
press of Restore defaults, and each time Hardware acceleration passes to a
higher level, from Off to Basic or Full or from Basic to Full, adds one to
`Dialog::forget_renderer_failures`, the
player's requests to have the graphics card tried afresh; Restore defaults
reports a change every time, even when no setting moved, so that the host
acts on it.

The dialog's keys are the kit's (`DialogKey` is `oa::ui::kit::Key`), so a
host passes one through as the other. Backspace and Delete, which edit a
kit screen's text, do nothing in the dialog, its drop-down lists, its
notices and its prompts.

| Key | Does |
|---|---|
| Enter | OK |
| Escape | Cancel |
| Tab | the focus to the next control in the declared order: the section's rows, Restore defaults, Cancel, OK, then the sections, and round to the first |
| Shift+Tab | the focus to the previous control in that order |
| Up, Down | the focus to the control above or below |
| Left, Right | a switch Off or On, a slider, a level strip or a drop-down one step, Your files' mark along its buttons, a row of Developer's list, Show Active Only; from any other control, the focus to the control on that side |
| Space | flips a switch, presses a button, opens a section or a drop-down's list |
| Page Down, Page Up | scroll the section a view less three hint lines down or up: 184 pixels at Compact |
| End, Home | scroll the section to its end or its top |

While a drop-down's list is open, the keys work the list: Up and Down mark
the item above or below, Page Up and Page Down a list's height of items
away, Home and End the first and the last; Enter and Space choose the
marked item and close the list; Escape closes it unchanged, leaving the
dialog open; Tab and Shift+Tab close it and move the focus.

The arrows move the focus by where the controls lie, by the kit's rule
(`kit::focus_toward`; see [the kit](../kit/README.md)): a control in line
with the focused one comes first, then the nearest. The open section's rows
and Developer's list are each a scroll area, searched first from
a control inside it, rows it does not show included; a move from outside it
never lands on a row it hides. The sections' entries and the footer lie in
no scroll area. A row's control lies across the section at its control's
line, so Up and Down walk the rows in order wherever on its line each
control sits, Down from the last row reaches the footer (Cancel, whose
middle is nearer the row's), and Right from a section's entry reaches the
row at its height. An arrow that finds no control that way leaves the focus
where it is. From no focus, Up shows the focus on the last control in Tab's
order and the other arrows on the first. Tab and Shift+Tab never follow
where controls lie.

The focus shows once a key moves it; the first key to the dialog only shows
it. Page Up, Page Down, Home and End scroll whatever has the focus, and
never show or move it. A key that moves the focus onto a row, or acts on a
focused row, first scrolls the least that shows the row whole; a key that
moves it to a button or a section's entry does not scroll.

### The key layout card

Controls' first row is a card (`cards::key_card`, `src/cards.cpp`), across
the section under the row's line. It shows the keyboard tile, the eyebrow
"Key layout", the layout's name (`Dialog::key_layout_name`, wrapped, never
cut), a badge counting the player's own changes to it
(`Dialog::key_layout_changes`: "1 change of your own", "{count} changes of
your own"; none while there are none), where it applies ("Applies to every
game.", or "Applies with {mod} only." while the mod played chooses its own,
`Dialog::key_layout_mod_only`) and how a match shows the keys ("Ctrl+/ in a
match shows the keys.", Cmd on a Mac). Its buttons, Change layout… and Edit
keys…, stand right of the words while the words keep at least 120 points,
and under them where they do not. Change layout… shows only where the
Library opens over the dialog (`Dialog::library_link`) and opens it on Keys
(`change_layout_control`); Edit keys… is the row's own control and returns
`DialogAction::edit_keys`. The host fills the name, the count and where it
applies each time the dialog opens and after the key editor or the Library
closes.

### The practice pad

Accessibility shows a pad (`cards::practice_pad`) where the player tries
the helpers the section shows: in a column right of the rows at Regular and
Large (200 and 260 points), and under the last row at Compact. It has the
eyebrow "Try it here", what to try with the helpers that are on (Sticky
modifiers: "Tap Shift, then press A."), a well with the keys held, latched
or locked as keycaps, what the last key did (latched, locked, let go,
counted, too short for Slow keys, bounced, held long enough or let go too
soon for Hold to confirm), Hold to confirm's bar while that helper is on,
and Turn every helper off (`helpers_off_control`), which turns Sticky
modifiers, Hold to confirm, Slow keys, Bounce keys, the Order wheel and
Pointer keys off and lets go of the pad's keys; it is greyed while all of
them are off. The host hands the pad keys with `dialog_practice_key` and
time with `dialog_practice_tick`; both do nothing outside Accessibility.
Keys pressed on the pad never reach the game.

### The footer's count

The footer counts the settings the dialog shows changed from what it
opened with (`changed_settings_count`): each setting it would store
differently, the install IDs' switches, Welcome setup's Show again and the
registered pages' own writes; on a mod's options page, each option whose
value reads differently. It shows "1 setting changed" or "{count} settings
changed" right against Cancel, wrapping to two lines in the small font
where one does not fit, and nothing while none changed.

## Sizes

The dialog, its notices and its prompts are laid out in points, one point a
pixel of the game's 640 by 480 picture, at one of three size classes
(`Dialog::size_class`, `kit::Notice::size_class`, `kit::Question::size_class`).
A host chooses the class from the room it has and draws the dialog at a
whole scale (`draw_dialog`'s placement); the OA layer chooses both from the
window ([src/app](../../app/README.md#the-oa-layer)). The classes' sizes are
the UI kit's metrics (`kit::compact_metrics`, `kit::regular_metrics`,
`kit::large_metrics`, picked by `kit::metrics_of`):

| | Compact | Regular | Large |
|---|---|---|---|
| Used for room of | below 768 by 534 points | from 768 by 534 | from 1008 by 648 |
| Settings dialog | 480 by 324 | 720 by 486 | 960 by 600 |
| Padding | 12 | 18 | 24 |
| Section list's width | 144 | 176 | 208 |
| Section entry's height | 19 | 25 | 32 |
| Header's height | 26 | 36 | 36 |
| Footer's buttons' height | 28 | 34 | 38 |
| Footer's height | 48 | 48 | 48 |
| Characters a line of a host's text under a row | 50 | 82 | 114 |
| Notice's and prompt's width | 400 | 520 | 600 |
| Notice's and prompt's greatest height | 440 | 500 | 640 |
| Notice's and prompt's least height | 150 | 150 | 150 |

The sizes are Kit 2's metrics (`kit::Metrics::window_padding`,
`nav_width`, `control_height`, `window_footer_height`, `space`); the
section list's own entry heights and icon places are
`geometry::nav_metrics`. The pixel pins (`ui-engine-settings-pixels`) hold
Compact. The window's header is the kit's 26-point band at Compact, where
nothing larger fits and the section list needs its room, and Kit 2's 36
points at Regular and Large (`Sizes::header_height`, given to the kit as
`HeaderLook::height`); the heading and the first row follow it
(`Sizes::body_top`, `Sizes::first_row_top`). Every other
size is the same in all three classes: rows keep their height, and text,
switches, strips, drop-down fields, buttons and the slider's line keep
theirs, so a larger dialog shows more rows and scrolls less
(`scroll_limit`). The content column starts right of the section list and
ends the padding short of the right edge; a section's rows and Developer's
list take its width, a slider's track running across it, and
the body between the header's and the footer's rules. The header's mark,
title and version keep their Compact distances from the dialog's edges,
the footer's buttons keep their order, Restore defaults at the
padding from the left, each at least as wide as its caption needs. A hint keeps its lines; the host's own texts
under a row (Controller's Steam Input notice, where the game files are)
break at the class's characters. A drop-down's open list opens over its
field when it would pass the class's footer line. A notice's or a prompt's
text wraps at the class's width, through the kit's one wrap, and its
buttons keep their sizes.

The OA layer takes the class and the scale from the window
(`kit::layer_viewport`): the Interface size's whole step, or with Auto the
window's height over 720, the nearest step, never below the display's
density. While the modern fonts are on and opened, 150% is among Auto's
steps and a tie takes the larger (1× below 900 lines, 150% from 900, 2×
from 1260); otherwise Auto keeps whole steps (1× below 1080, 2× from 1080).
The step is capped at the largest at which Compact's 480 by 324 dialog fits
the window, less its safe insets on a touch screen, and is at least 1×, so a
size never draws a dialog cut off by the window's edge. The class then
comes from the window's points at that step: the largest whose dialog fits
them with 24 points round it (`kit::size_class_of`), so Regular from 768 by
534 points and Large from 1008 by 648. Where Auto's step leaves the window
below Large, or below the class Window size chose, and one step down gives
it that class, Auto takes the step; a chosen Interface size never steps
down. A class Window size chose is used wherever its dialog fits the
points whole, and the largest that fits elsewhere
(`kit::size_class_within`). 150%, chosen or what Auto picked, is held the
same way:
it is 1.5× where Compact's 720 by 486 pixels at one and a half fit the
window, and 1× where they do not. On a 1920 by 1080 window Auto with the
game's fonts is Large at 1×, a step down from 2×, which would leave 960 by
540 points, Regular; 100% is Large at 1×, 150% is Large at 1.5× (1280 by
720 points, the dialog 1440 by 900 pixels), 200% is Regular at 2× and 400%
is Compact at 3×, as 4× does not fit Compact in 1080 rows; on 1280 by 720
150% is Compact at 1.5× (853 by 480 points), and on 640 by 480 it is 1×.
800 by 600 is Regular at 1×, and 1280 by 720 and 2560 by 1440, at 1× and
2×, are Large.

In a match the dialog is laid out and drawn at a class and scale of its own
(`OaLayer::match_dialog`, `window_placement::match_window`,
[src/app](../../app/README.md#the-oa-layer)), never drawn at 1× and
enlarged. Its scale is the layer's on the match's canvas, the Interface
size's or Auto's with Auto's step down, capped by HUD scaling, as the
dialog always was: above the side column's scale it is the largest whole
step at or below that scale, and at least 1. The class comes from the
window's whole safe area at that scale, and the dialog, its class's size
at the scale, is centred in the window, or centred across the area right
of the side column where the middle would cover the column and that area
holds it. With HUD scaling On:

| Window | Scale | Class | The dialog's place, in pixels |
|---|---|---|---|
| 640 by 480 | 1× | Compact | (144, 78), right of the column |
| 800 by 600 | 1× | Regular | (40, 57), over the column, which leaves too little room |
| 1280 by 720 | 1× | Large | (256, 60), right of the column |
| 1920 by 1080 | 1× | Large | (480, 240), in the middle |
| 1920 by 1080, modern fonts on | 1.5× | Large | (368, 90) |
| 2560 by 1440 | 2× | Large | (320, 120), in the middle |
| 3840 by 2160 | 2×, the column's scale capping 3× | Large | in the middle |

A 640 by 480 window shows the dialog at (144, 78), pixel for pixel as
before. On a phone with touch controls the dialog is still Compact, drawn
at 1× and fitted to the safe area.

## The rows' table

Every setting has one row, declared once in `src/settings_rows.cpp`, in
`Setting`'s order: a `kit::RowSpec<SettingsModel>` (`oa/ui/kit/rows.hpp`)
holding its id, its kind, its English label and hints, and its binding to
the settings the dialog shows. A `static_assert` holds the table to one row
for every enumerator, each at its setting's place and its id the setting's
name in kebab case (`setting_name`: `vertical-sync` for
`Setting::vertical_sync`). Beside each spec the table keeps the field of the
dialog's locks that locks the row, how Restore defaults copies it, and
whether its first hint line is a folder's path. `SettingsModel` is the
settings a row reads and sets and the dialog that shows them, for what a
row shows beyond them: the unit limit's highest stop, the offered screen
sizes, the system's language, Hardware acceleration's status, the Steam
Input notice and a Steam Deck's rate, and the host's texts.

Everything the dialog decides about one setting is read from its row: its
kind (`geometry::kind_of`: a switch is `toggle`, a strip `levels`, a slider
`slider`, a drop-down `choice`, Manage… a `value_and_button` with no value,
Your files `buttons`, and Where the files are and the Steam Input notice
`text`), its label and hints, its stops, levels or items and their
captions and value texts, its lock and whether its hints are its status.
Placement goes through the kit's `place_rows` with each row's view
(`geometry::row_view`, `geometry::place_section`), and the events through
the kit's `step`, `activate`, `press`, `drag` and `choose` on the row's
spec. Restore defaults copies each row through `geometry::copy_row`: a
switch, a strip and a drop-down of choices through their get and set, and
a slider, Language, Zoomed out units and the Mod through their own fields,
since their stops or items do not hold every value they may have (a value
between two stops is kept as stored). The geometry functions the dialog's
tests call (`stop_of`, `set_stop`, `strip_caption`, `value_text`,
`switch_on` and the others) each read the table. A section's rows, name
and heading are arrays indexed by `Page`.

To add a setting: add its enumerator to `Setting` and its value to
`EngineSettings` (with its key in `engine_settings.cpp`), then its row in
the table at the enumerator's place, made by the kit's factory for its
kind (`toggle`, `choice`, `slider`, `levels` and the others) with its id,
label, hint and binding functions, its lock field when a lock applies and
an exact copy when its stops do not hold every value; add its name to the
names array, and list it in its section's rows. Nothing else in the dialog
changes.

## The display list and the names

`geometry::dialog_list` returns what the dialog draws, in the order it
draws it (the face, the header, the section list, the open section's
heading and rows, or Developer's rows, list and footer, the footer band
and buttons, the edge and an open drop-down's menu), and its controls, in
the order a press tries them, each named for automation. The rows are the
kit's `add_rows` with the prefix `settings`, each row's control in the
section's scroll group; Developer's list and its footer are generic items
and the kit's components. Tab follows the declared order
the list holds (`DisplayList::tab_order`), which `dialog_list` builds as it
adds the controls.

The dialog's input reads this list through the kit, built in the fonts the
dialog is drawn in (`Dialog::drawn_fonts`, which the host sets to the fonts
it draws with): the sizes they widen and the lines they wrap the rows'
texts into come from one place (`geometry::shown_section`), so a control
answers where it is drawn whatever its Interface size, size class, type
step or language. Without fonts every character is measured at
`estimated_character_width`, for drawing and input alike. `dialog_layout`
lists the parts in the same fonts unless it is given others. The control
under a point is `kit::hit`'s, a finger's `kit::reach`'s, Tab's next control
`kit::next_in_tab_order`'s, an arrow's `kit::focus_toward`'s, and the wheel
carries its fraction with `kit::wheel_offset`. A row's control lies across
the section at its control's line, and its clip, the view narrowed to the
control's columns, keeps a press to the control where the view shows it.
The controls that take Left and Right as steps say so (`Control::steps`):
the rows' switches, strips, sliders and drop-downs, Your files, Developer's
list's rows and Show Active Only. The scroll groups are the section's rows
(`section_group`) and Developer's list (`developer_list_group`); the
entries, the footer, Show Active Only and Restore profile values are in
none.

The names: a registered page's entry is `settings.nav.<id>`, its id as
registered (`settings.nav.check.page`), and its rows `settings.<id>.<row>`
(`settings.check.page.toggle`, whose halves are `.off` and `.on`). The
engine's section entries are `settings.nav.<page>` (`controls`,
`engine-limits`, `language`, `graphics`, `user-interface`, `touch`, `controller`,
`developer`, `game-files`, `mod-keys`, `mod-patrol`, `mod-guard`,
`mod-tools`, `mod-chat`); `settings.restore-defaults`, `settings.cancel`,
`settings.ok` and `settings.scroll-bar`; each row `settings.<row>`, its
setting's name (`settings.vertical-sync`), Manage…
`settings.game-files-summary.manage`, and Your files `settings.user-folder`;
an open drop-down's items `settings.<row>.<value>` while it is open;
Developer's `settings.active-only`,
`settings.restore-profile-values`, `settings.hack-area.<area>`,
`settings.hack.<hack id>` and `settings.hack.<hack id>.<parameter>`, with
`.<value>` for a set's value, `.<n>` for a list's item and `.length` for
its length; Downloads' Registries in the Library…
`settings.registries-in-library`, on the main menu, which opens the
Library on its Registries tab.

Every word is the code's, never a text as shown, so no name changes with
the language. A row's word is its `Setting`'s enumerator with `_` as `-`,
its spec's id; a drop-down's item's or a strip's level's `<value>` is its
id (`kit::Stepper::id`): the word the preferences keep it as
(`stored_words.hpp`, or the text functions such as `menu_scaling_text`),
lower case with every other character a `-` (`1/32` gives `1-32`, a
language's tag `zh-Hans` gives `zh-hans` (`oa.settings.language.zh-hans`),
System default `system`), and
for Zoomed out units' Icons, which no preference keeps yet, `icons`.
`geometry::choice_word` gives it, and the dialog names an open menu's item
controls by it. The kit lists the parts of a control that take a press of
their own after it (`kit::automation_parts`): a switch's
`.off` and `.on`, a strip's levels, Your files' `.saves`, `.screenshots`
and `.mods`, an open drop-down's items. The automation endpoint lists
them all after `oa.` ([docs/automation.md](../../../docs/automation.md#oas-own-screens)).

## Developer Mode

Developer Mode lets the player change the standard hacks of the profile
the game plays: turn any hack on or off and set its parameters, without
touching the mod's `oamod.yaml`. With no mod it changes the plain 3.1c
baseline, so it can turn hacks on over 3.1c.

It lies in the Developer section, which holds, top to bottom, in the
dialog's own fonts, colours, switches and sliders:

- at its top, **Enable Developer Mode**, an Off/On switch, Off by default,
  with its hint, under it **Show performance statistics**, which
  Developer Mode leaves alone, and under that **UI kit gallery**, its hint
  "Every part of OA's own screens, in each of its states, at this size."
  and a **Show** button (`settings.kit-gallery.show` to automation) that
  asks the host to show the UI kit gallery over the dialog
  (`DialogAction::show_gallery`; [src/ui/kit](../kit/README.md#gallery)),
  the dialog staying open under it and no setting changing: three rows
  with half the clear pixels of a section's rows round them, so that the
  list has room; at Regular and Large they never scroll. Each takes a label line and one hint
  line, but the gallery's hint takes two at Compact, where its line holds
  fewer characters: the list starts 51 points lower than with two rows at
  Compact, and 39 at Regular and Large (`developer_rule_for`). Where the
  rows end lower still, as in a language whose hints take more lines or
  while hints lie further apart in the modern fonts, the list starts under
  them (`developer_list_scroll`);
- under them, the list of every standard hack of the registry
  (`oa::data::mod_profile::standard_hacks`), at Regular and Large in a
  view of its own that scrolls as a long section does, with its scroll bar
  in the margin, and
  two levels of parts that open and close, every one closed at first: an
  area of the registry (`developer_areas`; its title, such as "Interface",
  and how many of its hacks are on), and under it each hack: its title,
  such as "Deterministic Wind", and its Off/On switch, and open, its id
  (`economy.deterministic-wind`) in a quieter colour, the registry's
  summary, "Applies at next match" in amber for a rule (sim-scope) hack,
  and a control for each parameter. The areas, and the hacks within each,
  follow their titles alphabetically as the language shown writes them;
  the titles are the registry's, in English, and pass through the
  interface catalogue like the dialog's other words;
- at its foot, which at Regular and Large never scrolls, **Show Active
  Only (X/Y)**, a switch that shows only the hacks that are on and the
  areas that hold one, X the hacks on as the section shows them and Y every
  standard hack (89), and **Restore profile values**, which clears every
  override.

At Compact Developer scrolls as one (`developer_scrolls_as_one`): its
three rows, the list from the line under them, then Show Active Only and
Restore profile values under the list's last row, all in the section's
own view with the section's scroll bar, as a long section scrolls. The
focus walks from the rows into the list and on to Show Active Only, each
scrolled the least that shows it whole; Page Down, End, the wheel and the
scroll bar move all of it.

A parameter's control follows its type: a switch for a boolean; a slider
for a whole number, from its least to its most by its step, the hack's
constraints with the other parameters (such as `min <= max`) narrowing it,
and for an int-or-none with a first stop for none; a slider of the
registry's values for an enumeration; for a decimal, a slider whose step
is the power of ten that gives it from 1,000 to 10,000 stops across its
range, with the registry's and the profile's own values among its stops;
a slider of the values the registry, its
presets and the profile give for a string; a switch for each value of a
set; and for a list, a slider for each item, an ascending list's items
kept in order, a list of distinct words swapping a word in with the item
that held it, and a slider of its length when the registry lets it vary.
Each value shows with its unit, and a hack that is off shows "Off: it plays
as 3.1c." in place of its parameters. A summary is broken into lines of at
most 45 characters, which fit the section in the game's small font; its
degree, middle dot, multiplication and plus-minus signs, which the game's
fonts lack, are written as " degrees", "*", "x" and "+/-".

Off, the list is static: it shows the profile as it ships, and the
overrides the settings keep are not laid over it. Its areas and hacks
still open and close and its filter still works, so its values can be
read, but no switch or slider of a hack takes a change, each fading as a
locked setting does, and Restore profile values takes no press; Show
performance statistics takes changes whatever Developer Mode is. On, the
list's controls take changes and the overrides apply. Every change goes
into `EngineSettings::hack_overrides`: one override for each hack whose
state differs from the profile's, holding the parameters set to values
the hack does not already have; a hack set back to the profile's state
drops its override. Turning a hack off drops the
parameters it set. Its changes show at once and Cancel puts them back, as
the other settings' do; OK keeps them. An override that does not fit its
hack as the resolver checks one (a parameter unknown or out of its bounds,
a constraint broken) is shown as the profile's state. Restore defaults
turns Developer Mode Off and keeps the overrides; only Restore profile
values clears them. The host keeps which areas and hacks are open and the
filter while the game runs (`DeveloperList`).

Developer's controls are numbered from the open section's first row's:
Enable Developer Mode 17, Show performance statistics 18, UI kit gallery
19, Show Active Only 20, Restore profile values 21, and the list's rows
that take input from 22, in the list's order. Every header takes the focus and a press, which
opens or closes it; a press on a hack's switch sets it. Space opens or
closes an area or a hack and flips a switch; Left and Right close and open
an area, turn a hack or a switch Off and On, and move a slider a stop.
Page Up, Page Down, Home and End scroll the list, as they scroll a long
section, the wheel over the dialog too; the two rows over it and its
footer stay where they are. Tab moves the focus through Enable Developer
Mode, Show performance statistics, the list's rows, Show Active Only and,
while Developer Mode is on, Restore profile values, then the footer's
buttons and the sections. Down from the list's last row reaches Show Active
Only, and Down from there OK, under it.

## Mod options

A mod whose profile turns ui.options-dialog on has its own options, which
the same dialog shows as a second kind (`DialogKind::mod_options`,
`open_mod_options_dialog`): its list holds five sections of its own and no
divider, and only `EngineSettings::mod_options` (`ModOptions`) change.

| Section | Setting | Range |
|---|---|---|
| Keys | Snap override key, Autoclick key, Rotate build key | one of `option_keys` |
| Patrolling | Hold position, Maneuver, Roam | Reclaim only, Both, Assist only |
| Guarding | Hold position, Maneuver, Roam | Stay, Normal, Scatter |
| Build tools | Optimize DT rows, Full rings | Off, On |
| | Mex snap radius | 0 to the mod's most cells (`ModOptions::mex_snap_most`, at most `most_snap_radius`) |
| Snap & chat | Wreck snap radius | 0 to the mod's most cells |
| | Accessible chat | Off, On |
| | Resource bar background | None, Text, Solid |

A snap radius the mod gives no room shows "Set by the mod"
(`Lock::set_by_mod`, `Locks::mex_snap` and `wreck_snap`). Restore defaults
there resets the mod options alone, and the engine's settings keep their
values. The application keeps the mod options with the player's other view
settings; this library neither reads nor writes them.

## Your files and the notice

System's first row, Your files (`Setting::user_folder`, a row of
buttons), changes no setting. Its first hint line shows the player's own
folder (`Dialog::user_folder`, which the host sets as the dialog opens) by
its tail that fits (`geometry::hint_is_path`, `path_tail`); its second says "Saved
games, screenshots, films and mods.", or in amber why the last folder asked
for could not be shown (`set_folder_notice`). Three buttons, SAVES,
SCREENSHOTS and MODS, stand right-aligned on its label line, drawn as
Cancel is. A click released on the button it pressed, or Space on the
button the keys mark, asks the host to show that folder
(`DialogAction::open_folder`, `Dialog::folder_to_open`) and leaves the
dialog open; Left and Right move the mark along the buttons.

Notices and prompts are the OA UI kit's notice and question
(`oa/ui/kit/components_more.hpp`) under their old names: `Notice`,
`NoticeParagraph`, `NoticeAction`, `Prompt`, `PromptButton`,
`PromptAction` and `PromptAnswer` are the kit's, and so are their sizes,
their buttons' numbers and the prompt's limits; `prompt_bar_control`, the
bar's part in `prompt_layout`, stays the dialog's. Each function here calls
the kit's, passing the dialog's look-up of the interface's words for a
notice. `notice_layout` and `prompt_layout` list the parts the kit places,
as the dialog's layout lists its own.

`notice.hpp` is a notice of Open Annihilation's own in the dialog's look,
which the main menu shows over itself: a raised panel 400 pixels wide, a
header with the icon (or the OA mark) and a title, its text in white in
the small font, wrapped between words, a folder's path in the
regular font broken after its separators, why its folder
could not be opened in amber, and a footer with a button that opens the
folder, drawn as Cancel is, and OK; it grows with its text from 150 to 440
pixels (`notice_height`, `notice_layout`, `draw_notice`). A click released
on the button it pressed presses it; a finger's press on neither button
takes the nearer one within reach, and its release where the finger landed
presses it (`notice_finger_down`); Enter and Escape close it; Space
presses the marked button, OK at first; Left, Right, Up, Down, Tab and
Shift+Tab move the mark (`notice_pointer_*`, `notice_key`).

`prompt.hpp` is a prompt in the notice's look that asks with one to three
buttons of its own captions, as a mod package's install does: its text,
given finished in the language shown and drawn as given, is placed as a
notice's (`kit::place_question`), and an optional progress
bar lies under it. Its buttons stand right-aligned in the footer, five
columns apart, each as wide as its caption at the estimated character
width and 16 more, at least 52, so that a press lands where a button is
drawn whatever the fonts; an accent button is drawn as OK, the others as
Cancel (`prompt_height`, `prompt_layout`, `prompt_fits`, `draw_prompt`).
Enter and Space answer the marked button, Escape and N its cancel button, Y
its primary one; Left, Up and Shift+Tab mark the one before, Right, Down
and Tab the one after; a finger's press takes the nearest button within
reach (`prompt_pointer_*`, `prompt_finger_down`, `prompt_key`).

`add_registry.hpp` is the Add registry window in the same look, over
Settings or the main menu. At Compact, Regular and Large it is that
class's notice wide and as tall as its words from the class's least notice
height to its greatest. A registry comes to it by its address
(`AddRegistry::Mode::by_url`), from an `.oareg` file (`from_file`) or as a
mirror of a registry the player added (`mirror`), and it stands in one of
four phases. The host makes `ProbeView`, plain strings, from what the
content service found, and the window shows:

- editing: the title Add registry (Add mirror and the heading "Mirror of
  {name}" for a mirror), the address field (`add-registry.url`,
  "http://…" while it is empty; a file shows "From {file}" in its place),
  and, for an address add only, a Signing key field under it
  (`add-registry.key-field`, "ed25519:…" while it is empty, at most 128
  bytes). A mirror and an `.oareg` file show no Signing key field. CANCEL
  and CHECK (`add-registry.cancel`, `add-registry.check`);
- checking: "Checking {address}…" and CANCEL, with both fields disabled;
- ready, adding a registry: its name in the heading role
  (`add-registry.name`), Address (`add-registry.address`), Packages
  ("{n} · {kinds}", the kinds as mods, maps and languages), Signing key or Signing keys
  (each fingerprint, at most three, then "+N more"; `add-registry.key`),
  or for a registry that names no key the amber "Unsigned · accepted in
  Developer mode only" in its place, Downloads (direct, with download
  keys, or with download keys · needs an install ID), the amber warning
  "Not reviewed by the OA team. Only add registries you trust. OA checks
  every package it downloads.", CANCEL and ADD (`add-registry.add`);
- ready, adding a mirror: Address, "Checked against the keys you trust for
  {name}" and those keys (at most three), CANCEL and ADD MIRROR
  (`add-registry.add`);
- refused: the refusal in amber (`add-registry.refusal`), the fields
  editable again with CANCEL and CHECK, or a file's "From {file}" with
  CANCEL alone.

The name, the address, the keys and the refusal are labels: areas neither
enabled nor focusable, their text what they show. `add-registry.key` stays
the fingerprint label. The focus starts on the address field while it
takes typing and on CANCEL otherwise, so that Enter or a pad's A never
trusts a registry first. Tab reaches the Signing key field after the
address, then the buttons. On either field Enter is CHECK, Space (a pad's
A) and a press start the system's text input
(`AddRegistryAction::start_typing`), and Backspace, Delete, Left, Right,
Home and End edit the focused field; Escape is CANCEL. Typed or pasted
text is taken up to its first line break. The address stays within
`oa::formats::url::max_url_bytes` and the signing key within 128 bytes,
each cut at a character's start. An empty signing key leaves the
descriptor's keys. A key the player typed replaces them, and the catalogue
must verify with that key alone. Editing a refused address or signing key
goes back to editing (`add_registry_list`, `add_registry_settle`,
`add_registry_field`, `add_registry_text`, `add_registry_key`,
`add_registry_pointer_*`, `add_registry_finger_down`, `draw_add_registry`).

`challenge.hpp` is the download check in the same look. At Compact, Regular
and Large it is that class's notice wide, 400, 520 or 600 points, and as
tall as its text from that class's least notice height to its greatest.
The title is Downloads and the heading is "Check that you're a person".
The text names the registry and the page, without the page's scheme or
query. The code sits centred in a framed field in the accent colour, in
the largest font, with two extra columns after each glyph but the last.
Under it the window says the check updates by itself, or, once the check
has run out, that it ran out. CANCEL is always there. OPEN THE CHECK is
there while the check waits and this computer can open a page; TRY AGAIN
is there once the check has run out. Focus starts on OPEN THE CHECK, or
TRY AGAIN, or CANCEL. Enter presses the marked button and Escape is
CANCEL; a pad's A and B arrive as those keys. A finger within the kit's
reach holds the nearest button (`challenge_text`, `challenge_height`,
`challenge_list`, `challenge_settle`, `challenge_key`,
`challenge_pointer_move`, `challenge_pointer_down`, `challenge_pointer_up`,
`challenge_finger_down`, `draw_challenge`). The parts automation lists are
`challenge.code`, `challenge.status`, `challenge.open`,
`challenge.try-again` and `challenge.cancel`.

## Dependencies

The dialog, its notices and its prompts draw through the OA UI kit
(`oa-ui-kit`): its colours, its Compact metrics and its text, and the
dialog's rows are the kit's declared rows (`oa/ui/kit/rows.hpp`), drawn
from its display list.

## Tests

`ui-engine-settings-challenge` checks the words for a check that waits, one
that has passed and one that has run out, with and without a page that can
be opened here, the buttons and the starting focus, Enter, Escape, Left
then Enter, a pointer release and a finger within reach, and that every
part, including the focus ring, lies inside the window at Compact, Regular
and Large, with each control named once.

`ui-engine-settings-registries` holds the Add registry window: its words,
fields and buttons in every mode and phase, the Signing key field present
and enabled for an address add, disabled while checking, and absent for a
file and a mirror, the keys over three and the unsigned line, the focus
starting on CANCEL once ready, Enter and Space on CANCEL never adding,
typing and a paste cut at its line break and at 2,048 bytes for the
address and 128 for the signing key, a refused address or signing key
going back to editing, the pointer, and every part inside the window at
every class. The registries list is the Library's (`ui-library` and
`ui-library-layout`). Downloads' link, on the main menu, is
`settings.registries-in-library` and opens the Library on Registries
(`ui-engine-settings-downloads`).

`ui-engine-settings-rows` holds every setting's row, written out as
literals, to the table: its kind, label, hint lines without and with the
Steam Input notice and a Steam Deck's rate, stops, levels or choices, a
strip's level width or a drop-down's field width, the levels a strip
offers, whether its hints are its status, its lock while a game is in
progress, and the field of the locks that locks it. Every row's id is one
word, none twice in a section, and every drop-down's item and strip's
level has an id of one word, none twice in its row, that is the word the
preferences keep once the row is set there (`write_settings`). On every section of
every kind of dialog (the engine's settings with and without Touch,
Controller and Game files, with and without the Steam Input notice and a
Steam Deck's rate, a mod's options and Language alone), at its top and its
scroll end, unlocked and under a game's locks, it finds every control of
the display list named once, every control `dialog_layout` lists in the
list where the layout has it, and Tab stopping only on the list's
focusable, enabled controls, each once; and
Developer with every hack open, an open drop-down's items, and the Tab
orders of Controls, Graphics and Developer with an area open as literals.

`ui-engine-settings-downloads` holds the Downloads section: its place before
Developer for every combination of Touch, Controller and Game files, its
rows for none, one and three registries with the Registries row last
(three scroll at Compact), the
labels and hints filled with the registries' names, "Made at your first
download" as the switch's second hint while no install ID exists and a
labelled "Your install ID" row once one does, an install ID shown as
its first 8 and last 4 hex digits, each state's value text, RESET and EMPTY
disabled when they cannot act, those two locked during a game while the
check and the switches stay editable, Cancel putting the switches and the
check back after RESET and EMPTY have already asked the host, Restore
defaults, every part inside the dialog at Compact, Regular and Large, and
the controls' names unique.

`ui-engine-settings-dialog` checks the names the automation endpoint lists
for the dialog, `oa.` before each name and part `kit::automation_parts`
gives, on every section of the engine's settings with Touch, Controller and
Game files listed and every hack open, with a drop-down open on Controls,
and on every section of a mod's options: the
kit finds no fault in the names, each matches `^oa(\.[a-z0-9-]+)+$`, is
at most 120 bytes and like no other ignoring case, and the names a journey
presses (`oa.settings.wheel-zoom.off`, `oa.settings.max-zoom-out.whole-map`,
`oa.settings.user-folder.saves`, the footer's and others) are among them.

`ui-engine-settings-pixels` and `ui-engine-settings-pixels-data` hold the
dialog, its notices, its prompts and the OA button to the pictures they
drew when the kit was taken out of the dialog: a SHA-256 of each scene,
with a stand-in font and with the installed game's fonts. The Developer
scenes (at its top, with Developer Mode on and its first area open, and at
scale 2) were recorded again when UI kit gallery joined Developer's rows,
which moved its list down; no other scene changed. The Graphics scenes
(at its top, locked, and at its end) were recorded again when Fog of War
joined Graphics' rows, which shortened the scroll bar's thumb and put the
row between Explosion flash and Zoomed out units, and again when the
camera's switches and the effect sliders joined them, which shortened the
thumb further and put the water sliders at the end, and again when New
unit shadows joined them at the end; no other scene changed. The Controls
scenes were recorded again when Group keys centre the view joined its rows,
and again when Double-click selects joined them under Middle-click pans,
which shortened the scroll bar's thumb; no other scene changed.

`ui-engine-settings-dialog` also holds UI kit gallery's row: its label,
its hint in two lines at Compact and one at Regular and Large, and SHOW,
which asks for the gallery by a press and by Space and changes no
setting; Developer's list exactly the row's height lower than with two
rows at every class; and every other section's Compact parts, as a digest
of each, as they were before the row was added, a digest that differs
printed so that a change meant to move them can pin them again.

`ui-engine-settings-prompt` lays out prompts of one, two and three buttons,
their keys, a pointer's press and release and a finger's within reach, the
progress bar's part and a text found cut, and at each size class a notice
and a three-button prompt within the class's width and heights, their
buttons at Compact's sizes and the notice's text in fewer lines at the
wider classes. `ui-engine-settings` covers the defaults, a Raspberry Pi's and a light
machine's included, the keys read and written, Hardware acceleration's
words and the numbers its switch once wrote, words that are no number, and
the locks of a game, the flags and the renderer, and the Language
switches and text size: their defaults, 80% among them, a file without
them, a file with CR LF line ends, the mod and the picked folder stored as
their paths, the picked folder kept while it is the mod and through
Restore defaults, forgotten when another mod or No Mod is chosen and when
its key names a folder that is not the mod, a mod folder not offered read
as the picked one, and files without the keys; the round trip, the
size's range and
clamping, Restore defaults and the text style they make; the language:
its defaults, a file without it, the values read, a tag the game does not
know kept in the file, the round trip, Restore defaults and the command
line's lock; Developer Mode's switch and overrides: a file without
them, the overrides kept under each profile's id and read back, erased
when none are left, and kept by Restore defaults; and the Touch section's
keys, its defaults on every machine, its words read and others dropped, the
hold delay held to its range and put on its nearest step, and its round
trip and Restore defaults; the backups switch: Off by default on every
machine, read as every switch, written alone and erased by Restore
defaults; a Steam Deck's defaults at 60 and 90 frames a second with
Control size Larger, nothing else moved, a named file without them, a
screen's rate held to the range and put on a step, a stored value winning
and Restore defaults bringing them back; and Control size and the
Controller section: their keys, defaults on every machine, words read and
others dropped, the speeds held to their ranges and steps, and the round
trip and Restore defaults; and Interface size: its key, Auto by default
with and without the player's own file, its six words each read and
written alone, `150` with the modern fonts On, and as 100% read with them
Off from the key or from a named file's default, the words it refuses
(`0` among them) read as Auto, other keys left as they were, and Restore
defaults; and the eight file-type switches: on by default, only `off`
turning one off, the launch file's switch stored alone as `off` and as `on`
with the other seven keys untouched, and erased by Restore defaults;
`ui-engine-settings-file-types` that each file-type switch stands for its
own type alone, in the set a registration acts on, the set a withdrawal
acts on and the opener change it asks for; that the launch file's switch
turned off and on again, as OK applies it, registers and withdraws launch
files alone: elsewhere than on Windows in a scratch XDG data folder (the
launch file's desktop entry, icon and part of the MIME package go and come
back, and no file of the other seven, nor the program's icon, is written or
removed), and on Windows under a scratch key of `HKEY_CURRENT_USER` (the
launch file's six values go and come back, the other seven's 42 never
written or removed); and that on macOS, against a stand-in for the
system's questions, it hands launch files alone back to the opener of
plain text and takes them back, the switches then show what the system
reports, and a launch file the system kept is named in the hint's list;
`ui-engine-settings-dialog` the dialog's layout (every part inside the
panel and none overlapping), and at Regular and Large every section with
Touch, Controller and Game files listed inside the dialog under every lock,
its parts apart, each section's entry fitting its words, each control
pressed where it is drawn, hints kept to their lines, Graphics showing more
whole rows than at Compact and scrolling less, and the footer's buttons at
their Compact sizes and in their order; its sections, switches and their one table,
slider stops, both level strips, keys, footer buttons, locks and the faces
it draws; the arrows by geometry (`the_arrows_move_by_geometry`: down a
section's rows to the footer and back, from a section's entry to the row
at its height, from Manage… to the entry at its height and back, from Developer's entry
to Restore defaults, down Graphics past its view, from no focus, and Tab's
order on every section); the OA button's icon at its size in both buttons, at rest, under
the pointer, held and twice as large, and the mark without it; with the
installed game, the header's icon in its place and the mark without it;
the sections' order and names, Developer at the foot under the divider,
and System holding Your files, then File Type Registration's eight
switches, Launch files last, where the game registers file types at run
time or the File types text row
where the app declares them, and Engine Limits holding the unit limit, pathfinding and the skirmish
player count;
Language's drop-down, four switches and Text size slider,
their place in the list, their texts, clicks, drags and keys on them, the
text style they set, the slider's 26 stops from 50% to 300%, and its lock
while the modern fonts are Off, lifted as they turn On; the Language
drop-down's choices, each language named in itself, its field and list,
the pointer and the keys on them, a press off the list, the command line's
lock, and lists that scroll or open over their field;
the Graphics section's twenty-two rows, New unit shadows last, their places at every offset
and under every lock, the focus scrolling them into view, both forms of a
locked row, Smooth motion's and New unit shadows' switches and their two hints each, Zoomed out units' faded Icons and After zoom's lock, every status of Hardware acceleration, Full's included, and
the requests to try the graphics card afresh; User Interface's seven rows, Menu scaling, Window frame, HUD scaling, Interface size, Window size, Reset window positions and Escape opens the game menu, Window frame's two ways and HUD scaling's two hints; Accessibility's fifteen rows, the captions at both ends of each range, Choose… opening the key editor on Latchable while Edit keys… stays on All, and each change applying at once; Interface size's drop-down
offering 150% only with the modern fonts shown On, and turning them Off
taking 150% to 100%; Graphics drawn on the kit canvas's smooth form at 1.5×
with every pixel painted whole; on sections of the test's own taller than the
view (`SectionHooks`), its scrolling: the view and its limit, the wheel, the
scroll bar, the scroll keys, the focus brought into view, rows the view
cuts and the control numbers; and the mod options' sections, stops,
Restore defaults and locks; Developer: its two rows and their places over
the list, Show performance statistics taking changes whatever Developer
Mode is, the list's areas closed at first and opening, the list static
while Off, a hack turned on and off, a switch, a slider and a set's value
set through the pointer and the keys, the overrides kept and dropped as
the profile's own values come back, Restore profile values, Show Active
Only and its count, the list's scrolling and focus, and its summaries'
lines; Controller: listed only once a gamepad has sent input, after
User Interface and Touch and before Game files, each entry's number kept, the
focus walking it, `set_controller_section` listing and unlisting it, its
fifteen rows with their strips, sliders (26 and 36 stops), switches and
drop-downs by pointer and keys, Control size, Hold delay and QUEUE and ADD
as Touch's own settings, Restore defaults (a Steam Deck's frame rate and
Control size Larger included) and Cancel, the Steam Input notice's exact
text in amber taking no press or focus, the focus kept on its row as the
notice comes and goes, the dialog with Controller drawn alike but for the
list from its entry down and without it as ever, and Maximum frame rate's
Steam Deck line; Touch: listed only with touch controls, at its place in the list
with the line and Developer under it, the list without it where it always
was, each entry's number the same with and without it, the focus walking
it, its rows, their texts, strips, slider stops and switches, Restore
defaults and Cancel, the dialog with it drawn alike but for the list from
its entry down, and a finger's press taking the nearest control within
reach (15 points off a switch takes it, 30 points off nothing, a mouse
press 15 points off nothing) at several scales, a slider dragged from under
its track and an open list's nearest item; Game files: listed only with
its flag, between Touch or User Interface and Downloads, the line and
Developer under it, its entry numbered 8 with Touch 5, Controller 6,
Developer 7, Downloads 9, System 10, Game Content 11 and Updates 12, the
focus walking it, its three rows and their texts, Manage… returning
`manage_game_files` to a click, to Space and to a finger beside it, the
switch by either half and the keys, Restore defaults and Cancel, where the
files are taking no press and no focus, and the lines a location breaks
into; the Language dialog: its one entry at the top, its focus,
Restore defaults restoring its section alone and keeping a locked
language, and with fonts that hold no glyphs every text drawn and measured
in scripted modern fonts; Your files: its place, its buttons under the
pointer and the keys, its amber line and a long folder's tail; the notice:
its text wrapped and its path broken at its separators, its parts kept
apart, its buttons, keys and colours; and
`ui-engine-settings-dialog-data` its fonts, and every text
fitting its place in them, a scrolled section's at every offset included,
Touch's with each way of its strips, Controller's with each way of its
strips and drop-downs, with and without the notice and with its lists
open, Graphics on a Steam Deck, every status line in the 309 columns
of a hint, the Game files section with a host's usual texts and the
Language dialog, Your files with a long folder and
each reason a folder could not be shown, the notice with a long path and a
failure, and every row of
Developer Mode's list with every area and hack open and every hack on,
over the installed game. `native-engine-settings` sends the wheel and the
scroll keys through the main menu's and the match's dialog, turns Vertical
sync On in the dialog and reads it back from the renderer, and sets
Hardware acceleration to Full and to Basic in a shared game, where each
waits for the game's end. It also finds Text size locked with the modern
fonts Off, turns them On and raises the size a stop, which the text style
reads at once and OK saves, and chooses Deutsch from the Language
drop-down, which puts German in effect at once, and steps back to English.
On a 640 by 480 window it shows System at its top and its end in English
and German, and finds File Type Registration and its eight switches, Launch
files among them, drawn with no text wider than its box or across another.
With touch controls on it finds Touch listed between User Interface and
Developer with its rows at their defaults, and a finger's press beside
Haptics' switch taking it where a mouse press there does nothing; in the
match, it checks that on a phone the dialog and the OA button fit the
safe area, and on each window of the in-match table under
[Sizes](#sizes) that the dialog takes
its class and scale, its class's points at the scale centred right of the
column, is drawn there pixel for pixel as the dialog drawn offscreen at
them, and shows a section a click on its entry's middle asks for.
It finds no Mods section, and the mods in the application's Library: no
mod played, and a folder stored by an earlier version's Pick Folder...
listed by its oamod.yaml's name with Play now; --base-game greys Play now
with its note and Play now then changes nothing; on that folder played,
Switch off asks the Switch Mod question, which Escape and CANCEL put away
with the Mod unchanged, and SWITCH erases its keys, after which the next
start no longer lists it, and a start erases a picked folder's key that
names a folder other than the mod stored; a stored mod folder that is gone
reads as no mod played, and OK replaces it; and a start that plays a
folder without a profile keeps the overrides under the folder's own id.
Over a match it finds no Mods section, the Library closed, Play now and
Switch off greyed and asking nothing, and the keys never moving the focus
onto a row the game locks. `native-mod-switch` switches the mod ten times
through the Library, each a soft restart.

## Limitations

The kit's painter does not draw a text item's tracking, so the code is
drawn after the rest of the window, with the tracking the item names.

The arrows' scoring rule is the kit's (`kit::focus_toward`), to be
revisited after controller playtests (D30). Because a row's control lies
across its row, Down from a section's last row reaches Cancel even under a
switch in OK's column, and Down from Show Active Only reaches OK, passing
Restore profile values, which lies off to its left. Each pointer event and
arrow builds the display list afresh to read its controls; with every area
and hack of Developer's list open, a pointer move costs about a quarter of
a millisecond. A row's control rectangle spans its row's line, and only
the part within its clip, the control itself, takes a press; automation
that presses a row by its rectangle must press that part. The names are the
dialog's own; listing them to automation, with
`oa.` before each and the parts of a switch, a strip, an open drop-down and
Your files' buttons, is U11's, which also gives a drop-down's items ids of
their own in place of their numbers.

The practice pad answers keys only where the host hands them over
(`dialog_practice_key`, `dialog_practice_tick`); the OA layer's settings
screen does not yet, so on screen the pad shows its words and Turn every
helper off works, but a key pressed there does nothing. The rows' buttons
read Choose…, Manage…, Show, Reset and the like, in title case.

## The missing-language notice

`include/oa/ui/engine_settings/language_notice.hpp` and
`src/language_notice.cpp` lay out the notice shown at the start when a
language is only available as a download. The screen is named
`language-notice`. Its buttons are `language-notice.use-english` and
`language-notice.download`. Automation lists them as
`oa.language-notice.use-english` and `oa.language-notice.download`, and the
dialog as `oa.language-notice`. Use English, when the operating system
asked for the language, writes that language's tag into
`open-annihilation.language-notice-dismissed`.

## The manual's pictures

`oa-ui-engine-settings-pictures DIRECTORY` draws each section the engine's
dialog lists when Touch, Controller and Game files are on: the dialog
alone, unlocked, at Compact and 2× (960 by 648), in the installed game's
fonts (`OA_GAME_DIR`), or with `--modern FONTS` in the modern look, Barlow
from the fonts folder named (`local/deps/text-fonts`, or the build's
staged fonts), Look showing Modern. The manual's pictures are the modern
look's. The header shows the shipped app icon, the plates,
cropped to the pixels that show (`branding/open-annihilation-256.png`). It writes one PNG a section, under the names
`docs/images/settings/` uses (`controls-1.png`, `language-1.png`,
`graphics-1.png`, `downloads-1.png`, `system-1.png`, `updates-1.png`,
`accessibility-1.png`, and
`page_word` plus `.png` for the others), and `controls-2.png`,
`downloads-2.png`, `system-2.png`, `accessibility-2.png` with those sections scrolled to their
ends, `updates-2.png` with a release offered on the card, and
`updates-3.png` scrolled to Previous version. The
rows a host fills (Your files, Game files, Downloads' install ID, Game
Content) are the defaults the manual describes. Updates is the card the
game fills: Installed 0.8.7 and Up to date with Check now, then 0.8.8 is
out with Download update and Skip this version, and at the end Previous
version · 0.8.6 with Roll back…. The data test
`ui-engine-settings-pictures` runs the same program into a scratch folder
under the build. To refresh the manual, build the tests, draw the
pictures into a scratch folder and copy them into `docs/images/settings`:

```sh
oa-ui-engine-settings-pictures pictures --modern local/deps/text-fonts
```
