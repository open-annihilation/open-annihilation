# Key table

`--check-key-table FILE` starts a headless skirmish, plays every row of `FILE` through `Runtime::dispatch_event`, and compares the effect of each row with the file. The check writes `key-table-observed.tsv` in the working directory, including when a row does not match. It writes any saved toggle back before it exits, so the preferences file is the same after a run as before it.

`base.tsv` is today's game, with no mod: 220 rows. `hacks.tsv` plays those rows again with the twelve catalogue hacks turned on, then the rows that exist only while those hacks answer: 254 rows. Where a hack's action replaces the base action, the hacks row is named by the replacing action. A migration ticket that changes a row updates the table in that ticket and says why in its report.

## Columns

The header is `row`, `setup`, `input`, `effect`, separated by tabs.

`row` is a catalogue action id plus a variant after `:`. Variants of one action share its stem. The id is unique in the file.

`setup` is `;`-joined words applied after the match is put back to one start, and before the effect is read:

- `select=none|commander|builder|factory|aircraft|combat|all`
- `interface=left-click|right-click` (every `pointer.` row names one)
- `switch-alt=on`
- `wheel-zoom=off`
- `armed=build|attack`
- `chat=open`
- `menu=open`
- `unit-info=open`
- `shared`
- `watcher`
- `developer`
- `debug-keys`
- `marker=received`
- `platform=macos` (played only on macOS; skipped elsewhere, and not counted)
- `escape-menu` (Escape opens the game menu)

`input` is one of:

- `key <scancode> [ctrl] [alt] [shift] [primary] [repeat] [held=<scancode>]`
- `hold <scancode> <milliseconds>`
- `button <left|right|middle> <place> [clicks=2] [ctrl] [alt] [shift] [primary] [held=<scancode>]`
- `drag <left|right> battlefield [held=<scancode>] [modifiers]`
- `wheel <up|down> <battlefield|megamap> [modifiers]`

`primary` is Command on macOS and Ctrl elsewhere. `held=` on a key or a button is a grammar extension: the named key is down for the gesture (a build tool, or the whiteboard key). Places are `battlefield`, `unit`, `minimap`, `build-button`, `queued-site`, and `edge-left`, `edge-right`, `edge-top`, `edge-bottom`. The edge rows still say `button` so they stay in the grammar; the check moves the pointer to that outermost window pixel and does not press, because a click there would land on a gadget. Those rows are `camera.edge-scroll`. `pointer.pan` is the middle button.

`effect` is `;`-joined `name=value` of the probes that changed, sorted by name, or `none`. Probes are counts, roles, and on/off words. They are never a function name, a unit name, a status line, or a map coordinate.

## How to regenerate

From a build with the self-checks, in an empty working directory:

```
open-annihilation --game-dir "$OA_GAME_DIR" --skip-intro --mute \
  --check-key-table src/app/key-table/base.tsv \
  --preferences-file /tmp/K03-key.conf
```

Copy `key-table-observed.tsv` over `base.tsv` only after reading every row against the code. Repeat with `--mod` pointing at a profile that turns on `ui.megamap`, `ui.whiteboard`, `ui.resource-panel`, `ui.selection-shortcuts`, `ui.build-tools`, `ui.click-snap`, `ui.build-preview`, `ui.options-dialog`, `ui.display-modes`, `console.key-remaps`, `teams.alliance-menu-all-game-types`, and `units.build-rotation`, and copy that observed file over `hacks.tsv`.

The check puts the match back to one start before every row, so a row's effect does not depend on the row before it. `chat.send` types the letter `a` and then the input key; the letter is not a column. Squad 1, and every other select or add digit except `group.select-2`, is assigned to the commander and the selection is cleared again before the row, so those rows recall a squad that exists. `group.select-2` is the empty squad and clears the selection. A wheel whose place is `megamap` opens the megamap as setup. `armed=attack` presses A, and `armed=build` presses a build button, before the effect is read. `build.move-queued-site` shift-clicks a build onto the queued site before the drag. `whiteboard.wipe` starts with one line on the battlefield, so the right drag has a mark to remove. `whiteboard.move` and `whiteboard.erase` start with one marker. `O` is `panel.orders-tab` and `G` is `order.guard`: those stems name the panel and the guard order the keys give today. A build click records `site=free` when the ground under it is not a metal spot. Home with no selection selects the commander; the start view is already on the commander, so that row may name no `camera` probe.

