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

The extension table followed is 19 and network play's API is 2. Hooks added
in versions 15 to 19 are not filled. The library links `oa-app-netgame-sdk`,
so it is asked before network play.

## Tests

`boneyards-options`, `boneyards-launch`, `boneyards-exit-prompt`,
`boneyards-entry-flow` (the entry's decisions before the package), and the
`native-boneyards-*` checks. Launch checks read `OA_LAUNCHER_GAME_DIR`.

## Limitations

The menu check, the `--check-*` dispatch (`boneyards_self_checks.cpp`) and
the launch-check library are linked into this library only when the game's
self-checks are built. The dispatch includes the checks' header through the
link to `oa-boneyards-checks`. A release build compiles none of them.
