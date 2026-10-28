# Model Composite

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Limit id | `limits.model-composite` |
| Written as | `model-composite` under `limits` |
| Area | Limits (`limits`) |
| Scope | view: local display only; each player may differ |
| Runs on | Each machine, for its own view. |
| Parameters | 3 |
<!-- END GENERATED: facts -->

## Description

The size of the buffer a unit's moving pieces, build effect and carried
units are drawn into. `width` and `height` are the buffer's size in the
game's pixels. Without `clamp-oversize` the buffer grows to fit a larger
model, as in 3.1c, which starts at 600 by 600. With `clamp-oversize` the
buffer keeps its size and a larger model is cut to it from its top left,
and so are its build outline and the units it carries.

With enhanced anti-aliasing a model is drawn finer into a buffer as many
times larger, so a model that fits is drawn whole at every level. The
default is 1,280 by 1,280 with clamping on.

## Configuration example

Written under `limits`. The `limits.` prefix of the registry id is dropped.

```yaml
limits:
  model-composite:
    width: 1280
    height: 1280
    clamp-oversize: true
```

`{preset: baseline}` is 3.1c: 600 by 600, and the buffer grows. There is
no shorthand.

## Details

### Network games

The limit is view. It stays out of the sim hash, and each player may set
it differently. It changes only the picture on this machine. Raising it
does not make the profile OA-only
([12.3](../oamod-standard.md#123-wire-behaviour-a-profile-selects)).

### When data goes past it

A model larger than the buffer grows the buffer, unless `clamp-oversize`
is on, in which case the model is cut to the buffer from its top left.
The engine writes no log line for a model that does not fit.

### Related

- [`effects`](limits.effects.md) sizes the emitter pool, which is the other
  view limit.
- [`ui.display-modes`](ui.display-modes.md) changes the screen. It does not
  change this buffer.

### Settings

Every parameter is fixed. Only the profile sets the size and the clamp.
No INI key and no registry value adjusts them.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Limit id `limits.model-composite`, written as `model-composite` under the profile's `limits` block. The `limits.` prefix of the registry id is dropped there.

| Write | Means |
| --- | --- |
| `model-composite: true` | On, every parameter at its default. |
| `model-composite: {width: 1280}` | On, the parameters named set and the rest at their defaults. |
| `model-composite: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `model-composite: false` | Off, the same as leaving it out: every parameter at its 3.1c baseline. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `width` | `int` | pixels | `64` to `8192` | - | `600` | `1280` | `fixed` | `view` | - |
| `height` | `int` | pixels | `64` to `8192` | - | `600` | `1280` | `fixed` | `view` | - |
| `clamp-oversize` | `bool` | - | `true`, `false` | - | `false` | `true` | `fixed` | `view` | - |

Adjustable: `fixed`: only the profile sets it.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{width: 600, height: 600, clamp-oversize: false}` |

`baseline` sets every parameter to its 3.1c value: the limit is on and plays as 3.1c.

### Shorthand

This limit takes no shorthand: write `true`, `false` or a parameter map.

### Every parameter at its default

```yaml
limits:
  model-composite:
    width: 1280
    height: 1280
    clamp-oversize: true
```
<!-- END GENERATED: schema -->
