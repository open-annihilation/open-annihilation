# Console

The overlay: the main menu strip, the consoles, and the pictures they draw.

## Entry points

`register_boneyards_console_screens` and `oa/boneyards/console/status_widget.hpp`.
The library is `oa-boneyards-console`. Sources under `src/` that are not in
the public include tree are visible to this module through
`oa-boneyards-console-private`.

## State

Reads the screen services, the client session and the page fonts. Writes the
overlay's windows and the launch bridge it was given. Closing the overlay
returns the player to the main menu. The banner strip shows the ad server's
list when one was sent, and the local sample frames otherwise.

## Invariants

`-y` enters without a key. The exit box asks "Exit to the system?". QuickPlay,
Locator, Matchmaking and the registration form stay behind an Admin or Beta
Tester account while their release options are off.

## Typing

A console or pane that opens to ask for text gives that field the keyboard.
A later refresh does not move the keyboard once the player has left the
field. Return runs the button the layout names as its default (`crdefault`).

| Screen | Keyboard |
| --- | --- |
| Chat console | the entry line, when the console opens |
| Locator, player, planet, battle and frequency faces | the name field, when that face opens |
| Locator, Server Transfer | no field |
| Create Frequency | the frequency name, when the pane opens |
| Change Frequency | the password, only when that frequency needs one |
| Host pane | the game name, when the pane opens |
| Battle Intelligence of a forming battle | the password, only when that battle needs one |
| Enlistment form | User, its first field. Return runs Register |
| Registration unavailable | Return runs Ok |
| Login | the name, or the password when a name is saved |
| Profile, Account | no field. CHANGE focuses the password |
| Contents, Ladders search | no field; the console opens on a page |
| QuickPlay, Recon, Help, News, Scoreboard, the player pane, the other Profile tabs | no field that typing is for |

Matchmaking's layout names Activate, and the dialog does not keep that
screen's keys, so Return is not wired there.

## Tests

`boneyards-console-*`, including the navigation, registration and gamepad
checks. Several read `coreprime.ccx` and the language catalogue.
`boneyards-console-quickplay`, `boneyards-console-matchmaking` and
`boneyards-console-locator` open their consoles as on a profile that does not
mount the mod, so the layouts and the new art come through the client
session's reader. Run by hand with `--frames DIR` after the mod folder, the
QuickPlay check writes its idle face and its searching face as
`quickplay-idle.png` and `quickplay-searching.png`; the Locator check writes
one face for each search, `locator-player.png`, `locator-planet.png`,
`locator-battle.png`, `locator-frequency.png` and `locator-server.png`; the
registration checks write `registration-web.png`, `registration-form.png`,
`registration-sending.png`, `registration-unavailable.png` and
`registration-registered.png`; all to compare with the design.
`boneyards-console-help-pages` opens every Help page of the install from
the Help console, as a player does, in the bundled page faces. Run by hand
with `--frames DIR`, it writes each page as `<page>-frame.png`, the console
as the player first sees it, and `<page>-page.png`, the whole page; with
`--pages DIR` it shows and writes every `.htm` file of that folder too, such
as News and ladder pages saved from the content server.

## Limitations

The analogue atlas is the committed file
`data/boneyards/fonts/boneyards-analogues.bin`, copied into the game's fonts
folder. A missing atlas keeps the faces' own glyphs.
