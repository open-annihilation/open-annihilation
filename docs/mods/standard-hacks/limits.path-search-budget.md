# Path Search Budget

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Limit id | `limits.path-search-budget` |
| Written as | `path-search-budget` under `limits` |
| Area | Limits (`limits`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | The machine that owns the unit or player concerned. |
| Parameters | 1 |
<!-- END GENERATED: facts -->

## Description

How many path nodes the path search may visit in one tick, for every
player together, before a local game's pathfinding setting scales it. In
3.1c the budget is 1,333 nodes a tick. The search runs on the machine that
owns the unit.

## Configuration example

Written under `limits`. The `limits.` prefix of the registry id is dropped.
`nodes` is the shorthand, so a bare number sets it.

```yaml
limits:
  path-search-budget: 66650
```

`{preset: baseline}` is 1,333. `true` is the default, 66,650.

## Details

### Network games

The limit is part of the profile's sim hash, so every machine must play the
same budget. Raising it does not make the profile OA-only
([12.3](../oamod-standard.md#123-wire-behaviour-a-profile-selects)).

A shared game or a replay plays this budget as it is. A local game
multiplies it by the player's pathfinding setting: the setting's node
count times this budget, divided by 1,333, and held to at most 10,000,000.
The search still runs on the machine that owns the unit.

### When data goes past it

The tick's budget is divided by the number of players, and each active
player's share is added to that player's credit. The engine writes no log
line when a search spends the credit it has, and none when a map asks for
a longer path.

### Related

- The computer-player hacks under `ai.*` decide what those players order.
  This limit is how far the path search looks while it carries an order
  out, for human and computer players alike.
- [`units-per-player`](limits.units-per-player.md) sizes the unit table.
  It does not change the path budget.

### Settings

`nodes` reads the INI key `Preferences/AISearchMapEntries` when the profile
binds `limits.path-search-budget` under `settings` (the entry has a
shorthand, so the binding may name the entry alone). That value is the
budget above. The player's pathfinding setting then scales it in a local
game, and does not scale it in a shared game or a replay.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Limit id `limits.path-search-budget`, written as `path-search-budget` under the profile's `limits` block. The `limits.` prefix of the registry id is dropped there.

| Write | Means |
| --- | --- |
| `path-search-budget: true` | On, every parameter at its default. |
| `path-search-budget: {nodes: 66650}` | On, the parameters named set and the rest at their defaults. |
| `path-search-budget: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `path-search-budget: 66650` | On, the shorthand: a bare value sets `nodes`. |
| `path-search-budget: false` | Off, the same as leaving it out: every parameter at its 3.1c baseline. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `nodes` | `int` | nodes | `1` to `10000000` | - | `1333` | `66650` | `install` | `sim` | - |

Adjustable: `install`: the player's settings may set it when the profile binds it under `settings`.

- `nodes` reads the settings-file value `Preferences/AISearchMapEntries` when the profile binds it under `settings`.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{nodes: 1333}` |

`baseline` sets every parameter to its 3.1c value: the limit is on and plays as 3.1c.

### Shorthand

The limit has one parameter, so a bare value sets `nodes`: `path-search-budget: 66650` is `path-search-budget: {nodes: 66650}`.

### Every parameter at its default

```yaml
limits:
  path-search-budget:
    nodes: 66650
```
<!-- END GENERATED: schema -->
