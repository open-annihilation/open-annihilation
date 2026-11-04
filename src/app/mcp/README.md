# src/app/mcp

The MCP server: Model Context Protocol on standard input and standard
output, and on a loopback HTTP port, so a program that starts the game
or attaches to one already running can talk to it. It is `oa-game`'s
extension `oa-app-mcp`, registered after the automation endpoint.
Without `--mcp` it does not read standard input and does not write
standard output. Without `--mcp-http` it opens no socket.

`--mcp` speaks revisions `2025-06-18`, `2025-03-26` and `2024-11-05`.
`--mcp=observe` lists only the tools that read. `--mcp-http` serves the
same revisions on `127.0.0.1` or `[::1]`, one client at a time. All three
set `option_effect::remote_controlled`, so the main loop runs while the
window is inactive and the battle room's engine line ends in ` REMOTED`.

## Entry points

- `oa_extension_init_mcp` fills the extension table.
- `Session` (`include/oa/app/mcp/session.hpp`) answers one JSON-RPC
  message at a time. `LineBuffer` frames a byte stream into lines. Neither
  reads a file or a socket, and neither waits.
- `fill_option_hooks` takes `--mcp`, `--mcp-http` and `--mcp-clock`.
  `stdio_wanted` and `capture_stdio` tell `main` to keep the standard
  streams before the log redirect, and only when `--mcp` is on.
- `HttpServer` (`include/oa/app/mcp/http.hpp`) listens on loopback and
  writes `mcp.json` beside the preferences. `open` is the only call that
  opens a socket.
- `add_game_tools` adds `game.info`, `game.status`, `game.speed`,
  `game.pause`, `camera.get`, `camera.set`, `camera.center`, `camera.zoom`,
  `clock.mode`, `clock.step`, `clock.run`, `game.quit` and the resources
  `oa://game/info` and `oa://game/status`. `open_session` gives these, the
  unit tools and the map tools to both transports, and they share one queue
  and one clock.
- `add_unit_tools` adds `units.list`, `units.get`, `units.orders` and
  `units.types`. They read the running match through the match view
  (`match_view.hpp`).
- `add_order_tools`, called from `add_play_tools`, adds `orders.issue`,
  `orders.state` and `units.select`. They command this machine's units
  through the play host. Both transports offer them, because both sessions
  are given the one play bridge.
- `add_map_tools` adds `map.info`, `map.visibility` and `map.radar`. They
  read the same match view.
- `add_ui_tools` adds `ui.screen`, `ui.tree`, `ui.click`, `ui.select`,
  `ui.type`, `ui.key`, `ui.pointer`, `ui.scroll`, `ui.drag`, `ui.wait`
  and `capture.screenshot`.
- `add_event_tools` adds `events.subscribe`, `events.read` and the resource
  `oa://events`. Both transports share one list of the last 4096 notes.
  Each session chooses which kinds it is sent. A note's tick is the match's.

The frame hook serves each session at `FrameStage::pump`, which reads the
client and pushes input, and at `FrameStage::presented`, which only polls
what is already held so a picture of the frame just shown can finish.
With `--give-orders` the pump gives a skirmish the orders a headless
save/load run gives, once, before the tools read. A campaign, a shared
match and a replay are left alone, and a headless run never reaches the
hook. `game.speed`, `game.pause`, the camera moves, the clock tools,
`orders.issue`, `orders.state` and `units.select` are checked at the pump,
queued, and applied at `FrameStage::after_pump`, before the match clock, in
the order they arrived. `camera.get` only reads. The answer names the tick
the change landed on. An order's answer also names the queues. A camera
move's answer names the place the limits held. `--mcp-clock stepped` offers
that clock once when a single-player match starts, before the queued changes
run. Frames keep drawing while it is held. When both transports are on they
share that one clock, and a step from either client runs it. A shared match
is refused and stays on the wall clock. A stepped clock holds an order or a
standing order until a step runs it, and that step's tick is the one the
order lands on. A camera move does not wait for a step.

