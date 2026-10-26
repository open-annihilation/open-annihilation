# Key Remaps

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `console.key-remaps` |
| Area | Console (`console`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Each machine, for its own view. |
| Parameters | 0 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

The backslash key no longer repeats the last console line, leaving it free for the whiteboard. Insert repeats the last line instead, and F10 becomes a second key for toggling the debug keys.

## Configuration example

```yaml
hacks:
  console.key-remaps: true
```

## Details

### Behaviour

With the hack on:

- **Backslash** does nothing in the console; the key is left to other uses, such as drawing on the [whiteboard](ui.whiteboard.md).
- **Insert** re-runs the last console line, as backslash does in 3.1c.
- **F10** toggles the debug keys, as F11 does. Ctrl+F10 keeps its own meaning.

F10 still needs the developer passphrase, as F11 does. Without it, F10 does nothing. Insert re-runs the last console line under the same rule as typing it: an option repeats with no passphrase, and a cheat repeats only while cheats are allowed.

### 3.1c behaviour

In 3.1c, after the developer passphrase, backslash re-runs the last console line and F11 toggles the debug keys. Insert and F10 alone do nothing.

### Network games

Keys act on the local machine only. The hack is part of the profile hash, so every machine has it on or off together.

### Interactions

- [ui.whiteboard](ui.whiteboard.md) draws while backslash is held; this hack keeps backslash from also repeating a console line.

### Implementation notes

- The app answers `game.repeat-command` and `debug.keys`. While this hack is on, the registry gives the first Insert and adds F10 to the second. The console still turns Insert into the repeat key, F10 into F11 and backslash into no key in `key_acted_on` (`src/ui/console/src/debug_hotkeys.cpp`).
- `console_apply_rules` (`src/ui/console/src/commands.cpp`) copies the rules record field `rules.console.key_remaps.enabled` into `Console::key_remaps`.
- Tests: `ui-console` (`src/ui/console/tests/console_test.cpp`, `test_key_remaps`: F10 needs the passphrase, Insert repeats a cheat only while cheats are allowed, backslash does nothing; without the hack Insert and F10 do nothing).

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `console.key-remaps`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `console.key-remaps: true` | On. |
| `console.key-remaps: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

This hack has no parameters.

### Presets

This hack has no presets.

### Shorthand

This hack has no parameters to set: write `true` or `false`.

### Every parameter at its default

```yaml
hacks:
  console.key-remaps: true
```
<!-- END GENERATED: schema -->
