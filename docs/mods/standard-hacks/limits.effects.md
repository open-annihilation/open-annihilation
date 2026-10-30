# Effects

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Limit id | `limits.effects` |
| Written as | `effects` under `limits` |
| Area | Limits (`limits`) |
| Scope | view: local display only; each player may differ |
| Runs on | Each machine, for its own view. |
| Parameters | 2 |
<!-- END GENERATED: facts -->

## Description

How many effect emitters the game may draw: smoke, wakes, nano spray,
flames and the teleport trail. `queue` is how many emitters each layer
holds before another spawn drops that layer's oldest. A layer holds one
more than `queue` at most. `reserve` is how many emitter slots the shared
pool hands out, across every layer. A spawn that finds none free creates
nothing.

In 3.1c each layer evicts past 400 emitters, and the pool holds 1,000.
When nothing sets `reserve`, it is ten times `queue`.

## Configuration example

Written under `limits`. The `limits.` prefix of the registry id is dropped.

```yaml
limits:
  effects:
    queue: 20480
    reserve: 204800
```

Leave `reserve` out and it follows `queue`. `{preset: baseline}` is 3.1c's
400 and 1,000. There is no shorthand.

## Details

### Network games

The limit is view. It stays out of the sim hash, and each player may set
it differently. It changes what this machine draws. Raising it does not
make the profile OA-only
([12.3](../oamod-standard.md#123-wire-behaviour-a-profile-selects)).

### When data goes past it

Past `queue` emitters on a layer, the oldest is dropped to make room. Past
`reserve` free slots, the spawn creates nothing. The engine writes no log
line for either.

### Related

- [`ui.effects-tweaks`](ui.effects-tweaks.md) and
  [`ui.explosion-flash`](ui.explosion-flash.md) change how explosions are
  drawn. They do not change how many emitters a layer may hold.
- [`model-composite`](limits.model-composite.md) is the other view limit
  that sizes a drawing buffer.

### Settings

`queue` reads the INI key `Preferences/SfxLimit` when the profile binds
`limits.effects.queue` under `settings`. `reserve` is fixed. When the
profile does not set it, it is ten times the resolved `queue`, including a
`queue` that came from the INI key.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Limit id `limits.effects`, written as `effects` under the profile's `limits` block. The `limits.` prefix of the registry id is dropped there.

| Write | Means |
| --- | --- |
| `effects: true` | On, every parameter at its default. |
| `effects: {queue: 20480}` | On, the parameters named set and the rest at their defaults. |
| `effects: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `effects: false` | Off, the same as leaving it out: every parameter at its 3.1c baseline. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `queue` | `int` | effects | `1` to `1000000` | - | `400` | `20480` | `install` | `view` | - |
| `reserve` | `int` | effects | `1` to `10000000` | - | `1000` | `204800` | `fixed` | `view` | - |

Adjustable: `fixed`: only the profile sets it; `install`: the player's settings may set it when the profile binds it under `settings`.

- `queue` reads the settings-file value `Preferences/SfxLimit` when the profile binds it under `settings`.
- When nothing sets `reserve`, it is 10 times `queue`.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{queue: 400, reserve: 1000}` |

`baseline` sets every parameter to its 3.1c value: the limit is on and plays as 3.1c.

### Shorthand

This limit takes no shorthand: write `true`, `false` or a parameter map.

### Every parameter at its default

```yaml
limits:
  effects:
    queue: 20480
    reserve: 204800
```
<!-- END GENERATED: schema -->