## State

One `Session` for each transport that is on, created on the first runtime
and kept across a switch of the mod. Held calls and resource subscriptions
live in that session. Both sessions share one play queue, one clock and
one screen drive. The duplicated standard handles live until the process exits.
`Serve::runtime` points at the runtime of the frame being served, and is
cleared at the end of the pump, after the presented stage and after the
queued changes are applied. `Serve::frame` counts the pumps. The screen
tools keep one act, one wait and one picture in `Serve::ui`. `UnitHost`
holds the match view for that same frame and is cleared with it, and both
sessions read through it. Both sessions share one event list, and each
has its own subscription. `--mcp-http` and `--mcp` can both be on; they do
not share a session.

## Invariants

- A pump does not wait. Standard input is polled with no timeout, because
  a non-blocking flag on a duplicate would also change the descriptor the
  process was started with. Standard output is made non-blocking on the
  first write, after the log redirect.
- At most 256 KiB are read and 32 messages answered per pump. A line
  longer than 1 MiB is refused. Reading stops while 1 MiB of answers is
  waiting for the client.
- A tool that is not finished is held and polled from later pumps. It is
  not waited on inside the frame. `ui.wait` checks the screen on each poll
  and returns when the timeout has passed. It does not sleep.
- A click, a key or a drag names a control where one exists. A point is
  used only for a pointer, a scroll or a drag that names no control.
- `ui.key` with an action id presses the first key the action registry
  gives that action. It does not read the keys a player has laid out.
- `capture.screenshot` takes `scale` and `max_width` as whole numbers.
  `scale` is a percent from 1 to 100. The picture is an image content
  block after the text. On HTTP the picture is written when the presented
  stage delivers the held call, the same as on standard input.
- `game.speed` and `game.pause` are refused for a replay and a watcher, and
  a finished match refuses a pause. A shared match is changed, so the other
  machines stay in step. The change is logged with the tick it landed on.
- `orders.issue`, `orders.state` and `units.select` are refused for a replay,
  a watcher and a finished match. A unit this machine does not play refuses
  the whole call, and nothing is given. The change is logged with the tick
  it landed on. A stepped clock holds an order or a standing order until a
  step runs it. A selection is made at once.
- `camera.get` reads the view's place and zoom. `camera.set`,
  `camera.center` and `camera.zoom` move it through the same paths the game
  uses, and the view's limits hold the place and the zoom. The next
  presented frame draws that view, paused or not, as a scroll does. Every
  tier draws the zoom. The standard tier, which a software renderer uses,
  draws the world layer at the zoom. Basic and Full do too: above a zoom of
  1 they magnify a scene drawn at 1 when magnify is on, and otherwise draw
  at the zoom, and below 1 they draw at the zoom or through the area pass.
  A replay and a watcher may move the view, because it is this machine's
  picture. The director's view is refused. A move is logged with the tick
  it landed on. A camera move does not wait for a step.
- `clock.mode`, `clock.step` and `clock.run` are refused for a replay, a
  watcher and a shared match. A step or a run is also refused while the
  match is paused, finished, or still on the wall clock. One frame runs at
  most 120 ticks; a longer request stays held and continues on later frames.
  The only event a run reports on its own is the match ending (`game_over`).
  The change is logged with the tick it landed on.
- `game.quit` answers, the answer is written, and then the automation
  host's quit runs. It is not queued. On standard input, the process also
  leaves once the client closes its stream and any waiting answer has been
  written. Closing the HTTP client does not leave the game.
- Without `--mcp`, these hooks do not touch the standard streams. Without
  `--mcp-http`, no socket is opened and `mcp.json` is not written.
- HTTP reads at most 1 MiB and spends at most 1000 µs on a request per
  pump, and a request must arrive within 10 s. Those are the automation
  endpoint's budgets. A body is at most 1 MiB, and a client that does not
  read is dropped once 1 MiB of answer is waiting.
