# Build List Entries

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Limit id | `limits.build-list-entries` |
| Written as | `build-list-entries` under `limits` |
| Area | Limits (`limits`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | The machine that owns the unit or player concerned. |
| Parameters | 2 |
<!-- END GENERATED: facts -->

## Description

How many entries a builder keeps from its build list, the `[CANBUILD]`
lists in the side data, which both the build menus and the computer
players read. `copy` is how many entries a list keeps when `overflow` is
`truncate`: the rest are dropped, and the ones kept are the first, in
order. `dynamic` keeps every entry, up to 1,024, whatever `copy` says.

In 3.1c a list keeps 30 entries and drops the rest. The download menus may
add one entry more than the list keeps. They add only while a truncated
list holds fewer than 31 entries, so a list that already holds 31 or more
takes none of theirs. Under `dynamic` they add until the list holds one
more than the 1,024 it keeps.

## Configuration example

Written under `limits`. The `limits.` prefix of the registry id is dropped.

```yaml
limits:
  build-list-entries:
    copy: 36
    overflow: truncate
```

`{preset: baseline}` is 3.1c's 30 and `truncate`. There is no shorthand.

## Details

### Network games

The limit is part of the profile's sim hash, so every machine must load the
same lists. Raising it does not make the profile OA-only
([12.3](../oamod-standard.md#123-wire-behaviour-a-profile-selects)). The
lists are read while the game data loads, on every machine. The computer
players that build from them run on the machine that owns that player.

### When data goes past it

Entries past the number the list keeps are left out, with no log line. A
truncated list of `copy` 36 that names more builders' units keeps the first
36. A list of only 30 under that same `copy` still receives download-menu
units until it holds 31. Under `dynamic` the list keeps the first 1,024
`[CANBUILD]` entries and may take one download entry more.

### Related

- [`sides`](limits.sides.md) decides which side sections are read. The
  build lists live in that side data.
- [`category-mask-types`](limits.category-mask-types.md) decides which
  types a category names. It does not change how long a build list is.
- The `ai.*` hacks decide how computer players spend a list. They do not
  change its length.

### Settings

Both parameters are fixed. Only the profile sets them. No INI key and no
registry value adjusts them.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Limit id `limits.build-list-entries`, written as `build-list-entries` under the profile's `limits` block. The `limits.` prefix of the registry id is dropped there.

| Write | Means |
| --- | --- |
| `build-list-entries: true` | On, every parameter at its default. |
| `build-list-entries: {copy: 36}` | On, the parameters named set and the rest at their defaults. |
| `build-list-entries: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `build-list-entries: false` | Off, the same as leaving it out: every parameter at its 3.1c baseline. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `copy` | `int` | entries | `1` to `1024` | - | `30` | `36` | `fixed` | `sim` | - |
| `overflow` | `enum` | - | `truncate`, `dynamic` | - | `truncate` | `truncate` | `fixed` | `sim` | - |

Adjustable: `fixed`: only the profile sets it.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{copy: 30, overflow: truncate}` |

`baseline` sets every parameter to its 3.1c value: the limit is on and plays as 3.1c.

### Shorthand

This limit takes no shorthand: write `true`, `false` or a parameter map.

### Every parameter at its default

```yaml
limits:
  build-list-entries:
    copy: 36
    overflow: truncate
```
<!-- END GENERATED: schema -->
