# Boneyards

The lobby built into the game: the main-menu overlay, its consoles and
screens, the client that speaks to the lobby, the match report and the
launch check. One option, `OA_BONEYARDS`, builds it. On Windows 95 it is
off. The extension is registered last, so it is asked first for `-c` and
`-y`. It takes `-y`. It takes `-c` only when that names the online module
(`conline.cfg`, and a name of that shape); a launch file's `-c` — a path,
with or without spaces, an `.oalaunch` file, a `.ini` settings file, or a
launch read from standard input — is left for network play.

`cmake/OaBoneyards.cmake` builds the libraries below and `coreprime.ccx`,
copies the committed analogue atlas
(`data/boneyards/fonts/boneyards-analogues.bin`) into the game's files, then
builds the extension library `oa-boneyards-extension`. That
library is the only one the game links. The module's tests link the
libraries they exercise.

| Directory | Library | What it is |
|---|---|---|
| `extension/` | `oa-boneyards-extension` | The extension: switches, launch, the exit box, the menu check |
| `checks/` | `oa-boneyards-checks` | The launch check and the chat check. Not compiled into the extension |
| `client/` | `oa-boneyards-client` | The session the consoles drive |
| `console/` | `oa-boneyards-console` | The overlay, its consoles and the main-menu strip |
| `screens/` | `oa-boneyards-screens` | The screen models the consoles draw |
| `html/` | `oa-boneyards-html` | The markup pages and their font chain |
| `toolkit/` | `oa-boneyards-toolkit` | Windows, rows, edits and the bitmap faces |
| `platform/` | `oa-boneyards-platform-fonts` | The page faces that stand in for the Windows fonts |
| `rover/` | `oa-boneyards-rover` | The lobby client. See [rover/README.md](rover/README.md) |
| `reporter/` | `oa-boneyards-reporter` | The match report posted after a launched battle |
| `tests/` | `oa-boneyards-tests-installs` | Paths the tests use for the installed game |

## Entry points

`oa_extension_init_boneyards` registers the extension. `-y` opens the overlay
at the main menu's first update. `-c` with an online module clears the launch
block; a launch file's `-c` is left for network play. The consoles
call Rover through `oa/rover/interface.h`.

## What the player sees

With the module built, the main menu can open the overlay, and `-y` opens it
at the first update. The consoles, the pages, the chat and the launch check
behave as they did in the project this was copied from. When the boneyards
expansion is installed and turned on, the screens read `by.ccx` and the
`Boneyards/` files from `Expansions/totala/boneyards/` ahead of the game
folder. A game folder that already holds those files keeps working when the
expansion is off. When neither place has them, the main menu says what is
missing and the game goes on. The files the Boneyards keeps for the player,
the saved server list, the fetch log and the people lists, live in
`Boneyards/` inside the player's folder, beside Mods, Maps and Saves. A
`boneyards/` folder left there by an earlier build is still read, and the
next save renames it to `Boneyards` where the file system treats the two
names as different folders. QuickPlay and the Locator are open to every
account. Matchmaking and the in-game registration form stay behind an Admin
or Beta Tester account (`OA_BONEYARDS_*_RELEASED`, both off). The registration
address drawn in `coreprime.ccx` is `https://coreprime.net/register`. A wrong
password on the login status names `coreprime.net/forgot`, and the Login
help page says the same.

Chinese, Japanese and Korean on the markup pages come from the engine's text
faces in the game's fonts folder: the endonym face, and a language pack's
faces when that stack holds them. The collection the atlas is drawn from is
not shipped; the atlas itself is the committed file the build copies. A line
those faces do not hold draws nothing on a page. The
chat log, with modern fonts on, uses the engine's modern faces.

## State

The extension keeps the launch, the exit box and the font library for the
pages. The client keeps the session. Rover keeps its own links and records;
the consoles reach it through the table in `rover/include/oa/rover/interface.h`.
Nothing here changes a byte the game saves, sends or records while the
overlay is not in use.

## Tests

ctest names match the project's names, except a name that contained the word
`test` as its own word, which the engine's name check rejects. Those names
drop that word. `boneyards-font-atlas` redraws the committed atlas with
`tools/boneyards/build_font_atlases.py --check` and skips when the fonts or
a host compiler are missing. `native-boneyards-*` and the Rover replay tests are
registered and are not run on this machine. Tests that need the installed
game read `OA_GAME_DIR`. Launch checks read `OA_LAUNCHER_GAME_DIR`.

## Limitations

The 32-bit drop-in library is not built. The
module does not fill extension-table hooks added after version 14. A page's
font stack is opened from the fonts folder and is not the stack the engine's
own screens add a language pack to.