- `DELETE /mcp` is answered at once. The session it ends is replaced on
  the next pump, before another client is accepted.
- `events.subscribe` and `events.read` share one list of the last 4096 notes
  with `oa://events`. A note's tick is the match's tick, taken where the
  hook ran. `events.subscribe` sends later notes of the kinds it names as
  `notifications/oa/event`, and does not send notes already kept. An empty
  list stops them. `clock.step` and `clock.run` do not list these notes.

## Match facts

`game.info` and `oa://game/info` read what is fixed for the running game.
The engine version is the one `initialize` reports. The mod is its id,
display name and version, or null for the base game. The hacks are the
standard hacks that are on, each parameter as the mod profile resolves it,
including a Developer Mode override. Outside a match, `in_match` is false
and the engine version, the mod and the hacks stay filled.

In a match the map is its name and size. The rules are the ones the match
plays: commander death (`continues`, `ends` or `deathmatch`), line of sight
(`permanent`, `true` or `circular`), whether the map starts mapped, the
starting metal and energy, and the unit limit (`Game.max_units_setting`).
The mutators are the ones that match applied. `game_type` is null: a
running match does not carry a launcher mode name. Each seated player's
side and team are listed. An example mod is Ridge (`ridge`, version `28`).

`game.status` keeps the fields it already reported and adds, while a match
runs, `unit_limit` and `clock`. `clock.seconds` is the match tick divided
by 30. `clock.display` is the `h:mm:ss` the in-game clock shows. Each
player gains `units_created`, `board_row` and `economy`. Metal and energy
each have `amount`, `storage`, `income`, `expense`, `produced_total`,
`requested_total` and `wasted_total`. `storage` is `Player.metal_storage`
or `Player.energy_storage`, the store the resource bar reads. When
`resource_flags` shares allies' storage, `shared_storage` is
`Player.shared_metal_storage` or `Player.shared_energy_storage` and is not
added into `storage` again.

`income` and `expense` are `Player.metal_produced`, `Player.metal_requested`,
`Player.energy_produced` and `Player.energy_requested`: the per-second
figures `format_metal_rate` and `format_energy_rate` in
`src/ui/hud/src/resource_bar.cpp` format, and `format_panel_income` in
`src/ui/hud/src/resource_panel.cpp`. The JSON numbers are those figures.
The bar's own text shortens them.

Economy and the totals are the local player's and each player allied with
it (`allied_by`). An enemy's economy is null unless the call passes
`awareness` `full`, which a shared match refuses, the rule `units.list`
uses. `oa://game/status` takes no arguments, so it stays on the player's
view. Kills, losses, colours, teams, alliances, the clock and the rules
are shown for everyone. `game.info` and `game.status` only read, so
`--mcp=observe` lists both.

## Tests

