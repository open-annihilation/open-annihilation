# Game Recorder

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `recorder.ta-demo-recorder` |
| Area | Recorder (`recorder`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Every machine in the game. |
| Parameters | 7 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

The game recorder is present: every network game is recorded to a file, and the engine takes part in the recorder traffic that recorder-equipped players exchange inside the game's packets. In 3.1c there is no recorder: games are not recorded and no recorder traffic is sent or read.

## Configuration example

```yaml
hacks:
  recorder.ta-demo-recorder: true
```

Give automatic recordings their own extension and write them uncompressed:

```yaml
hacks:
  recorder.ta-demo-recorder:
    auto-extension: .rec       # files recorded without a name end in .rec
    compress-files: false
```

## Details

**Presence.** With the hack on, the machine presents itself to the other players as a recorder peer: it sends recorder protocol version 6 in every lobby setup block, where 3.1c sends 0 and is a *plain peer*. Recorder peers then exchange recorder records inside the normal game traffic. A machine that presents no recorder neither sends nor reads them.

**Recording.** Every network game is recorded from its start, in the recorder's container (header version 5). The recording holds the recording machine's player first, then every other player's setup block and team, the battle room's unit checks and their verdicts, and every record sent or received, in order.

- A recording the player names with the `.record` chat command is written under that name plus `manual-extension`.
- Otherwise the file is named after the local date, time and map, plus `auto-extension`.
- Files go into the mod's folder in `Recordings` in the player's own Open Annihilation folder, `Recordings/<mod id>`, unless `--net-record FILE` names the file. `compress-files` compresses the recorded packets.

**Recorder traffic.**

- Camera positions: the machine sends its own camera and keeps the other players', for [ui.camera-sharing](ui.camera-sharing.md), whose page describes `.sharemappos`.
- The "warp done" message, which [setup.commander-warp](setup.commander-warp.md) waits for.
- The cheat report a peer's recorder sends. When a player's report first shows cheats and `cheat-notices` is true, the engine announces "*name* has cheats enabled".
- The host's lobby options, sent in the battle room to recorder peers.

A recorder peer may send the records of its first game frame twice. The engine drops a create record that repeats the unit already in that slot (same owner, type and ground position), so the owner keeps its unit.

**Plain peers.** Other players' recorders treat a peer that presents no recorder differently: for example, they wait for its "warp done" message and send it no markers or synced lobby options. Presenting a recorder keeps the engine in step with them.

**Network games.** Every machine runs the hack, and the recorder protocol travels on the wire, so every machine must agree whether it is on. `header-version` and `weapon-id-patch` are part of the profile hash. The file names, compression and notices change only what each machine writes and shows.

**Related hacks.** [network.recorder-session-commands](network.recorder-session-commands.md) (the recorder's chat commands), [sharing.recorder-take-give](sharing.recorder-take-give.md), [setup.recorder-prebuilt-base](setup.recorder-prebuilt-base.md), [ui.whiteboard](ui.whiteboard.md), [ui.camera-sharing](ui.camera-sharing.md), and [recorder.ten-player-replay](recorder.ten-player-replay.md) for watching a full recording.

### Implementation notes

- `wire_rules_of` (`src/app/netgame/wire_rules_binding.cpp`) turns `recorder.ta_demo_recorder.enabled` into `WireRules::recorder_protocol` and copies `cheat_notices` into `WireRules::recorder_cheat_notices` (`src/netgame/include/oa/netgame/wire_rules.hpp`).
- The match reads and sends recorder records in `src/netgame/match/src/net_match.cpp` and the battle room in `src/ui/frontend-multiplayer/src/battleroom.cpp`; the repeated first-frame create is dropped in `src/netgame/match/src/match_binding.cpp`.
- `start_recording` in `src/app/netgame/runtime_net.cpp` starts the recording and reads `compress_files`, `manual_extension` and `auto_extension`. The container is written by the demo session module, `src/session/demo` (`include/oa/session/demo/recording.hpp`).
- Tests: `every_rule_maps` in `src/app/netgame/tests/wire_rules_test.cpp` (ctest `netgame-wire-rules`); `recorder_records_reach_the_match` in `src/netgame/match/tests/net_match_test.cpp` (ctest `net-match`); `live_recording_round_trips` in `src/session/demo/tests/demo_playback_test.cpp` (ctest `demo-playback`); the profile tests in `src/data/mod-profile/tests/mod_profile_test.cpp` (ctest `data-mod-profile`).

**Known limits.**

- The engine runs no cheat scanner and never sends a cheat report of its own; it only reads its peers' reports.
- `header-version` is accepted and hashed, but the engine always writes version 5.
- `registry-root`, where the recorder keeps its own settings, is accepted and not used.
- `weapon-id-patch` (wider weapon ids in the recorder's records) is not supported. A mod that numbers weapons past 255 raises [`limits.weapon-types`](../oamod-standard.md#63-limits) instead. The patch is still accepted, hashed and noted on the console, nothing more.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `recorder.ta-demo-recorder`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `recorder.ta-demo-recorder: true` | On, every parameter at its default. |
| `recorder.ta-demo-recorder: {header-version: 5}` | On, the parameters named set and the rest at their defaults. |
| `recorder.ta-demo-recorder: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `header-version` | `int` | version | `1` to `7` | - | `5` | `5` | `fixed` | `sim` | - |
| `manual-extension` | `string` | - | matching `^\.[A-Za-z0-9]{1,8}$` | - | `".tad"` | `".tad"` | `fixed` | `view` | - |
| `auto-extension` | `string` | - | matching `^\.[A-Za-z0-9]{1,8}$` | - | `".tad"` | `".tad"` | `fixed` | `view` | - |
| `registry-root` | `string` | - | any text | - | `""` | `""` | `fixed` | `view` | - |
| `compress-files` | `bool` | - | `true`, `false` | - | `true` | `true` | `fixed` | `view` | - |
| `cheat-notices` | `bool` | - | `true`, `false` | - | `true` | `true` | `fixed` | `view` | - |
| `weapon-id-patch` | `bool` | - | `true`, `false` | - | `false` | `false` | `install` | `sim` | - |

Adjustable: `fixed`: only the profile sets it; `install`: the player's settings may set it when the profile binds it under `settings`.

- `weapon-id-patch` reads the settings-file value `Preferences/MultiGameWeapon` when the profile binds it under `settings`.

### Presets

This hack has no presets.

### Shorthand

This hack takes no shorthand: write `true`, `false` or a parameter map.

### Every parameter at its default

```yaml
hacks:
  recorder.ta-demo-recorder:
    header-version: 5
    manual-extension: ".tad"
    auto-extension: ".tad"
    registry-root: ""
    compress-files: true
    cheat-notices: true
    weapon-id-patch: false
```
<!-- END GENERATED: schema -->
