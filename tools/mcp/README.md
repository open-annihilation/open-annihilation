# tools/mcp

An [MCP](https://modelcontextprotocol.io) server, `oa_mcp_server.py`, that
lets an MCP client (Claude Code, Claude Desktop or another) start the game,
watch it and drive it through the
[automation endpoint](../../docs/automation.md). The server speaks MCP over
its standard input and output and the automation protocol to the game. It
needs Python 3.8 or later and nothing beyond its standard library. It is a
program beside the game, not part of it: the game is unchanged, and the
endpoint is on only in a game the server starts with `--fark`, or one
started so by hand.

## Contents

- [Registering it](#registering-it)
- [The tools](#the-tools)
- [A session](#a-session)
- [Which game it starts](#which-game-it-starts)
- [What it reads, writes and starts](#what-it-reads-writes-and-starts)
- [Checks](#checks)

## Registering it

Register it with the client yourself, for the checkout you trust:

```
claude mcp add open-annihilation -- python3 /path/to/open-annihilation/tools/mcp/oa_mcp_server.py
```

The repository holds no `.mcp.json`. A project file would have every
Claude Code session opened in the checkout offer to run whatever the
script holds in that tree, on whichever branch is checked out; registered
by hand, it runs the script you chose. On Windows, where `python3` may be
missing, use `python` or `py -3` instead. Another client takes the same
command in its own configuration, such as `mcpServers` in Claude Desktop's
`claude_desktop_config.json`.

The server's own options follow the script's path:

| Option | What it does |
|---|---|
| `--executable PATH` | the game `game_launch` starts: `open-annihilation`, `open-annihilation.exe` or a `.app` bundle |
| `--preferences-dir DIR` | a folder whose preferences files `game_launch`'s `preferences_file` may name; again for more |
| `--runner COMMAND` | a command the game is started through, such as the compatibility layer a cross build's tests run under; name the executable with `--executable` then |

## The tools

| Tool | What it does |
|---|---|
| `game_launch` | starts the game with `--fark` and connects ([which game](#which-game-it-starts)); `headless` runs it on SDL's dummy drivers, with no window and no sound; `arguments` adds options such as `--host`, `--join ADDRESS` or `--resolution 1280x720` |
| `game_attach` | connects to a game already started with `--fark --fark-file FILE`; refused while a game launched here runs |
| `game_status` | whether a game is connected and running, its executable, the end of its log, the server's scratch folder and the executables `game_launch` may start |
| `game_quit` | quits as the main menu's Exit does, leaving a match first; `force` kills a game launched here |
| `screen` | the screen shown, the dialogs over it, the focused control, the pointer and the camera |
| `controls` | the controls that take the pointer, topmost first, with their names, kinds, places on the 640x480 canvas, state, text and a list's rows; `dialog` and `name_contains` filter them |
| `screenshot` | a PNG picture of the frame presented (or composed), whole or a region, shrunk by whole steps to at most `max_width` |
| `frame_hash` | the frame's FNV-1a hash and mean colour, to compare frames without a picture |
| `click_control` | clicks a control at its centre, by its name or its text; refused when another screen shows by the time the click arrives |
| `select_list_item` | chooses a list's row by its text or its key, scrolling the list with its arrows until the row shows |
| `click`, `drag`, `move_pointer`, `scroll` | the pointer and the wheel at a point of the canvas or the window |
| `press_key` | a key or a chord (`escape`, `return`, `f2`, `ctrl+s`), `repeat` times; `hold_frames` keeps it held, as for the arrow keys that scroll the camera |
| `type_text` | types into the field that takes text; `submit` presses Return after it |
| `send_input` | any of the endpoint's input events as they are |
| `wait_for` | waits until a screen, a dialog or a control shows, or with `gone` until it no longer does |
| `wait_frames` | lets the game run for a number of frames |
| `prefs`, `match`, `room` | the preferences, the running match (with `state_hash`, the digest a saved game carries) and the battle room |
| `subscribe`, `read_events` | chooses the kinds of event kept, and takes those that came |
| `request` | any automation request by its `op` and `fields`; the server sets the request's `id` and `op` itself |

Every answer of the endpoint is passed on as JSON text, without the
request's `id`. A refusal is a tool error naming the endpoint's error code,
such as `screen_changed`. Arguments are checked against each tool's schema
first, type and range: a wait is at most 600 seconds, and `press_key` holds
keys for at most 900 frames in all. A call that does not fit, or a tool
that fails, is answered as a tool error, and the server and the game go on.

## A session

A test asked of the client in words, such as "start the game headless,
open Skirmish and tell me which maps it lists", becomes calls like these:

1. `game_launch` with `headless` true, then `wait_for` `main_menu`;
2. `click_control` `SINGLE`, then `click_control` `Skirmish`, then
   `wait_for` `skirmish`;
3. `controls` with `name_contains` naming the map list, or `screenshot`
   to see the screen;
4. `game_quit`.

The controls' names are the ones the game's GUI files give, and
`oa.`-names for Open Annihilation's own screens
([docs/automation.md](../../docs/automation.md#oas-own-screens)).

## Which game it starts

`game_launch` starts only Open Annihilation's own executable, named
`open-annihilation` or `open-annihilation.exe` (in a macOS bundle, the
binary in `Contents/MacOS`). It is the server's `--executable`, else the one
`OA_EXECUTABLE` names, else the newest of this system's executable under the
repository's `build*` folders: `open-annihilation.exe` on Windows,
`open-annihilation` (or a bundle's) elsewhere. Its `executable` argument may
choose another of those, as `game_status` lists them, and no other file. It
answers the path and when that file was built. It never builds: run
[run.sh](../../run.sh), or build the `oa-game` target, first, so that the
game is the current source's.

The game is started with `--fark`, an endpoint file in the server's scratch
folder, and `--skip-intro` unless `skip_intro` is false. Its preferences
file is a new one in the scratch folder, holding no setting, so that the
player's own settings stay untouched; `preferences_file` may name an
existing file instead, in the scratch folder or in a folder the server's
`--preferences-dir` names. The installation is `game_dir`, else
`OA_GAME_DIR`, either a folder that holds `totala1.hpi`; without either the
game uses the folder chosen before.

`arguments` takes these options and refuses every other, `--fark` and its
options, `--preferences-file` and `--game-dir` among them (`--headless-check`,
the `--check-*` options and `--stage` cannot go with `--fark`, so they are
not taken either). A file or folder an option names must lie in the
server's scratch folder, a relative one read from the game's own folder
there.

| Options | What they take |
|---|---|
| `--skip-intro`, `--mute`, `--first-run`, `--touch-controls`, `--native-density`, `--experimental-3d`, `--no-hardware-acceleration`, `--base-game`, `--no-mutators`, `--trace-input`, `--debug-order-lines`, `--status-stdout`, `--host` | nothing |
| `--hardware-acceleration`, `--quit-after-game`, `--tournament`, `--closed` | nothing, or `=` and a word: `off`, `basic`, `full`; `after-score`; `on`, `off` |
| `--resolution`, `--max-fps`, `--content`, `--seed`, `--join`, `--player-name`, `--game-name`, `--game-password`, `--map`, `--side`, `--team`, `--colour`, `--player-limit` | a value, as the next argument |
| `--status-file`, `--user-folder`, `--data-dir`, `--mod-dir`, `--open`, `--install-mod` | a path in the scratch folder, as the next argument |

## What it reads, writes and starts

- **It starts** the game alone, as above, through `--runner` when the
  server was given one.
- **It reads** the endpoint file, the game's log, the listing of the
  `build*` folders and of `game_dir`, and nothing else. `game_attach` takes
  only an endpoint file as the game writes it: a JSON object under 4 KiB
  with the protocol's version, a loopback address, a token of 32
  hexadecimal digits and a process id, owned by the player and writable by
  no one else; a refusal quotes none of the file.
- **It writes** its scratch folder, `oa-mcp-*` in the system's temporary
  folder, which `game_status` names: the endpoint file, the game's log and
  the new preferences file of each game it starts. The folder is removed
  when the server ends. A game the server started is stopped then too; one
  it attached to is left running.
- **It connects** only to the endpoint on the loopback address, 127.0.0.1,
  `::1` or `localhost` (reached as 127.0.0.1), as the game serves no other.

The game it drives does whatever its input does, as a player at the
keyboard would. Clicks and keys can change and save the settings (in the
preferences file the game was given), save games, install, update and
uninstall packages and fetch them from the registries, change the
registries, and write to the player's own Open Annihilation folder and the
game's data folder, as the game does for a player. With `--host` or
`--join`, or from the multiplayer menus, the game plays over the network,
and typed text and chat in a battle room reach the other players, who are
also shown that the game is driven
([In a battle room](../../docs/automation.md#in-a-battle-room)). The
endpoint itself never reads or writes files, starts programs or reaches the
network for a client
([what it never does](../../docs/automation.md#what-it-never-does)).

## Checks

| ctest | What it checks |
|---|---|
| `mcp-server-selftest` | `oa_mcp_server.py --self-test`: the MCP requests, every kind of tool against a stand-in endpoint on the loopback address, and each refusal: an executable of another name or not found by the server, the options `arguments` does not take and paths outside the scratch folder, a game folder without the game, a preferences file outside the allowed folders, endpoint files the game would not write (another address, a wrong token, too large, not JSON, writable by others), `game_attach` while a launched game runs, `request` fields naming `id` or `op`, and arguments of the wrong type or out of range; a launch through a stand-in game, with its new preferences file; and a failing tool answered as its error |
| `native-mcp-server` | `tools/check_native_mcp_server.py`: the server started as a client starts it, with `--executable`, drives the built game on the dummy drivers with a new preferences file, from the main menu to Single Player and back, and quits it |
