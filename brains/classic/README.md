# Classic

Classic is the engine's own computer player, written in Lua. It plays exactly as the built-in computer player does: the same units land in the same squads, with the same standing orders, and each task comes due on the same tick.

Classic is an example and the documentation of that player. It is not the default. A seat plays Classic only when its package is this folder, and the built-in computer player stays the player a game gets when no brain is seated.

## Files

| File | What it does |
| --- | --- |
| `oabrain.yaml` | Manifest: id `classic`, levels easy, medium and hard, full awareness on. |
| `brain.lua` | The seat, the 30-tick sort countdown, and the ten squad tasks. |
| `lua/report.lua` | The console weight report for the seat. |
| `lua/squads.lua` | The squad sort and the standing orders it writes. |
| `lua/types.lua` | The type fields the sort and the tasks read, with the player's byte cuts. |
| `lua/rules.lua` | The ai.* rules, using 3.1c's values where a hack is off. |
| `LICENSE` | The engine's licence. |
| `README.md` | This file. |

`brain.lua` keeps the seat on `hooks.seat`. The task bodies set the next tick and then return. Squad 5 runs the structures timetable only while `ai.squad5-factory-tick` is on; otherwise it is idle and marks no section.

A second start on a seat that has already started reloads the profile: the weight and limit tables are reset, the profile is applied again, and the grids, tasks and metal spots stay. That start draws nothing. A loaded game has no saved seat, so start builds the seat again from the units, the same way a new game does. `hooks.report` returns the weight report.

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

## How Classic is checked

The shadow seats this folder beside the built-in player and compares every section. `sim-brains-classic` does that on the computer-player fake, and `native-brain-shadow-classic` does it in a skirmish when the game data is present.

The twins are the comment table at the top of `src/sim/brains/tests/classic_test.cpp`. Each case of the computer-player tests has a Classic case of the same name, except `test_controller_presence`, which tests the engine's seating rule. `test_installed_data` has a twin of the same name in `classic_profile.cpp`.

The swap plays the pinned computer-player checks twice, once by the built-in player and once with Classic in every computer seat. `sim-brains-classic-match` runs the match-level build cases, the three rule sets and each ai.* hack on its own, and requires the same random stream and the same world digest after every tick. With the game data, `native-classic-trace`, `native-classic-stage` and `native-classic-director-render` repeat the trace skirmish, the stage and the director render. `native-net-loopback-computer-classic`, `native-net-loopback-computer-watcher-classic` and `native-net-loopback-each-ai-classic` repeat the three loopback computer checks. Classic is seated by the default-brain preference for that run. The engine's default stays the built-in player.

The soak plays a thousand games. It is not part of this package's check yet.

Classic may spend 2,000,000 instructions in a tick, and the unused part is kept for up to 30 ticks. `sim-brains-classic-match` prints the largest tick spend and the bank's lowest point over its runs:

```
classic match: largest tick N instructions, lowest bank M
```
