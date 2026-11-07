# Enhanced Mode

Enhanced Mode is an opt-in set of versioned engine rules. Choose it in
Settings, under Engine Limits, before starting a game. It starts Off.
The choice applies to the next game; a running match keeps its rules.
The selected mod keeps its units, assets and unrelated rules.

Graphics settings are independent. Enhanced Mode does not select a
renderer, change graphical effects or require hardware acceleration.
Players in the same match may choose different graphics settings.

## Current preset

Rules version 2 is the current preset. It retains version 1's capacity of
10,000 units per participating player. It
allocates ranges only for participating players, with at most 32,767 units
across the match. Two players can therefore field 10,000 units each.
Spectators do not consume a unit range. The projectile pool holds 32,767
records, with allocation failures reported by the benchmark.

Scanner and jammer contact queries reuse the spatial projection within a
contact-picture pass. These changes establish capacity and remove repeated
work. Version 2 additionally shares bounded coarse routes between nearby
destinations, ignores moving traffic when building those static routes, and
rechecks every returned edge against current passability and player knowledge.
Narrow corridors outside the coarse graph retain the ordinary path search.

Local steering gives moving traffic temporary detours and yields by waiting
time, with unit ID breaking ties. The original mission and route remain in
place, and the final footprint occupancy test still prevents overlap. A short
mobile blockage can wait before requesting another full path; after 30
consecutive collision-blocked ticks it must replan, even if boundary jitter moves it
slightly. Target searches
use nearby spatial buckets when that reduces work, retaining the legacy
candidate order, target selection and random draws.

These changes do not yet establish smooth frame rates at that population.
Ray tracing remains future work and will be a separate graphics choice.

The engine resolves the preset over the selected game's profile before
building the world. Its version enters the simulation hash. With the mode
Off, existing profiles keep their canonical rules and hashes.

## Saved and recorded games

A game saved or recorded with Enhanced Mode keeps its rules version.
Loading it restores that mode without changing the preference for new
games. Older saves and recordings without a mode account play with the
mode Off. Unsupported versions and malformed metadata are refused.
Enhanced saves retain both the original participants and their physical
unit-range order. Loading a network save therefore preserves ownership and
unit IDs even when player slots were ordered differently between machines.
Version 1 remains supported with its original behavior and simulation hash.
Version 2 saves pending shared fields, the current static-map revision,
temporary traffic heads and actual-progress clocks. These owner-local
navigation tables stay outside shared world and trace hashes.

In multiplayer, every player must select the same mode before entering
the battle room. Enhanced games require matching supported versions and
simulation hashes before START. A guest does not automatically adopt the
host's mode in this implementation. Enhanced matches cannot start with
legacy clients.
The current movement transport still has a small per-tick route-update budget.
The new effective traffic heads are replicated, but simultaneous changes from
thousands of movers require a complete tick-batch transport before large
network battles can be treated as validated.

## Measuring battles

The simulation-only benchmark runs repeatable idle, marching and combat
workloads with a fixed seed and writes a JSON report. `group-march` gives both
armies distinct, footprint-checked arrival positions in shared destination
regions. Its report counts unique units making progress, arrivals and stalls,
including the initially near, middle and far thirds of each army. For example:

```sh
./run.sh --game-dir /path/to/TotalAnnihilation --enhanced-mode \
  --battle-benchmark /tmp/oa-combat.json --benchmark-units 128 \
  --benchmark-scenario combat --benchmark-warmup 30 --match-ticks 300
```

A capacity run on a large installed map can request the full population:

```sh
./run.sh --game-dir /path/to/TotalAnnihilation --enhanced-mode \
  --battle-benchmark /tmp/oa-20000.json --benchmark-units 10000 \
  --benchmark-map "Core Prime Industrial Area" --benchmark-scenario combat \
  --benchmark-warmup 30 --match-ticks 300
```

The population is per side and includes its commander. This harness
measures simulation tick cost, not rendered frame rate. It refuses
unsupported populations and maps without enough clear placement sites.
See [the command-line reference](command-line.md) for all options and
[the benchmark module](../src/app/battle-benchmark/README.md) for report
fields and workload limits.
Use `--benchmark-rules-version 1` or `2` with Enhanced Mode to compare the
presets on the same seed. The report includes simulation phase timings and
cumulative route, local-flow and targeting work; percentiles of individual
phases are not additive.