`mcp-protocol` checks framing, the lifecycle, "tools/list", "tools/call",
unknown tools, schema failures, held calls and their progress
notifications, resource subscription, and a tool result that carries a
PNG. `mcp-units` checks the unit tools against a stand-in match: the list,
one unit, the order queues and the types, including that a unit the
player is not shown is left out unless full awareness is asked for and
allowed. `mcp-map` checks the map tools against a stand-in match: the
map, a visibility grid as runs and as a PNG, and contacts that carry no
identity. Another player's grid and contacts are refused unless full
awareness is asked for and allowed. `mcp-options` checks `--mcp`,
`--mcp=observe`, `--mcp-http`, `--mcp-clock`, the options refused with
them, and the usage text. `mcp-http` checks HTTP framing, the bearer token
and its file, a second connection refused while a client is connected,
and a call that stays held through the pump and is answered when the
presented stage delivers. `automation-chords` checks the chords and
action ids `ui.key` presses. `mcp-play` checks that a replay and a watcher
refuse `game.speed` and `game.pause`, that a replay and a watcher may still
move the view, that a shared match refuses the clock, that a place past the
view's limits is reported where the limits hold it, and that a change is
applied on the pump after `after_pump` and the answer names that tick. It
also checks that an order for a unit this machine does not play is refused
when the queue is applied, that a move and a standing order name the tick
and the queue, and that an order sent under a stepped clock lands on the
tick the step runs. `mcp-events` checks `events.subscribe`, `events.read`
and `oa://events` with notes handed in and no running game: an unknown kind
is refused, a subscription sends only later notes of its kinds, the read
and the resource keep every kind, and the 4097th note drops the oldest.
`native-mcp-stdio` starts the game with `--mcp` and drives initialize,
the tool list, `game.status` and `game.quit`. `native-mcp-http` does the
same over `--mcp-http`, calling `game.status` and `game.quit`.
`native-mcp-ui` does the same through the screen tools: the main menu,
single player, the skirmish setup and back, the Library and Settings, and
the main menu strip's SINGLE and MULTI, and it saves two PNGs. It writes
its preferences into a fresh folder each run. `native-mcp-units` starts a
skirmish with `--mcp` and `--give-orders` and reads the units and a
factory's queue. `native-mcp-map` starts a skirmish with `--mcp` and
compares the sight and explored grids with the minimap's fog, then reads
the map and the contacts. `native-mcp-play` starts a skirmish through
`--fark` and checks that `game.speed` and `game.pause` are what
`game.status` reports. `native-mcp-camera` centres a unit, zooms, and checks
that `camera.get`, the view's span and a screenshot of that ground follow.
`native-mcp-clock` runs that skirmish twice with `--mcp-clock stepped`,
steps the same ticks, and requires the two state hashes to match.
`mcp-status` checks `game.info` in and out of a match, the economy against
a stand-in, an enemy's economy hidden unless full awareness is allowed,
and the clock's display at 0, 59 s and 1 h. `native-mcp-status` starts a
skirmish with `--mcp-clock stepped` and reads both tools: the mod and the
engine version, the local metal and energy, the unit limit, a clock that
advances, and a computer player's economy left unread.
`native-mcp-orders` starts a skirmish without `--give-orders` and checks
that `orders.issue` builds a solar collector, moves a unit and attacks, that
the queues come back, and that an order for a unit this machine does not
play is refused. `native-mcp-events` starts a skirmish without
`--give-orders`, subscribes to `unit_idle` and `order_done`, moves the
commander, and reads both notes for that move from `events.read` and from
`oa://events`.

## Limitations

While a client is connected the game draws a badge at the window's top
right, "MCP · realtime" or "MCP · stepped". Nothing is drawn when no
client has finished initialize, or after that client has gone. A
standard-stream client speaks
newline-delimited JSON-RPC, one request and then its answer. One pump
reads every line already waiting and then polls what is held, so a second
request sent before the first is answered can see the screen the first
has not yet changed. A JSON-RPC batch is refused. The schema check covers
type, properties, required, additionalProperties, enum, minimum and
maximum. `ui.wait` for an event hears the kinds the shared watch learns
at the pump. A loading's progress, which the automation endpoint sends
from its loading hook, is not one of those. `clock.run` until an event
stops when the match ends, or at `max_ticks` with an empty event list.
A run reports no unit events. Idle, finished orders, hurt and destroyed
units, finished builds, enemies seen and radar contacts are
`events.subscribe`'s, and `oa://events` keeps the last 4096 of them with
the screen and match notes. Each note's `tick` is the match's tick.
`seq` numbers the notes still kept, from 1. `next` in an answer is the
newest of those, or 0 when there are none. `events.read`'s `after` leaves
out notes at or before that seq. A subscription does not send the notes
already kept. The kinds are `screen`, `player`, `ready`, `chat`, `loaded`,
`match`, `pause`, `speed`, `alliance`, `transfer`, `game_over`,
`unit_created`, `unit_destroyed`, `unit_idle`, `unit_damaged`,
`order_done`, `build_done`, `enemy_seen` and `radar_contact`. `all` names
every one of them and is given on its own. A unit note names the unit, and
a radar contact names only its place.

