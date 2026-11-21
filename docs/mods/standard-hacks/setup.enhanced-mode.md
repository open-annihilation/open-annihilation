# Enhanced Mode

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `setup.enhanced-mode` |
| Area | Game Setup (`setup`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Every machine in the game. |
| Parameters | 2 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

Records the Enhanced Mode revision and the features a match plays. Revision 1
is the set new games play when Enhanced Mode is on. A match can name a
revision and a set of features. Graphics settings remain independent. Saved
games and recordings keep the revision and features they were played with.

## Configuration example

Select Enhanced Mode in Settings under Engine Limits. A profile can declare
the same rules explicitly:

```yaml
hacks:
  setup.enhanced-mode:
    revision: 1
```

The application still requires Enhanced Mode to be selected before loading
a profile that enables these rules. With Enhanced Mode on and no feature
list, a new game plays Revision 1.

## Details

The revision and features enter the simulation hash and are recorded in
saves and replays. Every multiplayer participant must use the same revision
and features. Games with the mode Off retain the classic allocation and
simulation path.

See [Enhanced Mode](../../enhanced-mode.md) for current limitations and
repeatable battle benchmarks.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `setup.enhanced-mode`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `setup.enhanced-mode: true` | On, every parameter at its default. |
| `setup.enhanced-mode: {revision: 1}` | On, the parameters named set and the rest at their defaults. |
| `setup.enhanced-mode: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `revision` | `int` | - | `1` to `1` | - | `1` | `1` | `match` | `sim` | - |
| `features` | `set<enum>` | - | `capacity`, `local-flow`, `routes-original`, `routes-large-regions`, `routes-fine-regions`, `indexed-searches`, `route-corner-advance`, `swept-ground-steps`, `corridor-lanes`, `patrol-goal-retention`, `ai-strike-retarget`, `squad-position-index`, `route-queue`, `group-destination-repair`, `distributed-arrivals`, `moving-corner-advance`, `local-throat-guide`, `credited-fallback-search`, `bounded-strike-recruits`, `ai-attack-retention`, `ai-rally-retention`, `local-following`, `occupancy-free-planning`, `cohort-targets`, `steering-projection-reuse`, `cohort-retention`, `cardinal-queue-escape`, `exact-indexed-searches`, `wide-strike-recruits`; each at most once | any | `[]` | `[capacity]` | `match` | `sim` | - |

Adjustable: `match`: as `install`, and the host may also change it for one game.

### Presets

This hack has no presets.

### Shorthand

This hack takes no shorthand: write `true`, `false` or a parameter map.

### Every parameter at its default

```yaml
hacks:
  setup.enhanced-mode:
    revision: 1
    features: [capacity]
```
<!-- END GENERATED: schema -->
