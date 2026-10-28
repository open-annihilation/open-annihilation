# Units per Player

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Limit id | `limits.units-per-player` |
| Written as | `units-per-player` under `limits` |
| Area | Limits (`limits`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | The host's machine, and every machine in the game. |
| Parameters | 4 |
<!-- END GENERATED: facts -->

## Description

How many units each player may own. `default` is the limit a game starts
with. `min` and `max` bound the limits a player or a host may choose, and
`default` is held between them. In 3.1c a game starts at 250 units per
player, chosen from 20 to 500. The unit slots and the highest unit id
follow the limit the game plays with.

## Configuration example

Written under `limits`. The `limits.` prefix of the registry id is dropped.

```yaml
limits:
  units-per-player:
    default: 1500
    min: 20
    max: 1500
```

`true` uses every default, including a fallback of 1,500.
`{preset: baseline}` plays 3.1c's 250, from 20 to 500, with a fallback of
101. There is no shorthand.

## Details

### Network games

The limit is part of the profile's sim hash, so every machine must play the
same values. Raising it does not make the profile OA-only. A profile is
OA-only only when [`weapon-types`](limits.weapon-types.md) is above 256 or
[`sides`](limits.sides.md) is above 5
([12.3](../oamod-standard.md#123-wire-behaviour-a-profile-selects)).

`default` is a match parameter. The host may change it for one game, and a
mutator may set it, because the registry marks this limit `mutator: true`.
A mutator value is held between this run's `min` and `max`. In a network
game the unit state a machine sends comes round on that limit: a full
record of a unit rides every `units-per-player` ticks.

### When data goes past it

This limit counts units a player may own, not files. The engine writes no
line because a unit file was too large. A mutator that asks for a `default`
outside `min` to `max` is held inside them, and the log says, with this
run's numbers:

`open-annihilation: mutators: limits.units-per-player.default 2000 is outside this run's 20 to 1500; the match plays 1500`

A setting or host value of `default` outside `min` to `max` is clamped, and
the profile warning says `clamped to [min, max]`.

### Related

- [`unit.max-id`](../script-extensions/unit.max-id.md) returns the unit
  limit the game recorded, times 10.
- [`sides`](limits.sides.md) and the identity keys that name sides are a
  different cap: how many sides load, not how many units a player owns.
- [`units.id-reuse-delay`](units.id-reuse-delay.md) decides when a dead
  unit's slot may be used again, inside the slots this limit sizes.

### Settings

`default` reads the registry value `UnitLimit` when the profile binds
`limits.units-per-player.default` under `settings`. The bound value is
clamped between `min` and `max`.

`limits-screen-fallback` reads the INI key `Preferences/UnitLimit` when the
profile binds it. It is the limit 3.1c's unit-limit screen shows when no
limit is stored. The value is kept for the profile; nothing reads it yet.

Open Annihilation's own unit-limit setting offers up to 1,500 units per
player, or `max` when `max` is higher. `min` and `max` are fixed: only the
profile sets them.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Limit id `limits.units-per-player`, written as `units-per-player` under the profile's `limits` block. The `limits.` prefix of the registry id is dropped there.

| Write | Means |
| --- | --- |
| `units-per-player: true` | On, every parameter at its default. |
| `units-per-player: {default: 1500}` | On, the parameters named set and the rest at their defaults. |
| `units-per-player: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `units-per-player: false` | Off, the same as leaving it out: every parameter at its 3.1c baseline. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `default` | `int` | units | `1` to `6553` | - | `250` | `1500` | `match` | `sim` | - |
| `min` | `int` | units | `1` to `6553` | - | `20` | `20` | `fixed` | `sim` | - |
| `max` | `int` | units | `1` to `6553` | - | `500` | `1500` | `fixed` | `sim` | - |
| `limits-screen-fallback` | `int` | units | `1` to `6553` | - | `101` | `1500` | `install` | `view` | - |

Adjustable: `fixed`: only the profile sets it; `install`: the player's settings may set it when the profile binds it under `settings`; `match`: as `install`, and the host may also change it for one game.

- `default` reads the registry value `UnitLimit` when the profile binds it under `settings`.
- A bound or match value of `default` is clamped between `min` and `max`.
- `limits-screen-fallback` reads the settings-file value `Preferences/UnitLimit` when the profile binds it under `settings`.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{default: 250, min: 20, max: 500, limits-screen-fallback: 101}` |

`baseline` sets every parameter to its 3.1c value: the limit is on and plays as 3.1c.

### Shorthand

This limit takes no shorthand: write `true`, `false` or a parameter map.

### Constraints

After resolution the values must keep:

- `min <= default`
- `default <= max`

### Every parameter at its default

```yaml
limits:
  units-per-player:
    default: 1500
    min: 20
    max: 1500
    limits-screen-fallback: 1500
```
<!-- END GENERATED: schema -->
