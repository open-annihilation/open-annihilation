# Enhanced Mode

Enhanced Mode is an opt-in set of versioned engine rules. Choose it in
Settings, under Engine Limits, before starting a game. It starts Off.
The choice applies to the next game; a running match keeps its rules.
The selected mod keeps its units, assets and unrelated rules.

Graphics settings are independent. Enhanced Mode does not select a
renderer, change graphical effects or require hardware acceleration.
Players in the same match may choose different graphics settings.

## Current preset

Rules version 1 establishes the mode's identity and validation. It does not
yet raise unit or projectile capacity, replace pathfinding or introduce
ray tracing. Those changes will be added through later versioned presets.
The released unit-capacity ceiling remains 6,553 units per player.

The engine resolves the preset over the selected game's profile before
building the world. Its version enters the simulation hash. With the mode
Off, existing profiles keep their canonical rules and hashes.

## Saved and recorded games

A game saved or recorded with Enhanced Mode keeps its rules version.
Loading it restores that mode without changing the preference for new
games. Older saves and recordings without a mode account play with the
mode Off. Unsupported versions and malformed metadata are refused.

In multiplayer, every player must select the same mode before entering
the battle room. Enhanced games require matching supported versions and
simulation hashes before START. A guest does not automatically adopt the
host's mode in this implementation. Enhanced matches cannot start with
legacy clients.

## Measuring battles

The simulation-only benchmark runs repeatable idle, marching and combat
workloads with a fixed seed and writes a JSON report. For example:

```sh
./run.sh --game-dir /path/to/TotalAnnihilation --enhanced-mode \
  --battle-benchmark /tmp/oa-combat.json --benchmark-units 128 \
  --benchmark-scenario combat --benchmark-warmup 30 --match-ticks 300
```

The population is per side and includes its commander. This harness
measures simulation tick cost, not rendered frame rate. It refuses
unsupported populations and maps without enough clear placement sites.
See [the command-line reference](command-line.md) for all options and
[the benchmark module](../src/app/battle-benchmark/README.md) for report
fields and workload limits.
