# Repair Rate

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `repair.rate` |
| Area | Repair (`repair`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | The machine that owns the unit or player concerned. |
| Parameters | 1 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

Builders repair damaged units at a speed that grows with the builder's
`WorkerTime` and the target's `MaxDamage` and `BuildTime`, instead of 3.1c's
single hit point per repair step. In the default mode the fraction of a hit
point left over by each step is kept, so slow repairs lose nothing to rounding.

## Configuration example

Switch it on with the exact mode:

```yaml
hacks:
  repair.rate: true
```

Scale the repair with worker time, but round each step's heal and energy up
to at least 1 instead of carrying the remainder:

```yaml
hacks:
  repair.rate: clamp-min-1   # shorthand for {mode: clamp-min-1}
```

## Details

### Behaviour

A repair step runs on each tick a builder repairs a damaged unit. Its work is
`w = WorkerTime / 30`. A target at full health, or one without a type, takes
no step in any mode.

In every mode the builder pays, as in 3.1c: the energy is asked of the
builder's own economy, and the builder's owner pays it whoever owns the
target. A repair pad is the builder for the aircraft it repairs, so the pad
pays, and the bottom bar shows the cost on the pad.

The first two modes start from the same two amounts, each computed at 53-bit
precision and truncated:

- health: `(MaxDamage × w − 1) / BuildTime + 1`
- energy: `(BuildCostEnergy × w − 1) / BuildTime + 1`

`mode` then chooses what is paid and healed:

- `clamp-max-1` (3.1c): each amount is at most 1. A step pays at most 1
  energy and heals at most 1 hit point, whatever the builder's worker time.
- `clamp-min-1`: each amount is at least 1, so a fast builder heals and pays
  in proportion. Under both clamp modes a target with a `BuildTime` of 0
  leaves both amounts at 0.
- `exact-remainder`:
  - The energy is the amount above, at least 1. When the economy refuses it,
    nothing is healed and the step has not happened.
  - The heal is `trunc(w) × MaxDamage / BuildTime`, with integer division.
    The division's remainder is added to what the builder carries for this
    target; each time the carried amount reaches `BuildTime`, `BuildTime` is
    taken off it and the step heals 1 more.
  - A builder carries remainders for its two latest targets. A third target
    takes the first entry's place and starts with nothing carried. An entry
    stays with its unit slot when the unit dies.
  - A builder that carries no remainders (unit index 0, or outside the table)
    heals 1 more whenever the division leaves a remainder, and at least 1.
  - A builder with `WorkerTime` below 30 (`trunc(w)` is 0) pays the energy
    and heals nothing. A step that pays and heals nothing has still happened.
  - A target whose `BuildTime` is 0 or negative is not repaired.
  - One step heals at most 65535 hit points.

The heal is applied as a healing damage event (kind 10). When another machine
simulates the target, the heal is sent to that machine as in 3.1c.

Self-repair from `HealTime` runs through the same step, so this hack also
decides how much a regenerating unit heals: see
[repair.healtime-self-heal](repair.healtime-self-heal.md).

### Baseline

3.1c repairs 1 hit point for at most 1 energy per step: `clamp-max-1`.

### Network games

The machine that simulates the repairing unit runs the step. `mode` is part
of the profile hash, so every machine plays the same rule. Under
`exact-remainder` the carried remainders are game state: they are folded into
the match-state digest that machines compare, and a saved game keeps them.

### Implementation notes

- The step is `recover_health` in `src/sim/unit-health/src/unit_health.cpp`,
  with `recover_health_exactly` for `exact-remainder`. It reads
  `rules().repair.rate.mode` (the `RepairRate` record in
  `src/data/match-rules/include/oa/data/match_rules/records.inc`).
- The remainders live in `Match::repair_remainders`, two entries per unit
  slot, allocated by `Match::keep_repair_remainders` in
  `src/sim/match-runtime/src/tick_host_health.cpp` only under
  `exact-remainder`. They are the rule-state table `repair-remainders`.
- Tests: `unit-health-repair-rate`
  (`src/sim/unit-health/tests/repair_rate_test.cpp`) covers each mode, the
  edges, a builder without a table and the two-target limit;
  `match-veterancy-repair` repairs in a match and checks the remainders as
  rule state; `data-match-rules` and `data-mod-profile` check the record and
  the profile parsing.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `repair.rate`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `repair.rate: true` | On, every parameter at its default. |
| `repair.rate: {mode: exact-remainder}` | On, the parameters named set and the rest at their defaults. |
| `repair.rate: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `repair.rate: exact-remainder` | On, the shorthand: a bare value sets `mode`. |
| `repair.rate: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `mode` | `enum` | - | `clamp-max-1`, `clamp-min-1`, `exact-remainder` | - | `clamp-max-1` | `exact-remainder` | `fixed` | `sim` | - |

Adjustable: `fixed`: only the profile sets it.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{mode: clamp-max-1}` |

`baseline` sets every parameter to its 3.1c value: the hack is on and plays as 3.1c.

### Shorthand

The hack has one parameter, so a bare value sets `mode`: `repair.rate: exact-remainder` is `repair.rate: {mode: exact-remainder}`.

### Every parameter at its default

```yaml
hacks:
  repair.rate:
    mode: exact-remainder
```
<!-- END GENERATED: schema -->
