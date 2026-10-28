# Unit Types

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Limit id | `limits.unit-types` |
| Written as | `unit-types` under `limits` |
| Area | Limits (`limits`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Every machine in the game. |
| Parameters | 2 |
<!-- END GENERATED: facts -->

## Description

How many unit type ids the same-type selection can hold. Selecting every
unit of the types already selected gathers those types in a bitset of
`bitset-bits` ids. A unit whose type id is at or past that width neither
adds its type nor is added. In 3.1c the bitset holds 512 type ids, and a
type past them is left out the same way.

`partial-widening` is kept for the profile. The engine has one type bitset,
and it always takes `bitset-bits`. Nothing reads `partial-widening` yet.

## Configuration example

Written under `limits`. The `limits.` prefix of the registry id is dropped.

```yaml
limits:
  unit-types:
    bitset-bits: 16384
    partial-widening: false
```

`{preset: baseline}` is 3.1c: 512 bits, and `partial-widening` false. There
is no shorthand. A bare number is refused.

## Details

### Network games

The limit is part of the profile's sim hash, so every machine must play the
same values. Raising it does not make the profile OA-only
([12.3](../oamod-standard.md#123-wire-behaviour-a-profile-selects)). The
same-type selection runs on the machine whose player is selecting.

### When data goes past it

A type id at or past `bitset-bits` is left out of the bitset. The engine
writes no log line for that. The unit still exists; it only fails to join
a same-type selection.

### Related

- [`category-mask-types`](limits.category-mask-types.md) is the separate
  width of category masks. A mod that numbers types past 512 usually raises
  both.
- [`ui.selection-shortcuts`](ui.selection-shortcuts.md) adds other selection
  shortcuts. This limit is the bitset the same-type selection itself uses.

### Settings

`bitset-bits` reads the INI key `Preferences/UnitType` when the profile
binds `limits.unit-types.bitset-bits` under `settings`. The raw value is
taken in steps of 512: a value up to 512 gives 512, and a larger value `v`
gives `((v >> 9) + 1) * 512`. `partial-widening` is fixed.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Limit id `limits.unit-types`, written as `unit-types` under the profile's `limits` block. The `limits.` prefix of the registry id is dropped there.

| Write | Means |
| --- | --- |
| `unit-types: true` | On, every parameter at its default. |
| `unit-types: {bitset-bits: 16384}` | On, the parameters named set and the rest at their defaults. |
| `unit-types: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `unit-types: false` | Off, the same as leaving it out: every parameter at its 3.1c baseline. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `bitset-bits` | `int` | bits | `512` to `65536`; a multiple of `512` | - | `512` | `16384` | `install` | `sim` | - |
| `partial-widening` | `bool` | - | `true`, `false` | - | `false` | `false` | `fixed` | `sim` | - |

Adjustable: `fixed`: only the profile sets it; `install`: the player's settings may set it when the profile binds it under `settings`.

- `bitset-bits` reads the settings-file value `Preferences/UnitType` when the profile binds it under `settings`.
- `bitset-bits` turns a bound value into the parameter by the rule `unit-type-bits`.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{bitset-bits: 512, partial-widening: false}` |

`baseline` sets every parameter to its 3.1c value: the limit is on and plays as 3.1c.

### Shorthand

This limit takes no shorthand: write `true`, `false` or a parameter map.

### Every parameter at its default

```yaml
limits:
  unit-types:
    bitset-bits: 16384
    partial-widening: false
```
<!-- END GENERATED: schema -->
