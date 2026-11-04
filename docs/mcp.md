# Driving the game with MCP

The game speaks the [Model Context Protocol](https://modelcontextprotocol.io)
when it is started with `--mcp` or `--mcp-http`, so a program on this
machine can read the screen, drive it, and in a match read units, the map
and the view and give orders. It is meant for an agent that tests the game
and, later, for a computer player. A player who is not using an agent never
starts it.

While a client is connected the window shows a badge at the top right,
`MCP · realtime` or `MCP · stepped`. With no client connected the badge is
not drawn, and a game that was not started with either option is unchanged.

The field-by-field limits of each tool are in
[src/app/mcp](../src/app/mcp/README.md). The command line is also listed in
[command-line.md](command-line.md#status). An engine that does not yet
speak the protocol is driven through
[tools/mcp](../tools/mcp/README.md) and the
[automation endpoint](automation.md).

## Contents

- [Starting it](#starting-it)
- [The badge](#the-badge)
- [Claude Code](#claude-code)
- [Claude Desktop](#claude-desktop)
- [Speaking the protocol](#speaking-the-protocol)
- [Over HTTP](#over-http)
- [Tools](#tools)
- [Resources](#resources)
- [Events](#events)
- [Rules](#rules)
- [An example](#an-example)

## Starting it

| Option | What it does |
|---|---|
| `--mcp` | Speaks the protocol on standard input and standard output. The game's log stays in the log folder, so those two streams carry only the protocol. |
| `--mcp=observe` | The same, listing and accepting only the tools that read. |
| `--mcp-http [127.0.0.1:PORT\|[::1]:PORT]` | Serves the same protocol at `http://127.0.0.1:PORT/mcp` or `http://[::1]:PORT/mcp`. With no address the game picks a port on 127.0.0.1. |
| `--mcp-clock stepped\|realtime` | With `--mcp` or `--mcp-http`, `stepped` holds a single-player match until `clock.step` or `clock.run` asks it to tick. `realtime` is the wall clock, which is also the clock when the option is left out. |

Only the command line turns it on. No setting, environment variable or
file does.

- `--mcp` and `--mcp-http` may both be on. They share one queue of changes,
  one clock and one list of events, and each has its own session.
- `--mcp-clock` needs `--mcp` or `--mcp-http`. A value other than `stepped`
  or `realtime` is refused. `--mcp` with a value other than `observe` is
  refused.
- Both transports are served from the main loop, so neither can be used
  with an option that runs no main loop or owns it, the same set `--fark`
  refuses: `--headless-check` and the options that imply it, the
  `--check-*` options, `--benchmark`, `--match-ticks`, `--render-script`,
  `--generate-script` and `--showcase`.
- `--mcp` also refuses `--status-stdout` (that option writes standard
  output too), a launch read from standard input, and `--list-content`.
  `--mcp-http` does not refuse those three.
- With either option the game keeps running every frame when its window is
  not the active one, and the battle room's engine line ends in ` REMOTED`.
- Frames keep drawing while a stepped clock is held. A shared match, a
  replay and a watcher refuse the stepped clock and stay on the wall clock.
  When both transports are on, a step from either client runs the one clock.
- On standard input, once the client closes its stream and any waiting
  answer has been written, the game leaves. Closing the HTTP client does
  not leave the game.

A headless run uses SDL's dummy drivers, as the
[automation endpoint](automation.md#turning-it-on) does:

```
SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy open-annihilation --mcp --skip-intro --mute
```

## The badge

The badge sits at the top right of the window, in the window's own pixels,
over the picture. It is `MCP · realtime` or `MCP · stepped`.

- It is drawn only after a client has finished `initialize` and sent
  `notifications/initialized`, and only while that client is still
  connected. A game that is only listening, a connection that has not
  finished that handshake, and a client that has gone draw no badge.
- On a menu the word is the clock `--mcp-clock` asked for (`realtime` when
  the option was left out). In a match it is the clock that match is
  actually using, including a later `clock.mode` and a shared match that
  stayed on the wall clock.
- When both transports have a client, there is still one badge. They share
  one clock.
- The badge is not part of the match. It does not change a saved game, a
  recording, or the state hash.

## Claude Code

Register the game by hand, for the executable you trust. The repository
holds no `.mcp.json`. A project file would have every session opened in
the checkout offer to run whatever that tree holds.

Standard input, so Claude Code starts the game:

```
claude mcp add open-annihilation -- /path/to/open-annihilation --mcp --skip-intro --mute
```

Add `--mcp-clock stepped` when the match should tick only when asked.
`--mcp=observe` lists only the tools that read. A further argument is a
game option, such as `--resolution 1280x720` or `--seed 1`. Do not pass an
option the [starting](#starting-it) rules refuse.

A game that is already running with `--mcp-http` is reached at the address
in `mcp.json`, with the file's bearer token:

```
claude mcp add --transport http open-annihilation http://127.0.0.1:PORT/mcp \
  --header "Authorization: Bearer TOKEN"
```

The token is 32 hexadecimal digits from that file. The game must already
be running: this command does not start it. One HTTP client is served at
a time.

## Claude Desktop

Claude Desktop starts the game itself, the same way, from
`claude_desktop_config.json` (Settings, Developer, Edit Config). On macOS
that file is `~/Library/Application Support/Claude/claude_desktop_config.json`.
On Windows it is `%APPDATA%\Claude\claude_desktop_config.json`. The path of
the executable is absolute. Quit Claude Desktop and open it again after
saving.

```json
{
  "mcpServers": {
    "open-annihilation": {
      "command": "/path/to/open-annihilation",
      "args": ["--mcp", "--skip-intro", "--mute"]
    }
  }
}
```

`--mcp-clock`, `stepped` and the other arguments go in `args` after
`--mcp`. On Windows the command is `open-annihilation.exe`.

## Speaking the protocol

On standard input and standard output each message is one JSON-RPC object
on its own line. A batch is refused. Revisions `2025-06-18`, `2025-03-26`
and `2024-11-05` are accepted; an older name is answered as `2025-06-18`.
The first request is `initialize`. The next message is the notification
`notifications/initialized`, which has no `id`. Until that notification
arrives, only `initialize` and `ping` are answered.

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"example","version":"0"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"game.status","arguments":{}}}
```

The `initialize` result names `protocolVersion`, `serverInfo`
(`open-annihilation` and the engine's version), `instructions`, and
capabilities: tools do not announce list changes, and resources can be
subscribed. Methods after that are `ping`, "tools/list", "tools/call",
`resources/list`, `resources/read`, `resources/subscribe` and
`resources/unsubscribe`.

A tool result is a content block of text, the JSON object below, and for
a picture a second block of type `image`. A refusal is a tool error whose
text is the reason. A call that does not match the tool's schema is
refused before the tool runs. The schema check covers type, properties,
required, additionalProperties, enum, minimum and maximum. Over
`--mcp-http` that refusal is a JSON-RPC error, code -32602, in an HTTP
200 answer whose body is one JSON object. The 2025-06-18 transport
answers a request that way; invalid arguments are a protocol error, not
an HTTP error. HTTP 400 is a request the transport itself cannot accept
(too large, not JSON, or no session).

One line is at most 1 MiB. A pump reads at most 256 KiB and answers at
most 32 messages, and it does not wait. A tool that is not finished is
held and polled from later frames. While it is held the server may send
`notifications/progress`. Send one request and read its answer before the
next: a second request sent early can see the screen the first has not
yet changed.

`game.quit` answers `{"quitting": true}`, the answer is written, and then
the game leaves as the main menu's Exit does, leaving a match first.
SIGTERM does the same when no window is shown (none, hidden, a headless
run, or the dummy video driver) or when `--mcp-http` is on. A window the
player can see, without `--mcp-http`, still asks the exit question. The
window's own close button asks that question either way.

## Over HTTP

`--mcp-http` writes `mcp.json` beside the preferences file, whole, and
readable only by its owner where the system has such modes. The log names
the file and the URL, and never holds the token.

```json
{"address":"http://127.0.0.1:53122/mcp","token":"9c1f…(32 hex digits)","pid":41877}
```

POST that URL with `Content-Type: application/json`, `Accept` listing
`application/json` and `text/event-stream`, and
`Authorization: Bearer` plus the token. A missing or wrong token is
`401`. The first answer carries `Mcp-Session-Id`; later requests send it
back. `DELETE /mcp` with that header ends the session. One client at a
time: a second connection is `409` and closed.

A finished answer is one JSON body. An answer that is still held, and a
server notification, is a server-sent event on the POST that asked. A
picture can be larger than 1 MiB; it is still written on that POST, across
as many writes as the connection takes. A GET that stays open is that one
connection, so a POST waits until it closes. Notifications are discarded
when no event stream is open. A request body is at most 1 MiB, a request
must arrive within 10 seconds, and a client that does not read is dropped
once 1 MiB of answer is still unsent and none of it has moved for 10
seconds. A notification that comes while a picture is still being written
is discarded.

## Tools

`--mcp=observe` omits every tool whose line below says it acts. An empty
object `{}` is the schema of a tool that takes no arguments. Every schema
refuses a property it does not name.

### The screen

| Tool | Acts | What it does |
|---|---|---|
| `ui.screen` | | The screen (`main_menu`, `skirmish`, `match` and the like), the dialogs over it, the focused control and the pointer. |
| `ui.tree` | | Every control as a tree: name, kind, rectangle, text, enabled and checked state, and a list's rows. Text, list rows and a row's key are UTF-8: bytes that are not already UTF-8 are read as the game's code page (Windows-1252). A masked field's text is empty; `length` is how many characters it holds. Covers the game's own screens, Open Annihilation's screens, an extension's windows, and a message box. Names are the ones in [automation.md](automation.md#oas-own-screens). |
| `ui.click` | yes | Clicks a control by its name, at the centre of where it is drawn. `window` picks that control when the name is repeated. |
| `ui.select` | yes | Chooses a list row by its words or its index. `window` names the list when that name is repeated. A row off the page is scrolled into view. |
| `ui.type` | yes | Types into the control that has the keyboard. `submit` presses Enter after the text. |
| `ui.key` | yes | Presses a chord, or the first key the action registry gives an action id. |
| `ui.pointer` | yes | Moves the pointer onto a control, or to a point. `window` picks the named control when the name is repeated. |
| `ui.scroll` | yes | Turns the wheel at a control, or at a point. `window` picks the named control when the name is repeated. |
| `ui.drag` | yes | Drags from one control to another, or from one point to another. One `window` applies to both named ends. |
| `ui.wait` | | Waits until a screen, a control or a text shows, or until a match-watch event, and then returns. `window` names the control. It does not stop the frame. |
| `capture.screenshot` | | A PNG of the frame just presented, or of the one composed, whole or a region. The text is the width, the height and the source. The picture is also an image content block. A masked field is drawn as its mask character, so neither the text nor the picture carries the field's text. |

```json
{"ui.screen": {},
 "ui.tree": {},
 "ui.click": {"type": "object", "additionalProperties": false, "required": ["name"],
   "properties": {"name": {"type": "string"}, "window": {"type": "string"},
     "button": {"type": "string", "enum": ["left", "right", "middle"]},
     "double": {"type": "boolean"}, "expect_screen": {"type": "string"}}},
 "ui.select": {"type": "object", "additionalProperties": false, "required": ["list"],
   "properties": {"list": {"type": "string"}, "window": {"type": "string"},
     "item": {"type": "string"},
     "row": {"type": "integer"}, "double": {"type": "boolean"}}},
 "ui.type": {"type": "object", "additionalProperties": false, "required": ["text"],
   "properties": {"text": {"type": "string"}, "submit": {"type": "boolean"}}},
 "ui.key": {"type": "object", "additionalProperties": false,
   "properties": {"chord": {"type": "string"}, "action": {"type": "string"},
     "phase": {"type": "string", "enum": ["tap", "press", "release"]},
     "repeat": {"type": "integer", "minimum": 1, "maximum": 30}}},
 "ui.pointer": {"type": "object", "additionalProperties": false,
   "properties": {"name": {"type": "string"}, "window": {"type": "string"},
     "x": {"type": "integer"},
     "y": {"type": "integer"}, "space": {"type": "string", "enum": ["game", "window"]}}},
 "ui.scroll": {"type": "object", "additionalProperties": false,
   "properties": {"name": {"type": "string"}, "window": {"type": "string"},
     "x": {"type": "integer"},
     "y": {"type": "integer"}, "dx": {"type": "integer"}, "dy": {"type": "integer"},
     "space": {"type": "string", "enum": ["game", "window"]}}},
 "ui.drag": {"type": "object", "additionalProperties": false,
   "properties": {"from": {"type": "string"}, "to": {"type": "string"},
     "window": {"type": "string"},
     "x0": {"type": "integer"}, "y0": {"type": "integer"},
     "x1": {"type": "integer"}, "y1": {"type": "integer"},
     "space": {"type": "string", "enum": ["game", "window"]},
     "button": {"type": "string", "enum": ["left", "right", "middle"]}}},
 "ui.wait": {"type": "object", "additionalProperties": false,
   "properties": {"screen": {"type": "string"}, "control": {"type": "string"},
     "window": {"type": "string"},
     "text": {"type": "string"}, "event": {"type": "string"},
     "timeout_ms": {"type": "integer", "minimum": 0, "maximum": 600000}}},
 "capture.screenshot": {"type": "object", "additionalProperties": false,
   "properties": {"source": {"type": "string", "enum": ["presented", "composed"]},
     "space": {"type": "string", "enum": ["game", "window"]},
     "region": {"type": "array"},
     "scale": {"type": "integer", "minimum": 1, "maximum": 100},
     "max_width": {"type": "integer", "minimum": 0}}}}
```

`ui.key` does not read the keys a player has laid out. `scale` is a
percent. `region` is `x`, `y`, `width` and `height`. A name is used where
one is given; a point is used for a pointer, a scroll or a drag that names
no control. `window` is the extension window, or the list or slider a
scroll button belongs to, and it picks that control when the same name is
listed more than once. One `window` on `ui.drag` applies to both ends.
`ui.click` is refused when another screen shows by the time
the click arrives (`expect_screen`). A masked field's text is not in
`ui.tree`, `ui.screen`, `ui.wait`, or the `screen` object a click
answers with. `ui.tree` gives `length`, the number of characters that
field holds, and an empty `text`.

### The match

| Tool | Acts | What it does |
|---|---|---|
| `game.info` | | What is fixed for the game: the engine version, the mod, the standard hacks in force, and in a match the map, the rules and each player's side and team. |
| `game.status` | | The running match: tick, clock, pause, speed, the unit limit, each player (with units, kills, losses, colour, team, allies and economy) and the outcome. With no match the tick is 0. It has every field of the automation endpoint's `match` request except `state_hash`. |
| `game.speed` | yes | Sets the speed, as the `+` and `-` keys do. The answer names `speed` and `applied_tick`. |
| `game.pause` | yes | Pauses or resumes, as the Pause key does. The answer names `paused` and `applied_tick`. |
| `game.quit` | yes | Leaves the game. Answers `{"quitting": true}`. |
| `clock.mode` | yes | Sets the match clock to `realtime` or `stepped`. |
| `clock.step` | yes | Runs the stepped clock for `ticks` ticks. The match's speed does not scale them. |
| `clock.run` | yes | Runs the stepped clock until a tick, or until the match ends. |

```json
{"game.info": {},
 "game.status": {"type": "object", "additionalProperties": false,
   "properties": {"awareness": {"type": "string", "enum": ["player", "full"]}}},
 "game.quit": {},
 "game.speed": {"type": "object", "additionalProperties": false, "required": ["speed"],
   "properties": {"speed": {"type": "integer", "minimum": 0}}},
 "game.pause": {"type": "object", "additionalProperties": false, "required": ["paused"],
   "properties": {"paused": {"type": "boolean"}}},
 "clock.mode": {"type": "object", "additionalProperties": false, "required": ["mode"],
   "properties": {"mode": {"type": "string", "enum": ["realtime", "stepped"]}}},
 "clock.step": {"type": "object", "additionalProperties": false, "required": ["ticks"],
   "properties": {"ticks": {"type": "integer", "minimum": 1, "maximum": 100000}}},
 "clock.run": {"type": "object", "additionalProperties": false,
   "required": ["until", "max_ticks"],
   "properties": {"until": {"type": "string", "enum": ["event", "tick"]},
     "max_ticks": {"type": "integer", "minimum": 1, "maximum": 100000},
     "tick": {"type": "integer", "minimum": 0}}}}
```

`game.info` names the engine version (as `initialize` does), the mod (id,
display name and version, or null for the base game) and the standard hacks
that are on, each with the value the mod profile resolves. Outside a match,
`in_match` is false and those three stay filled. In a match it adds the map
(name and size) and the rules the match plays:
- commander death: `continues`, `ends` or `deathmatch`;
- line of sight: `permanent`, `true` or `circular`;
- whether the map starts mapped;
- the starting metal and energy;
- the unit limit;
- the mutators applied;
- each seated player's side and team.

`game_type` is null for now: a running match does not carry the launcher's mode
name.

`game.status`, in a match, adds `unit_limit` and `clock`. `clock.seconds` is
the tick divided by 30, and `clock.display` is the `h:mm:ss` the in-game
clock shows. Each player has `units`, `units_created`, `kills`, `losses`,
`colour`, `team`, `allies`, `board_row` (the kills board's order) and
`economy`. Metal and energy each have:
- `amount`;
- `storage`, plus `shared_storage` when allies' storage is shared;
- `income` and `expense`, per second, the figures the resource bar shows;
- `produced_total`, `requested_total` and `wasted_total`.

The economy is the local player's and its allies'. An enemy's is null
unless `awareness` is `full`, which only a single-player match allows.
`oa://game/status` reads with the player's awareness.

`game.speed` and `game.pause` are refused for a replay and a watcher, and
a finished match refuses a pause. A shared match is changed, so the other
machines stay in step. The change is logged with the tick it landed on.

`clock.mode`, `clock.step` and `clock.run` are refused for a replay, a
watcher and a shared match. A step or a run is also refused while the
match is paused, finished, or still on the wall clock. One frame runs at
most 120 ticks; a longer request stays held and continues on later frames.
`clock.step` answers `ran`, `tick` and `events` (only `game_over`, when
the match ends during the step). `clock.run` with `until` `tick` runs
until `tick`, and at most `max_ticks`. With `until` `event` it stops when
the match ends, or at `max_ticks`. The answer says why it stopped.

### Units and orders

| Tool | Acts | What it does |
|---|---|---|
| `units.list` | | The units the local player is shown. Each has its id, type (the unit name, such as `ARMCOM`), name, side, owner, position in map pixels, health, build progress as a percent, flags (`selected`, `radar`, `cloaked`) and its current order. |
| `units.get` | | One of those units. |
| `units.orders` | | That unit's primary and secondary queues. |
| `units.types` | | The catalogue of types, not the units on the field: build options, weapons, maximum speed, and sight, radar and sonar distances. |
| `orders.issue` | yes | Gives move, patrol, attack, guard, repair, reclaim, build, factory or stop. |
| `orders.state` | yes | Sets fire or move to 0, 1 or 2, or switches on, off or cloak. |
| `units.select` | yes | Selects units, as a player would. |

```json
{"units.list": {"type": "object", "additionalProperties": false,
   "properties": {"owner": {"type": "string"}, "type": {"type": "string"},
     "x": {"type": "integer"}, "z": {"type": "integer"},
     "radius": {"type": "integer", "minimum": 0},
     "awareness": {"type": "string", "enum": ["player", "full"]}}},
 "units.get": {"type": "object", "additionalProperties": false, "required": ["id"],
   "properties": {"id": {"type": "integer", "minimum": 0, "maximum": 65535},
     "awareness": {"type": "string", "enum": ["player", "full"]}}},
 "units.orders": {"type": "object", "additionalProperties": false, "required": ["id"],
   "properties": {"id": {"type": "integer", "minimum": 0, "maximum": 65535},
     "awareness": {"type": "string", "enum": ["player", "full"]}}},
 "units.types": {"type": "object", "additionalProperties": false,
   "properties": {"type": {"type": "string"}}},
 "orders.issue": {"type": "object", "additionalProperties": false,
   "required": ["units", "kind"],
   "properties": {"units": {"type": "array"},
     "kind": {"type": "string", "enum": ["move", "patrol", "attack", "guard",
       "repair", "reclaim", "build", "factory", "stop"]},
     "append": {"type": "boolean"},
     "target": {"type": "integer", "minimum": 1, "maximum": 65535},
     "point": {"type": "array"}, "type": {"type": "string"},
     "count": {"type": "integer"},
     "facing": {"type": "integer", "minimum": 0, "maximum": 3}}},
 "orders.state": {"type": "object", "additionalProperties": false,
   "required": ["units", "state"],
   "properties": {"units": {"type": "array"},
     "state": {"type": "string", "enum": ["fire", "move", "on", "off", "cloak"]},
     "value": {"type": "integer", "minimum": 0, "maximum": 2},
     "on": {"type": "boolean"}}},
 "units.select": {"type": "object", "additionalProperties": false, "required": ["units"],
   "properties": {"units": {"type": "array"}, "add": {"type": "boolean"}}}}
```

`owner` is `me`, `all` or a player from 0 to 9. `type` on `units.list` is
a unit name. An area is `x`, `z` and `radius` in map pixels, and the three
are given together. `awareness` `full` includes units the player is not
shown, and only in a match that is not shared. With no match running, each
of these reads fails rather than inventing an empty list.

`orders.issue` is for this machine's units only. `append` adds the order
behind the ones the unit already has. A point is two whole map pixels,
`x` and `z`, or three, `x`, `y` and `z`. At most 64 units are named. A
unit this machine does not play refuses the whole call, and nothing is
given. A replay, a watcher and a finished match refuse the call. Each
answer names `applied_tick`. An order's answer also names each unit's
queues, in the same words as `units.orders`. A selection's answer names
the units selected afterwards. `add` keeps the units already selected; an
empty list clears the selection. A selection is made at once. A stepped
clock holds an order or a standing order until a step runs it, and that
step's tick is `applied_tick`.

An order's `kind` in a queue is the mission's registered name. A build
order's `build` is the product's unit name and `parameter_2` is how many
remain.

### The map

| Tool | Acts | What it does |
|---|---|---|
| `map.info` | | The map's name, its size in map pixels, the plot counts, the sea level, and a height for every cell. `metal` lists cells whose metal is not zero. `features` lists origin plots. |
| `map.visibility` | | One player's `sight`, `explored`, `radar`, `sonar` or `jammed` grid, as runs or as a PNG. |
| `map.radar` | | That player's radar and sonar contacts: a position and whether it is radar, sonar or both. No identity. |

```json
{"map.info": {"type": "object", "additionalProperties": false,
   "properties": {"cell": {"type": "integer", "minimum": 1, "maximum": 8192}}},
 "map.visibility": {"type": "object", "additionalProperties": false, "required": ["layer"],
   "properties": {"player": {"type": "string"},
     "layer": {"type": "string", "enum": ["sight", "explored", "radar", "sonar", "jammed"]},
     "cell": {"type": "integer", "minimum": 1, "maximum": 8192},
     "format": {"type": "string", "enum": ["png", "rle"]},
     "awareness": {"type": "string", "enum": ["player", "full"]}}},
 "map.radar": {"type": "object", "additionalProperties": false,
   "properties": {"player": {"type": "string"},
     "awareness": {"type": "string", "enum": ["player", "full"]}}}}
```

`map.info`'s `cell` is 16 when omitted, one plot. A cell that would make
the height grid larger than 512 by 512 is refused. `features` lists at
most 2500 origin plots; `features_truncated` is set when there were more.

`map.visibility`'s `cell` is 32 when omitted. `format` `rle` is the
default: runs of a count and a 0 or 1. A PNG is grey, 255 where the cell
is on, and is also an image content block. A PNG of more than 256 by 256
cells is refused. For sight and explored, `minimap` is the minimap's own
fog for those cells. `player` is `me` or a player from 0 to 9. Another
player's grid or contacts need `awareness` `full`, and only in a match
that is not shared. `kept` on `map.radar` is false when the match does not
keep that player's picture, and the contacts are then empty.

### The camera

| Tool | Acts | What it does |
|---|---|---|
| `camera.get` | | The view's place, in map pixels, and its zoom. |
| `camera.set` | yes | Moves the view to `x`, `z`. The view's limits hold the place. |
| `camera.center` | yes | Centres on a unit, or on a point `x`, `z`. |
| `camera.zoom` | yes | Sets `zoom`, or multiplies it by `factor`, about a point when `x` and `y` are given. |

```json
{"camera.get": {},
 "camera.set": {"type": "object", "additionalProperties": false, "required": ["x", "z"],
   "properties": {"x": {"type": "integer"}, "z": {"type": "integer"}}},
 "camera.center": {"type": "object", "additionalProperties": false,
   "properties": {"unit": {"type": "integer", "minimum": 1, "maximum": 65535},
     "x": {"type": "integer"}, "z": {"type": "integer"}}},
 "camera.zoom": {"type": "object", "additionalProperties": false,
   "properties": {"zoom": {"type": "number"}, "factor": {"type": "number"},
     "x": {"type": "number"}, "y": {"type": "number"}}}}
```

A move's answer names the place the limits held, and `applied_tick`. The
next presented frame draws that view, paused or not. A replay and a
watcher may move the view, because it is this machine's picture. The
director's view is refused. A camera move does not wait for a step. The
change is logged with the tick it landed on.

## Resources

| URI | What a read returns |
|---|---|
| `oa://game/info` | What is fixed for the game, as `game.info` reports it. |
| `oa://game/status` | The running match, as `game.status` reports it with the player's awareness. |
| `oa://events` | The last 4096 events, oldest first, as `events.read` reports them. |

`resources/subscribe` on `oa://events` asks to be told when that list
changes. The notification is `notifications/resources/updated`.

## Events

| Tool | What it does |
|---|---|
| `events.subscribe` | Chooses the events this session is sent from now on, as `notifications/oa/event`. An empty list stops them. Notes already kept are not sent again. |
| `events.read` | Reads the events still kept, oldest first, the last 4096. `after` leaves out events at or before that `seq`. |

```json
{"events.subscribe": {"type": "object", "additionalProperties": false,
   "required": ["events"], "properties": {"events": {}}},
 "events.read": {"type": "object", "additionalProperties": false,
   "properties": {"after": {"type": "integer", "minimum": 0}}}}
```

`events` is `"all"`, one kind, or a list of kinds. `all` is given on its
own and names every kind. The kinds are `screen`, `player`, `ready`,
`chat`, `loaded`, `match`, `pause`, `speed`, `alliance`, `transfer`,
`game_over`, `unit_created`, `unit_destroyed`, `unit_idle`,
`unit_damaged`, `order_done`, `build_done`, `enemy_seen`,
`radar_contact`, `said`, `page`, `ticker`, `alert`, `session` and
`console`. An unknown kind is refused. `ui.wait`'s `event` argument stays
the match-watch kinds (`screen` through `radar_contact`). `said`, `page`,
`ticker`, `alert`, `session` and `console` are `events.subscribe` and
`events.read` only.

Each note has `seq` and `tick`, and `event` for the kind. `tick` is the
match's tick, taken where the hook ran. `seq` numbers the notes still
kept, from 1. `next` in an answer is the newest of those, or 0 when there
are none. A unit note names the unit. A radar contact names only its
place. A `said`, `page`, `ticker`, `alert`, `session` or `console` note
also has `frame` and `text`. Both transports share the one list of 4096;
the 4097th note drops the oldest. Each session chooses which kinds it is
sent. `clock.step` and `clock.run` do not list these notes. They report
only the match ending.

## Lobby coverage

The lobby is the screen after the intro, before a match. Every control a
player can use there is reached by the screen tools. A repeated name is
picked with `window`. A list's scroll buttons are `decbutton` and
`incbutton` with `window` set to that list's name; in a lobby list
`incbutton` is the arrow at the top of the bar and `decbutton` the one at
the bottom. A slider's buttons use the slider's name the same way.
`ui.select` chooses a row of a list. A row reads as the words it shows: a
row drawn as an icon and a name, such as a player, a frequency or a
profile category, reads as the name. A label that holds controls, such as
a radio group, lists its title and its radios under the same window.
Scattered buttons, including a planet, are `ui.click`. A button that is
dark takes no press, so a client waits for `enabled` before clicking, as
a player waits for it to light. A masked
field's text is not in these answers. `ui.tree` reports `length` and an
empty `text`. `Password`, `Battle password`, `Join password` and
`Frequency password` are masked.

| Surface | How a client reaches it |
|---|---|
| Login | `Guest Login`, `Registered User`, `Username`, `Password`, `LOGIN`, `CANCEL`, `REGISTER`, `Server List`, `Status`. `Username` and `Password` name the fields. `Password` takes no click until `Registered User` is chosen, and reports `length` and an empty `text`. After `LOGIN` the login console closes and the green layer slides off the map; the console buttons light when it has. |
| Registration | `REGISTER` opens the web page: `Url`, `Host`, `Qr` and `Back`. `Back` returns to login. `GameHot` or `WebHot` chooses a route when both are shown. The in-game form's fields are `User`, `Pass`, `Confirm`, `Email`, the radios `ArmRadio`, `CoreRadio`, `OpenRadio`, `RulesBox`, and `Register`, `Cancel`, `Status`. `Pass` and `Confirm` are masked. When the page is missing, a message box stands in for it. |
| Side prompt | `Arm`, `Core`, `Choose`, `EXIT`. `Choose` confirms the side. `EXIT` is the other way out. |
| Main-frame buttons | `ExitBYBtn`, `MF_Quickplay_Button`, `MF_Release_Button`, `MF_Recon_Button`, `MF_Chat_Button`, `MF_Battle_Button`, `MF_News_Button`, `MF_Ladders_Button`, `MF_Locator_Button`, `MF_Profile_Button`, `MF_Help_Button`, on `BYMainFrame`. |
| Ticker | The control `Ticker`. Its text is the whole line. The caption drawn on the label is the scrolling slice, about 99 characters, and is not the value to read. |
| Advert banner | The button `Ad_Banner` on `BYMainFrame`. Its text is `sample N` for a built-in frame, or the reel id as digits. `ui.click` presses it. The picture itself is not a control. |
| War map | Each planet on the viewer is a button on `MapPanel`, named by its unique name, or `name #id` when the name is shared, or `#id` when it has none. The same planet is also that decimal id, on the same rectangle, so `ui.click` `{name: id, window: MapPanel}` lands on it. Its text is `id= owner= battles= forming=`. While the map is locked (the login console or another console is open, or the green layer is still sliding off after login) a press selects nothing, and every planet reports `enabled` false. A press on an enabled planet chooses it: the planet summary shows its name as a label on `BYMainFrame`, and `MF_Recon_Button`, `MF_Chat_Button` and `MF_Battle_Button` light. |
| Planet off the viewer | The same label and the same decimal id, as labels on `BYMainFrame` with no area, so the facts stay readable. `ui.click` is refused (`has no area`) until the planet is on the viewer. Moving the map is the eight edge buttons `Map up`, `Map up right`, `Map right`, `Map down right`, `Map down`, `Map down left`, `Map left`, `Map up left`. |
| Sector and magnification | Labels `Sector` and `Magnification` on `BYMainFrame`, with no area. They are read, not clicked. Magnification changes by `ui.click` `{name: incbutton, window: Mag_Slider}` or `decbutton` on that slider. At a limit the number can stay the same; the click still lands. |
| Recon | `MF_Recon_Button`, then `ReconList`, whose rows are the planet's description, and the field labels (`Planet:` and the planet's name, `GW Map:`, `Type:`, `Gravity:`, `Wind Speed:`, `Tide Strength:`, `Affiliation:`, `Population:`), each value a label named by its text. |
| Chat | `MF_Chat_Button`. The log is `ChatList` (`ui.select` is not how a line is read; the rows are `items`). Send with `ui.click` `Chat entry`, then `ui.type` with `submit`. Rooms are `Chat rooms`, each row the frequency's name. Players are `Chat players`, each row the player's name; `ui.select` one by its name to open the player pane, with the player's name, rank and clan as labels, the tabs `GeneralTabBtn`, `LadderTabBtn` and `AwardsTabBtn`, `PAGE`, and the checks `Whisper Lock` and `Muzzle`. `PAGE` fills the entry with `+Page` and the name; typing the message and `submit` sends the page. `+w`, a name and a message whispers. Scroll with `decbutton` or `incbutton` and `window` `ChatList`. |
| Create and info panes | `Frequency name`, `Frequency password`, `Frequency description`, and `GO`. `Frequency password` reports `length` and an empty `text`. The info pane's password is also `Frequency password`; `window` tells the two apart, and they are not shown together. |
| Battle room | `MF_Battle_Button`. `BattleList` lists each game type with the planet's battles under it. `ui.select` a battle by its name to open the `BATTLE INTELLIGENCE` pane: its name, `Game Type:`, the seats `Host:`, `Player 2:`, `Player 3:`, `Player 4:` with who holds each (`Blocked` for a seat the battle does not have), `Skill Level:`, `On Commander Death:`, and `JOIN`. A battle that needs a password also shows `Join password`. `HOST` opens the `HOST BATTLE` pane: `Battle name`, `Battle password`, `Game types`, the radios `Novice`, `Seasoned`, `Expert`, `2`, `3`, `4`, `Ends`, `Continues`, `Deathmatch`, and the group titles `Player Skill`, `Number of Players`, `On Commander Death`. A duel seats two, so `3` and `4` are disabled while `Duel` is the game type. `Battle password` and `Join password` report `length` and an empty `text`. `START` launches and stays disabled while the name is empty. `JOIN` enters the battle. `PlanetIntelList` is the planet list. |
| News, Ladders, Help | `MF_News_Button`, `MF_Ladders_Button`, `MF_Help_Button`. The list is `ContentsList`. A category row's text is `+ ` while its headlines are hidden and `- ` while they are shown. `ui.select` of that row toggles it. The arrow the player sees is not the text. Ladders adds `Ladder search` and `SEARCH`. A name the index does not list opens a message box. |
| Locator | `MF_Locator_Button`, disabled unless that account's release gate shows it. The window is `Locator`. `row_0` to `row_4` choose what to find; the labels `kind_0` to `kind_4` name them (`Locate Player`, `Locate Planet`, `Find Battle`, `Find Frequency`, `Server Transfer`). `Name` is the field and `Search` asks. Each result is a row of labels `rnameN`, `rwhereN` and `rstatusN`, with the icon `riconN`; `rhitN` chooses it, and `rbandN` marks the chosen one. `Relocate` goes to it and `Page` pages the player. |
| Profile | `MF_Profile_Button`, then `ProfileList`. `ui.select` a category. The shown tab's fields are named by their captions. `APPLY` and `RESET` are the buttons. |
| Quick play | `MF_Quickplay_Button`, disabled unless that account's release gate shows it. The console is hidden until the button opens it. Its window is `QuickPlay`. Its controls include `Any planet`, `Fuel Dump only`, `Novice`, `Seasoned`, `Expert`, `Arm`, `Core`, `Either`, the favourite map's check (listed by its caption, `Prefer my favorite:` and the map), the game types `Pick0` to `Pick4` beside the labels `Label0` to `Label4`, `FIND BATTLE` and `CANCEL SEARCH`. `window` `QuickPlay` tells `Any planet` and the skill radios from the same names elsewhere. |
| Matchmaking | The button `Minimize_Button` on the chat console, only while that account's release gate shows it. The dialog's window is `Matchmaking`. Its controls include `Duel`, `WinnerTakesAll`, `For Fun`, `Near mine`, `Any`, `Arm`, `Core`, `Either`, `This planet`, `Any planet`, `Activate`, which joins the queue, and `Close`. `window` `Matchmaking` picks `Any planet` when quick play is also open. |
| Message box | `BYMessageBox`, with `Text` and the buttons `Ok`, `Cancel`, `Yes`, `No` for the ones the box has. Read `Text`, then `ui.click` the answer. |
| Exit question | `Exit to the system?` opens when the window is asked to close. MCP does not ask the window to close, so this box is not opened by a tool. Its controls are `Text`, `Yes` and `No`. `Yes` leaves the game. `ExitBYBtn` does not open it; that button returns to the menu. `MF_Release_Button` closes the open console, or leaves while login is open. |
| Checks | `BYCheckBox` stays the window's own name. The control is listed by its caption (`Whisper Lock`, `Muzzle`, the favourite map's check), so each check in one console is distinct. |
| Launching | `START`, `JOIN`, `FIND BATTLE` and matchmaking's `Activate` are pressed with `ui.click` like any button. The replay-driven check (`native-mcp-lobby`) reads them and sets everything around them, but does not press them: each hands the game to a network battle, or to the server's search for one, which a replay does not answer. |

`said` is a chat line, `page` a page, `ticker` a new ticker line, `alert` a message box, `session` the connection level and the login name, and `console` the console that opened, once it shows (`login`, `recon`, `chat`, `battle`, `ladders`, `news`, `profile`, `help`, `quickplay`, `locator`, `update`). A console that opens over the map shows when the green layer has slid over it. The object field is `event`, plus `seq`, `tick`, `frame` and `text`. `ui.wait` does not see these six.

## Rules

- Without `--mcp` and without `--mcp-http`, nothing here runs. No stream
  is read, no socket is opened, `mcp.json` is not written, and the badge
  is not drawn. The match computes, saves, sends and records what it does
  with the option left out.
- Acts land at the frame's after-pump, before the match clock, in the
  order they arrived. The answer names the tick the change landed on.
  What an act did is written to the log as `mcp:` followed by the tick and
  the act. A tool that only reads is not an act.
- In stepped mode the same calls on the same ticks give the same match. A
  test runs a session twice and compares the state hash. The hash is the
  one a saved game carries. `game.status` does not include it; a run that
  was also started with `--fark` reads it from the automation endpoint's
  `match` request, with `state_hash` true. Give the skirmish `--seed` so
  the two runs do not draw a seed from the clock.
- A stepped clock holds an order until a step runs it. A camera move, a
  selection, a speed change and a pause do not wait for a step.
- Full awareness, and another player's visibility, are refused in a shared
  match. Orders are this machine's units only.
- The server does not read or write files for a client, does not start
  programs, and does not open a socket except the loopback listener
  `--mcp-http` asked for. What the game then does with a click or a key
  is what it does for a player at the keyboard, including saves, settings
  and network play.

## An example

A client that started the game with `--mcp --skip-intro --mute` opens
Skirmish and reads the match. The screen names are the ones `ui.screen`
reports.

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"ui.wait","arguments":{"screen":"main_menu","timeout_ms":20000}}}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"ui.click","arguments":{"name":"SINGLE","expect_screen":"main_menu"}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"ui.wait","arguments":{"screen":"single_player","timeout_ms":20000}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"ui.click","arguments":{"name":"Skirmish","expect_screen":"single_player"}}}
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"ui.wait","arguments":{"screen":"skirmish","timeout_ms":20000}}}
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"ui.click","arguments":{"name":"Start","expect_screen":"skirmish"}}}
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"ui.wait","arguments":{"screen":"match","timeout_ms":20000}}}
{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"game.status","arguments":{}}}
```

Started with `--mcp-clock stepped`, the same client then moves its
commander and lets thirty ticks run. `units` is the commander's id from
`units.list` with `owner` `me`.

```json
{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"units.list","arguments":{"owner":"me","type":"ARMCOM"}}}
{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"orders.issue","arguments":{"units":[412],"kind":"move","point":[1800,900]}}}
{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"clock.step","arguments":{"ticks":30}}}
```

The order's `applied_tick` is the tick that step runs. The log line is
`mcp:` that tick, then `orders.issue`, then `mcp:` the reached tick, then
`clock.step 30`.
