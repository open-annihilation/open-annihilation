# Start with One Human

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `setup.allow-start-with-ai` |
| Area | Game Setup (`setup`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | The host's machine. |
| Parameters | 0 |
| Status | retired: a profile that names it still loads, and it changes nothing |
<!-- END GENERATED: facts -->

## Description

Retired. Open Annihilation's battle room lets the host start a
multiplayer game with only one human player and computer players, with or
without this hack. A profile that names it still loads, and keeps the
hashes it had, but the hack changes nothing. Developer Mode no longer
lists it, and an override of it kept in the settings is left out.

## Configuration example

```yaml
hacks:
  setup.allow-start-with-ai: true
```

The hack has no parameters. A profile may leave it out.

## Details

### Behaviour

The battle room's start check first asks for an opponent: at least one
remote player or computer player. While a launch locks the battle room's
options (`rules.lock-options` in a launch file, `--lock-options`, or a
launcher's own lock; see
[Launch settings](../../command-line.md#launch-settings)), the room keeps
3.1c's rule instead and waits for a remote player: the launch arranged its
players.

The rest of the check is unchanged: every local, computer and remote player
must be ready, and a game whose players are all watching does not start.
One human with one or more ready computer players can therefore start, and
one human alone cannot.

### Baseline

3.1c needs a remote player in the battle room before the start is allowed,
so a game of one human against computer players is only possible as a
skirmish.

### Network games

The host runs the start check. The hack stays part of the profile hash, so
a profile that names it plays with the machines it played with before.

### Interactions

[setup.multiple-local-ai](setup.multiple-local-ai.md) lets the one human seat
more than one computer player.

### Implementation notes

- `lobby_ready_to_start` in `src/ui/frontend-multiplayer/src/battleroom.cpp`
  counts computer players unless `lobby_options_locked`, and does not read
  `setup.allow_start_with_ai`. The profile still parses the hack.
- `retired: true` in the registry keeps the hack out of `standard_hacks`
  and of the overrides `read_overrides` gives and the resolver lays.
- Tests: `ui-multiplayer-lobby-rules` (`test_start_with_computers`) checks
  one human alone, then with a computer player, then with the options
  locked and a remote player, with the hack off and on and without a
  profile; `data-mod-profile` (`test_retired_hack`) checks a profile that
  names it and its override.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `setup.allow-start-with-ai`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `setup.allow-start-with-ai: true` | Retired: it changes nothing. OA's battle room lets one human start against computer players unless a launch locks the options; the value is kept so a profile that names the hack still loads. |
| `setup.allow-start-with-ai: false` | Off, the same as leaving it out. OA's battle room lets one human start against computer players unless a launch locks the options; the value is kept so a profile that names the hack still loads. |

### Parameters

This hack has no parameters.

### Presets

This hack has no presets.

### Shorthand

This hack has no parameters to set: write `true` or `false`.

### Every parameter at its default

```yaml
hacks:
  setup.allow-start-with-ai: true
```
<!-- END GENERATED: schema -->
