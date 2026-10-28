# Display Modes

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `ui.display-modes` |
| Area | Interface (`ui`) |
| Scope | view: local display only; each player may differ |
| Runs on | Each machine, for its own view. |
| Parameters | 2 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

The options screen offers only display modes of at least 768 rows and the
display mode starts at 1024 by 768, and Ctrl+F9 or Print Screen saves a
screenshot named after the date, the map and the players. 3.1c offers modes
from 640 by 480 and starts there.

## Configuration example

```yaml
hacks:
  ui.display-modes: true
```

```yaml
hacks:
  # Keep 3.1c's smallest modes, but still name the screenshots.
  ui.display-modes:
    min-height-768: false
```

## Details

### Display modes

With `min-height-768`:

- The options screen lists only modes of 768 rows or more, and still none
  narrower than 640 pixels. 3.1c lists modes from 480 rows. The Open
  Annihilation settings' Screen size offers the same sizes after Desktop.
- The display mode settings, `DisplaymodeWidth` and `DisplaymodeHeight`,
  start at 1024 by 768 when they are not set, and a smaller stored width or
  height is raised to it as it is read. 3.1c starts at 640 by 480.

Without it, the modes and their default are 3.1c's.

### Screenshots

- Ctrl+F9 and Print Screen save a screenshot when the key is released, on
  every screen. The keys are taken before the screen sees them.
- The frame is saved as an 8-bit PCX file in the `screenshots` folder of the
  player's data folder.
- The file name is made of, in order:
  1. the date as the player's clock shows it (month, day and the year's last
     two digits) and " - ";
  2. during a match, the map's name, " - ", the first slot's player name
     (even an empty one), ", " with each later non-empty name, and a space;
     outside a match, `SHOT`;
  3. the first unused number from 0, with at least four digits, and `.pcx`.

  Every character a file name may not hold (`\ / : * ? " < > |`) in the
  date, the map and the names becomes `_`. For example:
  `10_03_26 - SHOT0000.pcx`.
- When the frame cannot be captured, the status line says "error writing
  screenshot".

### Graphics-driver warning

`dx-warning` keeps (true) or drops (false) a warning about the graphics
driver. The engine never shows that warning, so both values play alike.

A profile that wants to force a resolution gives it as a registry seed for
the display mode settings (see
[the OA MOD standard](../oamod-standard.md#54-settings-bindings-and-registry-seeds)).

### Network games

The hack changes only this machine's display. Nothing needs to agree
between machines.

### Implementation notes

- The application reads `ModProfile::ui.display_modes` (`enabled`,
  `min_height_768`, `dx_warning`).
- `minimum_mode_height`, `display_mode_setting` and `screenshot_file_name`
  are in [src/app/view_rules.cpp](../../../src/app/view_rules.cpp). The
  mode list filter is in
  [src/present/world-renderer/src/display_modes.cpp](../../../src/present/world-renderer/src/display_modes.cpp)
  (`tall_minimum_mode_height`), and the setting's default and floor in the
  preferences loader of
  [src/ui/frontend-state](../../../src/ui/frontend-state)
  (`tall_display_mode_setting`). The screenshot keys are
  `Runtime::handle_view_rule_key` and `Runtime::capture_named_screenshot` in
  [src/app/runtime_view_rules.cpp](../../../src/app/runtime_view_rules.cpp).
- Tests: `app-view-rules` (`display_modes_follow_the_profile`,
  `screenshot_names`), `world-overlays` (the mode list),
  `frontend-initialization` (the default and the raised setting).

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `ui.display-modes`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `ui.display-modes: true` | On, every parameter at its default. |
| `ui.display-modes: {min-height-768: true}` | On, the parameters named set and the rest at their defaults. |
| `ui.display-modes: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `ui.display-modes: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `min-height-768` | `bool` | - | `true`, `false` | - | `false` | `true` | `fixed` | `view` | - |
| `dx-warning` | `bool` | - | `true`, `false` | - | `true` | `true` | `fixed` | `view` | - |

Adjustable: `fixed`: only the profile sets it.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{min-height-768: false, dx-warning: true}` |

`baseline` sets every parameter to its 3.1c value: the hack is on and plays as 3.1c.

### Shorthand

This hack takes no shorthand: write `true`, `false` or a parameter map.

### Every parameter at its default

```yaml
hacks:
  ui.display-modes:
    min-height-768: true
    dx-warning: true
```
<!-- END GENERATED: schema -->

## Keys

<!-- BEGIN GENERATED: keys -->
| Action | Default chords | What it does |
| --- | --- | --- |
| `game.screenshot` | `Ctrl+F9, PrintScreen` | Saves a screenshot |
<!-- END GENERATED: keys -->

These keys move in the [key editor](../../keys.md#the-key-editor), in **Game**, while this hack is on.
