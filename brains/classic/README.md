# Classic

Classic is the engine's own computer player, written in Lua. It plays exactly as the built-in computer player does: the same units land in the same squads, with the same standing orders, and each task comes due on the same tick.

Classic is an example and the documentation of that player. It is not the default. A seat plays Classic only when its package is this folder, and the built-in computer player stays the player a game gets when no brain is seated.

## Files

| File | What it does |
| --- | --- |
| `oabrain.yaml` | Manifest: id `classic`, levels easy, medium and hard, full awareness on. |
| `brain.lua` | The seat, the 30-tick sort countdown, and the ten squad tasks. |
| `lua/squads.lua` | The squad sort and the standing orders it writes. |
| `lua/types.lua` | The type fields the sort and the tasks read, with the player's byte cuts. |
| `lua/rules.lua` | The ai.* rules, using 3.1c's values where a hack is off. |
| `LICENSE` | The engine's licence. |
| `README.md` | This file. |

`brain.lua` keeps the seat on `hooks.seat`. The task bodies set the next tick and then return. Squad 5 runs the structures timetable only while `ai.squad5-factory-tick` is on; otherwise it is idle and marks no section.

## Sections

`onTick` marks `sort` when the countdown reaches the sort, then `task1` through `task9` for each task that is due, in squad order. An idle task marks nothing. The air raid and the siege draw their delay first in their own section, and that draw is the section's first call.

## How the shadow shows it

Seat this folder beside the built-in computer player and compare the `schedule` and `sort` sections. No difference means the sort runs on the same ticks, assigns the same units, writes the same standing orders, and the tasks come due together.

## Same behaviour in the engine

| Lua | Engine source |
| --- | --- |
| `brain.lua` | `src/sim/ai/src/computer_player.cpp` (`computer_player_create`, `computer_player_tick_orders`, and each task's next tick) |
| `lua/squads.lua` | `src/sim/ai/src/computer_player.cpp` (`sort_squads`, `computer_sort_squad`) and `src/sim/ai/src/computer_internal.hpp` (standing order values) |
| `lua/types.lua` | The type fields gathered for that player in `src/sim/match-runtime/src/computer_host.cpp` (`load_types`), read through the brain's unit definitions |
| `lua/rules.lua` | The ai.* values `src/sim/ai/src/computer_player.cpp` plays by |