Digits 2 to 9 are one row each, set up as digit 1 is: `group.assign-<n>`, `group.select-<n>`, `group.add-<n>` and `build.page-<n>`. Selecting the commander already shows build page 2, so `build.page-2` records `none`. Pages 3, 4 and 5 move the page. Pages 6 to 9 do not move a page the commander does not have, so those rows are `none`.

## Rows left out

These catalogue rows are new or not routed today, so they are not in the tables: `order.cancel`, `select.clear`, `camera.zoom-in-key`, `camera.zoom-out-key`, `camera.zoom-reset`, `build.cell-*`, `chat.open-allies`, `ui.keys-overlay`, `ui.release-pointer`, `megamap.jump`. `select.category-*` is in the tables for a letter the loaded units tag and no named Ctrl action holds (`select.category-w` for `CTRL_W`). `camera.bookmark-*` and `game.health-bars-mode-*` are in the tables too. A bookmark return stores the view, moves it, then recalls, so its effect is the return. The main menu's quick keys, and touch and pad input, have their own checks.

`ui.quit` is the system's own quit and ends the process, so the table does not play it.

`order.force-modifier`, `order.queue-modifier` and `select.add-modifier` have no effect alone. They are recorded as the `:ctrl` and `:shift` variants of the rows they change: `order.attack:ctrl` for the force modifier, `order.attack:shift` and `order.move:shift` for the queue modifier, and `select.builders:shift` for the add modifier (`select.idle-builders:shift` in `hacks.tsv`).

`game.resource-panel` hides the panel only when it has rows. This skirmish has none, so F4 stays `game.kill-board`.

`watch.player-view` and `watch.lock-view` are a double-click on a resource-panel row, and that click is answered only while the local player is a watcher. This skirmish's commander is not one. The check's `watcher` word sets extension state for the speed keys and does not seat the player as a watcher. Locking and unlocking that camera return without moving the view, so `watch.lock-view` would not change a probe either.

`camera.classic-view`, `camera.unlocked`, `pointer.camera-orbit` and `pointer.camera-pan` are catalogue actions and are in both tables. Their effect is `none`: the Full 3D camera's pose and the unlocked flag are not probes. `build.move-queued-site` is in both tables and is `none` because the queued site's cell is a coordinate. `whiteboard.draw` is the held backslash and is `none`. `whiteboard.move` is `none` because a marker's new cell is not a probe. `whiteboard.erase` records `whiteboard=wiped`.

Chat's Up and Down are not catalogue actions. They are `chat.open:up` and `chat.open:down`, and the line stays open. B and Y box-select by the same filters as W. The catalogue names `select.box-combat`, `select.box-builders` and `select.box-factories`; those three rows are in `hacks.tsv`, where the filters answer.

Zoom level, a refilled store, the debug overlay, the debug info line, a pinned unit, build spacing, the status strip, and repeating an empty console line are not probes. Those rows are `none` when nothing else changed. A wheel on the battlefield does not move the view's x or z on its own, so its effect is `none` unless the megamap opens or closes. Home and N say `camera=centred` when the view jumps on both axes. A full-screen switch or the settings column can clamp the same view into a narrower battlefield; that clamp follows the display, so it is not a `camera` probe. A box drag that covers no unit, a building that can face only one way, and build spacing leave those rows at `none`. With the twelve hacks on, Ctrl+B, Ctrl+F and Ctrl+S take the hack's action, and Tab opens the team menu, so those rows record the new effect.
