# Several Local AI Players

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `setup.multiple-local-ai` |
| Area | Game Setup (`setup`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | The host's machine. |
| Parameters | 0 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

OA's battle room seats several computer players for each human, up to the
free slots. The hack remains so a profile that names it still loads; the
battle room no longer reads it. 3.1c's own battle room seats one computer
player per machine, and accepts any number another machine seats.

## Configuration example

```yaml
hacks:
  setup.multiple-local-ai: true
```

The hack has no parameters.

## Details

### Behaviour

In the battle room, clicking a blocked slot's player control seats a
computer player there, whatever the count already seated. The other
conditions are unchanged: the game must not be closed, and the commander
option must not be deathmatch. The hack is not consulted.

### Baseline

3.1c's battle room seats at most one computer player per machine. Another
machine's extra computer players are ordinary players of that machine, and
3.1c plays with them.

### Network games

The check runs on the machine that adds the computer player. Other machines
take any number of computer players from a remote machine. The hack is part
of the profile hash, so every machine plays under the same profile.

### Interactions

- [setup.ai-player-name-format](setup.ai-player-name-format.md) gives each
  computer player a distinct name.
- [setup.allow-start-with-ai](setup.allow-start-with-ai.md), now retired:
  one human starts against the computer players without it.
- [setup.map-scripted-units](setup.map-scripted-units.md) may seat a neutral
  computer player as the host presses START.

### Implementation notes

- `handle_panel_event` in `src/ui/frontend-multiplayer/src/battleroom.cpp`
  seats another computer player without reading `setup.multiple_local_ai`.
  The profile still parses the hack.
- Test: `ui-multiplayer-lobby-rules` (`test_several_computers`) seats two
  computer players with the hack on and with it off.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `setup.multiple-local-ai`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `setup.multiple-local-ai: true` | On. |
| `setup.multiple-local-ai: false` | Off, the same as leaving it out. OA's battle room still seats several computer players; the value is kept so a profile that names the hack still loads. |

### Parameters

This hack has no parameters.

### Presets

This hack has no presets.

### Shorthand

This hack has no parameters to set: write `true` or `false`.

### Every parameter at its default

```yaml
hacks:
  setup.multiple-local-ai: true
```
<!-- END GENERATED: schema -->
