# Enhanced Mode

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `setup.enhanced-mode` |
| Area | Game Setup (`setup`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Every machine in the game. |
| Parameters | 1 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

Enables the versioned Enhanced Mode rules for large battles. Version 1
supports compact participating-player unit ranges, up to 10,000 units per
player and 32,767 units across a match, and a 32,767-record projectile pool.
It also reuses spatial projections during contact scans. Renderer selection
is independent. Version 2 adds shared routing and local unit flow while
retaining those capacities. Version 3 selects regional shared-route
construction. Version 4 selects bounded swept ground-step checks. Version 5
reserves a distinct identity for advisory corridor lanes,
requiring its matching simulation behavior and validation
before adoption. Versions 1, 2, 3 and 4 remain available for
saved and recorded games with their original rules and hashes. The unit and
projectile capacities stay the same. Version 10 inherits version 8's movement
and selects AI strike-target distribution among spatial cohorts. Version 11
inherits version 10 and retains compatible current cohort targets within a
bounded weighted-cost margin. Version 13 is a private, unpromoted cardinal
queue escape experiment. It inherits version 10's routing, following,
cohort assignment and rendering policies without version 11's target
retention. Existing mature retries may try cardinally adjacent origin-cell
centres only when the ordinary result makes no cell crossing and a fresh
probe finds moving traffic. Ordinary cell crossings and the unchanged
fallback result are preserved, with the existing clearance checks and no
new saved state. No benefit is established in an actual 20,000-unit
moving-and-fighting comparison. Versions 9, 12 and 14 are unsupported;
later supported versions do not enable version 9.

## Configuration example

Select Enhanced Mode in Settings under Engine Limits. A profile can declare
the same rules explicitly:

```yaml
hacks:
  setup.enhanced-mode:
    version: 1
```

The application still requires Enhanced Mode to be selected before loading
a profile that enables these rules.
The switch selects version 4 for new games. Later experimental presets
require an explicit version choice and their matching behavior. An existing profile's bare
`true` still resolves to version 1 before that match choice is applied.

## Details

The version enters the simulation hash and is recorded in saves and replays.
Every multiplayer participant must use the same supported version. Games
with the mode Off retain the classic allocation and simulation path.

See [Enhanced Mode](../../enhanced-mode.md) for current limitations and
repeatable battle benchmarks.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `setup.enhanced-mode`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `setup.enhanced-mode: true` | On, every parameter at its default. |
| `setup.enhanced-mode: {version: 1}` | On, the parameters named set and the rest at their defaults. |
| `setup.enhanced-mode: 1` | On, the shorthand: a bare value sets `version`. |
| `setup.enhanced-mode: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `version` | `int` | - | `1`, `2`, `3`, `4`, `5`, `6`, `7`, `8`, `10`, `11`; `1` to `11` | - | `1` | `1` | `match` | `sim` | - |

Adjustable: `match`: as `install`, and the host may also change it for one game.

### Presets

This hack has no presets.

### Shorthand

The hack has one parameter, so a bare value sets `version`: `setup.enhanced-mode: 1` is `setup.enhanced-mode: {version: 1}`.

### Every parameter at its default

```yaml
hacks:
  setup.enhanced-mode:
    version: 1
```
<!-- END GENERATED: schema -->