## First 20,000-unit checkpoint

An Apple M5 Mac with 24 GB of memory completed the combat workload on Core
Prime Industrial Area with 10,000 units per side, seed 7, 30 warmup ticks
and 300 measured ticks. The final population was 19,349, with 2,495 live
projectiles sampled at peak and no projectile allocation failures. Repeated
runs produced the same final state digest, `4f1722b3e338a32d`.

One Check-build run measured 27.3 ms median, 73.7 ms p95 and 206.1 ms p99
simulation ticks. Host load affects wall times. This is a scripted,
simulation-only workload with no AI decisions or rendered frames; it does
not establish sustained 30 Hz simulation or 60 FPS rendering.

## Version 2 checkpoint

Paired Check-build runs on the same M5, map and seed compared 30 warmup
ticks and 300 measured ticks. Two combat runs per version retained the same
digest within that version: v1 `4f1722b3e338a32d`, v2 `f0c69d05ffa5d08f`.
The host had other active workloads, so wall-time ranges describe these runs.

| Workload | Rules | Median tick | p95 tick | p99 tick |
| --- | --- | --- | --- | --- |
| 20,000-unit combat, two runs | v1 | 25.2–28.5 ms | 40.2–40.5 ms | 54.5–71.9 ms |
| 20,000-unit combat, two runs | v2 | 23.4–27.0 ms | 34.4–39.1 ms | 51.2–56.5 ms |
| 20,000-unit group march | v1 | 11.9 ms | 18.7 ms | 31.9 ms |
| 20,000-unit group march | v2 | 9.5 ms | 13.3 ms | 24.0 ms |

Combat sighting-resolution attempts fell from 194,599,924 to 11,806,637;
these counts exclude bucket collection, sorting and membership filtering.
The group march kept all 20,000 units alive. Units advancing at least 16
pixels toward their assigned goals rose from 12,646 to 13,544. No unit
completed its long trip within the measured interval; some remained stalled.
The v2 march used 26 shared-route hits and 27 ordinary search starts.
This stress layout starts touching-width rows at 32-pixel spacing, with only
96 pixels between the opposing fronts and hold fire enabled. It measures
congestion after early contact, rather than sustained motion across open ground.

A separate v2 combat run completed 1,000 measured ticks, ending with 19,281
units and no projectile allocation failures. It measured 15.2 ms median,
40.9 ms p95 and 74.2 ms p99. Combat tails therefore still miss the 33.3 ms
simulation target. These are simulation measurements; rendered frame rate,
AI-heavy workloads and gaming-desktop results remain unvalidated.

## Remaining work

1. Measure simulation phases and AI work separately. Reduce the long ticks
   before making a whole-battle smoothness claim. Compare the same seeded
   workloads on the Mac and a gaming desktop.
2. Validate v2 flow on sustained installed-map battles and extend it where
   measured stalls remain. The synthetic counterflow, crossing and choke
   fixtures check arrival, determinism and zero overlap; those results alone
   do not establish that every installed-map group finishes its trip. Preserve
   Off and v1, and add complete tick batches for large network route bursts.
3. Separate simulation from presentation. The current application ticks and
   renders on the same thread: a 27 ms tick can miss a 16.7 ms frame deadline.
   Keep mutable World ownership on the simulation thread, queue commands for
   deterministic ticks, and publish bounded immutable presentation snapshots.
   Render poses, effects, visibility and HUD data from those snapshots.
4. Scale the existing Full renderer. It already retains geometry and groups
   draws; reduce remaining positioned-vertex uploads by sending rigid-piece
   transforms, then add bounded GPU culling, detail levels and distant-shadow
   budgets. Measure frame preparation, upload bytes and GPU time separately.
5. Add Ultra as an independent Graphics option. The current SDL GPU path has
   no acceleration-structure or ray-dispatch interface. Introduce native
   Metal and Vulkan ray-tracing backends around shared scene/material data.
   Start with hybrid sun shadows and ambient occlusion, then reflections,
   with temporal reconstruction and denoising. Probe support and fall back
   to Full on unsupported hardware. Preserve fog secrecy and simulation
   results with Enhanced Mode both Off and On.

The working targets are 33.3 ms per simulation tick at 30 Hz and 16.7 ms per
rendered frame at 60 FPS. Test their tail latency and sustained behavior
together; meeting either budget alone is insufficient.
