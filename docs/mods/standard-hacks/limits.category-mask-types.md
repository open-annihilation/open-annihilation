# Category Mask Types

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Limit id | `limits.category-mask-types` |
| Written as | `category-mask-types` under `limits` |
| Area | Limits (`limits`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Every machine in the game. |
| Parameters | 1 |
<!-- END GENERATED: facts -->

## Description

How many unit types a category mask can hold. The masks are what
`CATEGORY`, `badTargetCategory` and `noChaseCategory` name. A unit whose
type id is at or past `types` belongs to no category. In 3.1c a mask holds 512 type ids, and a type past them is
left out the same way. The width is a multiple of 512. The default is
512, the same as 3.1c: turning the limit on does not widen the masks.

## Configuration example

Written under `limits`. The `limits.` prefix of the registry id is dropped.
`types` is the shorthand, so a bare number sets it.

```yaml
limits:
  category-mask-types: 16384
```

`true` and `{preset: baseline}` are both 512.

## Details

### Network games

The limit is part of the profile's sim hash, so every machine must play the
same width. Raising it does not make the profile OA-only
([12.3](../oamod-standard.md#123-wire-behaviour-a-profile-selects)). The
masks are built while the unit files load, on every machine, and targeting
reads them on the machine that owns the shooter.

### When data goes past it

A type id at or past `types` is not set in any mask. The unit file still
loads. Weapons that test a category then treat that type as outside it.
The engine writes no log line for a type left out of a mask.

### Related

- [`unit-types`](limits.unit-types.md) is the separate width of the
  same-type selection. A mod that numbers types past 512 usually raises
  both, and they need not be equal.
- [`weapons.retarget-out-of-range`](weapons.retarget-out-of-range.md) and
  the other weapon hacks change how a weapon picks a target. Whether a
  type is a bad target still comes from these masks.

### Settings

`types` is fixed. Only the profile sets it. No INI key and no registry
value adjusts it.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Limit id `limits.category-mask-types`, written as `category-mask-types` under the profile's `limits` block. The `limits.` prefix of the registry id is dropped there.

| Write | Means |
| --- | --- |
| `category-mask-types: true` | On, every parameter at its default. |
| `category-mask-types: {types: 512}` | On, the parameters named set and the rest at their defaults. |
| `category-mask-types: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `category-mask-types: 512` | On, the shorthand: a bare value sets `types`. |
| `category-mask-types: false` | Off, the same as leaving it out: every parameter at its 3.1c baseline. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `types` | `int` | type ids | `512` to `65536`; a multiple of `512` | - | `512` | `512` | `fixed` | `sim` | - |

Adjustable: `fixed`: only the profile sets it.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{types: 512}` |

`baseline` sets every parameter to its 3.1c value: the limit is on and plays as 3.1c.

### Shorthand

The limit has one parameter, so a bare value sets `types`: `category-mask-types: 512` is `category-mask-types: {types: 512}`.

### Every parameter at its default

```yaml
limits:
  category-mask-types:
    types: 512
```
<!-- END GENERATED: schema -->
