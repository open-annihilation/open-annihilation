# Extension

The piece the game links. It takes `-y`, and `-c` when that names the online
module. A launch file's `-c` is left for network play. It keeps the launch
and the exit box, and installs the page fonts.

## Entry points

`oa_extension_init_boneyards` in `boneyards_extension.cpp`. The library is
`oa-boneyards-extension`. `boneyards_options.cpp`, `boneyards_launch.cpp` and
`boneyards_exit_prompt.cpp` are the switch, the launch and the exit box.

## State

Reads the engine's runtime, the network-play launch block and the player's
preferences. Writes the launch bridge, the exit box and the page font
service. A run that never opens the overlay leaves the engine's saved,
sent and recorded bytes as they were.

## Invariants

The extension table followed is 19 and network play's API is 4. Hooks added
in versions 15 to 19 are not filled. The library links `oa-app-netgame-sdk`,
so it is asked before network play. Network play's `recorded_game` hook
(`boneyards_recorded_game`) names the kind `boneyards` for a battle the
Boneyards launched, with its game id, its planet and its sector as the
members of the object under `game` in the battle's replay; the planet and the
sector are the galactic map's names when the launch reply came (the status
widget's `boneyards_status_widget_battle_place`). A member the map does not
name is left out. As the table is filled, `register_boneyards_replay_kind`
registers that kind with the Library's Replays section
(`oa::data::replays::register_kind`) under the title `Boneyards`, which
`languages/boneyards.tdf` translates: the section's kind menu offers the
Boneyards' battles, and a battle's row shows the title and its planet, its
card the planet and the sector.

## Tests

`boneyards-options`, `boneyards-launch` (which also checks that the hook's
game id, planet and sector reach a replay's `meta.json`), `boneyards-exit-prompt`,
`boneyards-entry-flow` (the entry's decisions before the package), and the
`native-boneyards-*` checks. `native-boneyards-launch` records each
side's battle and requires that replay to name the battle's game id, planet and
sector. Launch checks read `OA_LAUNCHER_GAME_DIR`.

## Limitations

The menu check, the `--check-*` dispatch (`boneyards_self_checks.cpp`) and
the launch-check library are linked into this library only when the game's
self-checks are built. The dispatch includes the checks' header through the
link to `oa-boneyards-checks`. A release build compiles none of them.
