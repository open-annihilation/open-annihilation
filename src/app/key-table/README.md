# Key table

`--check-key-table FILE` starts a headless skirmish, plays every row of `FILE` through `Runtime::dispatch_event`, and compares the effect of each row with the file. The check writes `key-table-observed.tsv` in the working directory, including when a row does not match.

`base.tsv` is today's game, with no mod. `hacks.tsv` is the same rows again, played with the twelve catalogue hacks turned on, then the rows that exist only while those hacks answer. A migration ticket that changes a row updates the table in that ticket and says why in its report.

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

`primary` is Command on macOS and Ctrl elsewhere. `held=` on a key or a button is a grammar extension: the named key is down for the gesture (a build tool, or the whiteboard key). Places are `battlefield`, `unit`, `minimap`, `build-button`, `queued-site`, and `edge-left`, `edge-right`, `edge-top`, `edge-bottom`. The edge rows still say `button` so they stay in the grammar; the check moves the pointer to that outermost window pixel and does not press, because a click there would land on a gadget.

`effect` is `;`-joined `name=value` of the probes that changed, sorted by name, or `none`. Probes are counts, roles, and on/off words. They are never a function name, a unit name, a status line, or a map coordinate.

## How to regenerate

From a build with the self-checks, in an empty working directory:

```
open-annihilation --game-dir "$OA_GAME_DIR" --skip-intro --mute \
  --check-key-table src/app/key-table/base.tsv \
  --preferences-file /tmp/K03-key.conf
```

Copy `key-table-observed.tsv` over `base.tsv` only after reading every row against the code. Repeat with `--mod` pointing at a profile that turns on `ui.megamap`, `ui.whiteboard`, `ui.resource-panel`, `ui.selection-shortcuts`, `ui.build-tools`, `ui.click-snap`, `ui.build-preview`, `ui.options-dialog`, `ui.display-modes`, `console.key-remaps`, `teams.alliance-menu-all-game-types`, and `units.build-rotation`, and copy that observed file over `hacks.tsv`.

The check puts the match back to one start before every row, so a row's effect does not depend on the row before it. `chat.send` types the letter `a` and then the input key; the letter is not a column. Squad 1 is assigned to the commander and the selection is cleared again before `group.select-1` and `group.add-1` (and their variants), so those rows recall a squad that exists. A wheel whose place is `megamap` opens the megamap as setup. `armed=attack` presses A, and `armed=build` presses a build button, before the effect is read. `whiteboard.draw:wipe` starts with one line on the battlefield, so the right drag has a mark to remove. `O` is `panel.orders` and `G` is `order.defend`: those stems name the panel and the guard order the keys give today. A build click records `site=free` when the ground under it is not a metal spot. Home with no selection selects the commander; the start view is already on the commander, so that row may name no `camera` probe.

## Rows left out

These catalogue rows are new or not routed today, so they are not in the tables: `order.cancel`, `select.clear`, `select.category-*`, `camera.bookmark-*`, `camera.zoom-in-key`, `camera.zoom-out-key`, `camera.zoom-reset`, `build.cell-*`, `game.health-bars-mode-*`, `chat.open-allies`, `ui.keys-overlay`, `ui.release-pointer`, `megamap.jump`, `ui.quit`. The main menu's quick keys, and touch and pad input, have their own checks.

`camera.classic-view` (Alt+R) and `camera.unlocked` (Alt+U) are answered today, and the catalogue does not name them until a later ticket adds them. `pointer.camera-orbit` and `pointer.camera-pan` are the same: the catalogue does not name them yet, so the drag that orbits or pans is not a row. The four edge rows use `pointer.pan` because that stem is a catalogue action; the edge names are variants.

Chat's Up and Down are not catalogue actions. They are `chat.open:up` and `chat.open:down`, and the line stays open. B and Y box-select by the same filters as W. The catalogue names `select.box-combat` for W; the tables call the other two `select.box-constructors` and `select.box-factories`.

Zoom level, a refilled store, the debug overlay, the debug info line, a pinned unit, build spacing, the status strip, and repeating an empty console line are not probes. Those rows are `none` when nothing else changed. A wheel on the battlefield does not move the view's x or z on its own, so its effect is `none` unless the megamap opens or closes. Home and N say `camera=centred` when the view jumps on both axes. A full-screen switch or the settings column can clamp the same view into a narrower battlefield; that clamp follows the display, so it is not a `camera` probe. A box drag that covers no unit, a building that can face only one way, and build spacing leave those rows at `none`. With the twelve hacks on, Ctrl+B selects idle builders and Tab opens the team menu, so those base rows record the new effect.
