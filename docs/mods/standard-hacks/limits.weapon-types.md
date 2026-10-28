# Weapon Types

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Limit id | `limits.weapon-types` |
| Written as | `weapon-types` under `limits` |
| Area | Limits (`limits`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Every machine in the game. |
| Parameters | 1 |
<!-- END GENERATED: facts -->

## Description

How many weapon ids a mod may number. A weapon file's `ID=` may be 0 up to
one less than `types`. The game holds as many weapons as the highest id it
loaded needs, and never fewer than 3.1c's 256, so a mod that stays at 256
plays exactly as 3.1c. In 3.1c the ids are 0 to 255. The default when the
limit is on is 4,096.

## Configuration example

Written under `limits`. The `limits.` prefix of the registry id is dropped.
`types` is the shorthand, so a bare number sets it.

```yaml
limits:
  weapon-types: 1024   # weapon IDs 0 to 1,023
```

`true` is 4,096. `{preset: baseline}` is 256, the same as leaving the
limit out.

## Details

### Network games

The limit is part of the profile's sim hash, so every machine must number
the same weapons. Raising `types` above 256 makes the profile OA-only: it
plays only between Open Annihilation builds that know these limits,
whatever its data uses. The wire behaviour is
[12.3](../oamod-standard.md#123-wire-behaviour-a-profile-selects).

- The host refuses a player on 3.1c. The line stays in the room and is not
  sent as chat. When weapon types are raised it is
  `*** {player} plays Total Annihilation 3.1c, which cannot play a mod with more than 256 weapon types.`
  When both this limit and [`sides`](limits.sides.md) are raised, this is
  the line shown. When the room also has mutators, the mutator line is the
  one shown.
- An Open Annihilation player is seated until that player's presence record
  arrives. The record carries the most weapon types and the most sides that
  build plays. A record that does not cover the mod refuses them once, with
  `*** {player} needs a newer Open Annihilation for this mod.`
  A build that sends no presence record is not refused. **START** stays
  grey while a 3.1c player is seated, and while an Open Annihilation player
  has not yet shown limits that cover the mod.
- Under `types` above 256 the weapon fire, interception and feature records
  are sent only in the wide forms, which carry the weapon id in 16 bits:
  `0xe0` is 37 bytes, `0xe1` is 15 and `0xe2` is 8. The feature record keeps
  its own action byte, and the weapon id is 0 for every action other than a
  hit. A 3.1c machine never receives a wide record. With only sides raised,
  the records stay 3.1c's.
- A recording of such a game keeps both limits and plays back only under a
  profile with the same ones. One whose limits differ says
  `This recording needs a mod with {n} weapon types and {m} sides.`
  A recording without them is refused under a profile that raises either:
  `This recording was made with 256 weapon types and five sides. The mod you are playing has more.`
  Wide records are played when the recording's weapon types are above 256.

### When data goes past it

An `ID=` past the limit leaves that weapon out. A missing or negative
`ID=` is left out too, and the engine writes no line for it. When any ID
is past the limit, one log line names how many of those sections were left
out, the limit and the highest such ID:

`open-annihilation: weapons: 1 weapon section has an ID past the limit of 256 weapon types and is left out; the highest ID is 2000`

More than one section reads `weapon sections have IDs` and `are`, with
this load's numbers. A missing or negative `ID=` is not part of that count.

### Related

- [`sides`](limits.sides.md) is the other limit that makes a profile
  OA-only, above 5.
- The `weapons.*` hacks change how a weapon fires. They do not change how
  many ids load.
- `recorder.ta-demo-recorder`'s `weapon-id-patch` stays unsupported, and a
  value of it in the settings file never sets `weapon-types`.

### Settings

`types` is fixed. Only the profile sets it. No INI key and no registry
value adjusts it.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Limit id `limits.weapon-types`, written as `weapon-types` under the profile's `limits` block. The `limits.` prefix of the registry id is dropped there.

| Write | Means |
| --- | --- |
| `weapon-types: true` | On, every parameter at its default. |
| `weapon-types: {types: 4096}` | On, the parameters named set and the rest at their defaults. |
| `weapon-types: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `weapon-types: 4096` | On, the shorthand: a bare value sets `types`. |
| `weapon-types: false` | Off, the same as leaving it out: every parameter at its 3.1c baseline. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `types` | `int` | weapon types | `256` to `65536` | - | `256` | `4096` | `fixed` | `sim` | - |

Adjustable: `fixed`: only the profile sets it.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{types: 256}` |

`baseline` sets every parameter to its 3.1c value: the limit is on and plays as 3.1c.

### Shorthand

The limit has one parameter, so a bare value sets `types`: `weapon-types: 4096` is `weapon-types: {types: 4096}`.

### Every parameter at its default

```yaml
limits:
  weapon-types:
    types: 4096
```
<!-- END GENERATED: schema -->