HTTP serves one connection at a time. A second connection is answered
`409` and closed, including while the open connection is a held
server-sent event stream. A held answer is a server-sent event on the POST
that asked for it, so a client does not need a parallel GET; a GET that is
still open is that one connection, and a POST waits until it closes.
Server notifications are discarded when no event stream is open. The
address in `mcp.json` is the URL, and the file holds the bearer token,
not the session id.

`units.list` returns the units the local player is shown. Each unit has
its id, type (the unit name, such as ARMCOM), name, side, owner, position
in map pixels, health, build progress as a percent, flags (`selected`,
`radar`, `cloaked`) and its current order. `owner` is `me`, `all` or a
player from 0 to 9. `type` is a unit name. An area is `x`, `z` and
`radius` in map pixels, and the three are given together. `awareness`
is `player` or `full`. `full` includes units the player is not shown, and
only in a match that is not shared; asking for it there fails the call.
`units.get` and `units.orders` refuse a unit the player is not shown in
the same way. With no match running, each of these calls fails rather
than inventing an empty list.

`units.orders` returns the primary and secondary queues. An order's
`kind` is the mission's registered name. A build order's `build` is the
product's unit name and `parameter_2` is how many remain. `units.types`
is the catalogue of types, not the units on the field: build options,
weapons, maximum speed as the definition's 16.16 fixed value, and sight,
radar and sonar distances.

`orders.issue` gives move, patrol, attack, guard, repair, reclaim, build,
factory and stop. `append` adds the order behind the ones the unit already
has. A point is two whole map pixels, x and z, or three, x, y and z.
`orders.state` sets fire or move to 0, 1 or 2, or switches on, off or cloak.
`units.select` selects, and `add` keeps the units already selected; an empty
list clears it. Each answer names `applied_tick`. An order's answer names
each unit's queues in the same words as `units.orders`. A selection's answer
names the units selected afterwards. A stepped clock holds an order until a
step runs it, and the order's `applied_tick` is the tick that step runs.

`map.info` returns the map's name, its size in map pixels, the plot
counts, the sea level, and a height for every cell. `cell` is the cell
size in map pixels, 16 when omitted, one plot. `heights` has one entry
per cell, the height of the plot at the cell's centre. `metal` lists the
cells whose metal is not zero. `features` lists origin plots only, at
most 2500; `features_truncated` is set when there were more. A cell that
would make the height grid larger than 512 by 512 is refused.

`map.visibility` returns one player's `sight`, `explored`, `radar`,
`sonar` or `jammed` grid. `cell` is 32 when omitted, one sight cell, the
cell the minimap's fog is drawn from. A coarser cell is on when any sight
cell it covers is on. `format` is `rle` or `png`; `rle` is the default,
runs of a count and a 0 or 1. A PNG is grey, 255 where the cell is on,
and is also an image content block. A PNG of more than 256 by 256 cells
is refused. For sight and explored, `minimap` is the minimap's own fog
for those cells, as the same runs. Radar, sonar and jammed cells are
where a contact sits, not a coverage disk, and they have no `minimap`.
`player` is `me` or a player from 0 to 9. Another player's grid needs
`awareness` `full`, and only in a match that is not shared.

`map.radar` returns that player's radar and sonar contacts. Each contact
is a position in map pixels and whether it is radar, sonar or both. It
has no identity. `kept` is false when the match does not keep that
player's picture, and the contacts are then empty. Another player's
contacts need `awareness` `full`, on the same terms as the grids. With no
match running, each of these calls fails rather than inventing a map.
