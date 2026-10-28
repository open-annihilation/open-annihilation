# Sides

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Limit id | `limits.sides` |
| Written as | `sides` under `limits` |
| Area | Limits (`limits`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Every machine in the game. |
| Parameters | 1 |
<!-- END GENERATED: facts -->

## Description

How many sides the game loads from `gamedata/sidedata.tdf`. It reads
`SIDE0` until a section is missing or `count` sides are in. In 3.1c that
is five sides, `SIDE0` to `SIDE4`. The default when the limit is on is 10,
so `true` loads `SIDE0` to `SIDE9` when those sections exist. A section
past the limit is left unread: its keys and rectangles are not taken.

## Configuration example

Written under `limits`. The `limits.` prefix of the registry id is dropped.
`count` is the shorthand, so a bare number sets it.

```yaml
limits:
  sides: 7   # SIDE0 to SIDE6
```

`true` is 10. `{preset: baseline}` is 5, the same as leaving the limit out.

## Details

### Network games

The limit is part of the profile's sim hash, so every machine must load the
same sides. Raising `count` above 5 makes the profile OA-only, even when
the side data itself has only five sides. It plays only between Open
Annihilation builds that know these limits. The wire behaviour is
[12.3](../oamod-standard.md#123-wire-behaviour-a-profile-selects).

- The host refuses a player on 3.1c. When only the sides are raised the
  line is
  `*** {player} plays Total Annihilation 3.1c, which cannot play a mod with more than five sides.`
  When [`weapon-types`](limits.weapon-types.md) is also above 256, the
  weapon-types line is the one shown. When the room also has mutators, the
  mutator line is the one shown.
- The presence record carries the most sides this build plays, with the
  most weapon types. A record that does not cover the mod refuses that
  player once. **START** stays grey until the limits cover the mod.
- A player whose chosen side is past the sides the room plays keeps
  **START** grey, and the room says once
  `*** {player} chose a side this game does not have.`
  That check is the room's for every profile, not only an OA-only one.
- With only sides raised, the weapon records stay 3.1c's. The side byte
  already holds every side. A recording keeps both limits and plays back
  only under a profile with the same ones, with the same two sentences as
  [weapon types](limits.weapon-types.md). A recording that only raises
  sides keeps 3.1c's records.

The identity keys that name sides are checked once this limit has resolved
([6.1](../oamod-standard.md#61-identity)). More `side-names` than `count`
is refused: `N names, more than the M sides of limits.sides`. A hidden
side or a computer-player side numbered at or past `count` is refused:
`side N is past the M sides of limits.sides`. A hidden-side list that hides
every side is refused: `hides every one of the M sides of limits.sides`.

### When data goes past it

A section past the limit is left out, and one log line names it. One
section reads

`open-annihilation: side data: gamedata/sidedata.tdf: SIDE5 is past the 5 sides this game plays (limits.sides) and is left out`

Several read `SIDE5 to SIDE6 are past the 5 sides this game plays (limits.sides) and are left out`,
with this load's first left-out section, its last, and the limit. A section
the file does not have is not past the limit: reading stops at the gap, and
there is no line. A side missing from a section the limit already left out
does not, by itself, refuse the file.

A player who chose a side the loaded data does not have is moved to a side
the match offers, and the log says, with this match's numbers:

`open-annihilation: side data: player 0 chose side 6, which this match's side data does not have; it plays side 0`

A computer player on a side computer players may not take gets its own
line:

`open-annihilation: side data: computer player 0 chose side 3, which computer players cannot take in this game; it plays side 0`

### Related

- [`weapon-types`](limits.weapon-types.md) is the other OA-only limit.
- [`build-list-entries`](limits.build-list-entries.md) is how many
  `[CANBUILD]` entries each loaded side keeps.
- [`units-per-player`](limits.units-per-player.md) is how many units each
  player may own, not how many sides exist.
- `identity.side-names`, `identity.skirmish-hidden-sides`,
  `identity.battle-room-hidden-sides` and `identity.computer-player-sides`
  all have to name sides below this limit.
  `identity.watcher-side-frame` is not compared with it.

### Settings

`count` is fixed. Only the profile sets it. No INI key and no registry
value adjusts it. The side a player picks in the skirmish or the battle
room is that player's choice among the sides this limit loaded, not a
setting that changes the limit.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Limit id `limits.sides`, written as `sides` under the profile's `limits` block. The `limits.` prefix of the registry id is dropped there.

| Write | Means |
| --- | --- |
| `sides: true` | On, every parameter at its default. |
| `sides: {count: 10}` | On, the parameters named set and the rest at their defaults. |
| `sides: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `sides: 10` | On, the shorthand: a bare value sets `count`. |
| `sides: false` | Off, the same as leaving it out: every parameter at its 3.1c baseline. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `count` | `int` | sides | `5` to `32` | - | `5` | `10` | `fixed` | `sim` | - |

Adjustable: `fixed`: only the profile sets it.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{count: 5}` |

`baseline` sets every parameter to its 3.1c value: the limit is on and plays as 3.1c.

### Shorthand

The limit has one parameter, so a bare value sets `count`: `sides: 10` is `sides: {count: 10}`.

### Every parameter at its default

```yaml
limits:
  sides:
    count: 10
```
<!-- END GENERATED: schema -->
