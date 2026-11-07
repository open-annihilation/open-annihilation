# Enhanced Mode

Enhanced Mode is an opt-in set of versioned engine rules. Choose it in
Settings, under Engine Limits, before starting a game. It starts Off.
The choice applies to the next game; a running match keeps its rules.
The selected mod keeps its units, assets and unrelated rules.

Graphics settings are independent. Enhanced Mode does not select a
renderer, change graphical effects or require hardware acceleration.
Players in the same match may choose different graphics settings.

## Current preset

Rules version 5 is prepared as the current preset only when its matching
corridor flow behavior is integrated and validated. It retains the earlier
presets' capacity of 10,000 units per participating player. It
allocates ranges only for participating players, with at most 32,767 units
across the match. Two players can therefore field 10,000 units each.
Spectators do not consume a unit range. The projectile pool holds 32,767
records, with allocation failures reported by the benchmark.

Scanner and jammer contact queries reuse the spatial projection within a
contact-picture pass. These changes establish capacity and remove repeated
work. Version 2 introduced bounded coarse routes shared between nearby
destinations, ignores moving traffic when building those static routes, and
rechecks every returned edge against current passability and player knowledge.
Narrow corridors outside the coarse graph retain the ordinary path search.
Version 3 groups more destinations into each shared field, returns shorter
route prefixes and retains useful fields when an extracted edge becomes
blocked. Its construction work stays bounded. Earlier presets keep their
original routing policy.

Version 4 checks the unit's footprint along each locally simulated ground
step, including intermediate cells and both sides of an exact diagonal
crossing. A free destination no longer permits a step through an occupied
intermediate cell. Work is bounded to 128 origins and 512 footprint cells;
an unsupported step receives the ordinary blocked response. This corrects
collision handling, but does not establish smooth movement through a crowded
battlefield. Off and versions 1, 2 and 3 keep their original movement rules.

Version 5 reserves a distinct rules identity for advisory corridor lanes.
The version metadata prepares save, replay
and multiplayer agreement; it does not by itself implement those behaviors
or establish improved bridge flow. Version 4 keeps its original ground
movement and collision checks. Unit and projectile capacities and renderer
selection remain unchanged.

Version 3 can also remove an unnecessary bend from a published guide when
the unit's actual footprint has a clear, fully known line to the following
point. The optional check has its own probe limit within the existing shared
budget. It traces the actual fixed-point position, including both sides of
diagonal grid crossings.

For an installed route with sustained blockage, version 3 periodically checks
the following corner without waiting for another search result. Each unit has
a 120-tick phase, with at most 128 origin queries and 512 footprint cells per
attempt. Successful advancement preserves the original goal, pending search,
retry clock and a compatible temporary passing head. This corrects blocked
waypoint cases; sustained 20,000-unit congestion remains unresolved.

Local steering gives moving units temporary detours around mobile ground
traffic, including idle units, and yields by waiting time measured at the
start of the tick, with unit ID breaking ties. The original mission and route
remain in place, and the final footprint occupancy test still prevents
overlap. A short mobile blockage can wait before requesting another full
path. After 30 consecutive collision-blocked ticks, ordinary replanning
resumes even if boundary jitter
moves the unit slightly. A local passing attempt can also retry once every
30 ticks, staggered by unit ID, without restarting that grace period.
Target searches use nearby spatial buckets and a sightings membership index
when that reduces work, retaining the legacy candidate order, target
selection and random draws.

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
Versions 1 and 2 remain supported with their original behavior and simulation hashes.
Versions 2, 3 and 4 save five owner-local navigation tables: shared fields, the
current static-map revision, temporary traffic heads, actual-progress clocks
and the shared-route scan clock. The scan clock prevents a load pass and a
regular pass at the same tick from polling the unit ranges twice. These
tables stay outside shared world and trace hashes.

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
Use `--benchmark-rules-version 1`, `2`, `3`, `4` or `5` with Enhanced Mode to compare the
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

These timing runs precede the route-quality corrections described below.

Paired Check-build runs on the same M5, Core Prime Industrial Area and seed 7
compared 30 warmup ticks and 300 measured ticks. Two combat runs per version
retained identical results within that version: v1 `4f1722b3e338a32d`,
v2 `943bf1ddd2329180`. Other host workloads varied, so these wall-time ranges
do not establish a uniform speedup.

| Workload | Rules | Median tick | p95 tick | p99 tick |
| --- | --- | --- | --- | --- |
| 20,000-unit combat, two runs | v1 | 12.9–18.0 ms | 21.9–27.3 ms | 27.8–30.2 ms |
| 20,000-unit combat, two runs | v2 | 10.8–12.1 ms | 15.3–25.2 ms | 18.1–36.6 ms |
| Dense 20,000-unit group march | v1 | 16.9 ms | 26.4 ms | 34.5 ms |
| Dense 20,000-unit group march | v2 | 19.7 ms | 24.9 ms | 36.5 ms |
| Active 20,000-unit march, two runs | v1 | 6.2–8.8 ms | 11.3–17.9 ms | 13.1–25.2 ms |
| Active 20,000-unit march, two runs | v2 | 8.8–11.2 ms | 13.8–18.2 ms | 16.1–20.8 ms |

Combat sighting-resolution attempts fell from 194,599,924 to 7,044,792;
these counts exclude bucket collection, sorting and membership filtering.
The default dense march starts touching-width rows at 32-pixel spacing,
with only 96 pixels between the opposing fronts and hold fire enabled.
It measures congestion after early contact, rather than sustained open-ground
motion. All 20,000 units survived; units advancing at least 16 pixels toward
their goals rose from 12,646 to 13,545. Total goal-distance improvement was
478,187 pixels for v1 and 485,923 for v2. Many remained stalled.

The active march uses the video's separate capture fixture: 48-pixel spacing
and banks offset 1,024 pixels away from the centre, with the same engine
physics and pose recording disabled for timing. This configuration is not
the default CLI layout. Both versions had 19,840 units advance at least 16
pixels. V2 performed 499 shared-route hits and 548 ordinary search starts;
v1 started 217 ordinary searches. V2's extra steering and replanning have
costs; these results do not establish that every formation moves faster.

A separate v2 combat run completed 1,000 measured ticks, ending with 19,292
units and no projectile allocation failures. It measured 11.4 ms median,
15.4 ms p95 and 19.6 ms p99, with a 61.2 ms maximum. Most simulation ticks
fit the 33.3 ms target in these runs, but occasional long ticks remain.
Rendered frame rate, AI-heavy workloads and gaming-desktop results remain
unvalidated.

## Route-quality checkpoint

Enhanced v2 now completes a temporary avoidance leg after the unit passes its
target, rather than requiring a turn back into a five-pixel arrival circle.
A replacement path clears the old avoidance leg even when its destination
has not changed. Shared routes preserve a clear lane to each unit's own
destination, favour cardinal expansion at equal depth, and retain bounded
route-prefix progress when a direct visibility test would exceed the tick's
remaining probe budget. Off and v1 keep their previous movement behavior.

The sustained eight-unit steering fixture, with one-cell footprints, completes
in 466 ticks instead of 549. Its closed-orbit windows fall from 34 to zero,
with no footprint overlaps. This fixture exercises the actual steering and
integration functions; it does not establish stock two-cell traffic capacity.

A separate 900-tick comparison repeats the original 20,000-unit march on Core
Prime Industrial Area with byte-identical initial positions, seed 7, 48-pixel
spacing and 1,024-pixel bank offsets. Units entering a tight orbit fall from
10,792 to 7,142, a 33.8% reduction. Here an orbit means travelling more than
24 pixels in 60 ticks while ending within five pixels of the window's start.
Units travelling fewer than 16 pixels in the final 150 ticks fall from 11,283
to 9,558. Median inward displacement increases from 552 to 630.5 pixels on
the left and from 406.5 to 614.5 on the right. All 20,000 units survive this
hold-fire comparison. Congestion and repeated local detours remain.

An exact static-footprint audit finds 1,419 of that original march's mirrored
goals blocked or on disconnected ground. The separate combat recording uses
connected, mirrored troop placements and real attack orders, with stock
footprints and movement. Its HUD counts the original roster's live targets
with positive health and removes a unit on its actual lethal-hit tick.
The dots are drawn from recorded engine positions; the recording does not
measure the game's rendered frame rate.

A subsequent 300-tick combat benchmark preserves the v1 digest
`4f1722b3e338a32d` and the v2 digest `943bf1ddd2329180`. One v2 run measures
14.6 ms median, 32.3 ms p95 and 71.7 ms p99, with a 561.6 ms maximum wall
time. Host load varies, and long ticks remain; these results do not establish
smooth whole-battle performance.

## Congestion and CPU checkpoint

Shared-route polling now visits at most 1,024 unit slots per tick across the
active players independently of the ordinary search cursor and its credit.
It skips a unit whose full search is already active, leaving that search's
route and completion authoritative. Sustained blockage can still receive a
shared terrain detour when the wall lies beyond the local traffic probe.
Pending fields, unchanged guides and direct goal legs leave ordinary dynamic
replanning available after 30 blocked ticks. A successful delivery observes
the ordinary 60-tick retry interval; pending fields do not consume it.

Static-map changes, including wrecks, retain useful fields and mark them
dirty rather than discarding every frontier. Returned connections are
checked against the current map. Polling rotates the first active player
each tick so one army cannot always consume the shared extraction budget.
Completed dirty fields refresh one at a
time, at 30-tick intervals, at least 60 ticks after completion. A route can
skip its first coarse anchor when a bounded footprint check clears the
connection to the next corner. Local passing also rejects an immediate
sideways return that would undo a leg completed on that or the preceding
tick, while allowing forward rejoining and a mature recovery to reverse.
Completed inactive heads retain their completion tick; new routes clear
that history. A shorter copy of an existing guide also leaves a mature
passing head intact. The saved head remains 27 bytes. Traffic probes follow
the movement controller's projected aim; passing directions retain the
original route intent and bounded lookahead.

A unit stationary for at least 30 ticks may voluntarily yield its traffic
priority during its staggered mature retry. Eligibility uses the same
tick-start motion snapshot as ordinary priority comparisons. The recovery
keeps the existing passing candidates, footprint checks and native movement.

Physical footprint results share a fixed 640 KiB scratch memo within one
planner invocation. Each lookup still checks the requesting player's current
fog first. A new invocation, including another load pass at the same tick,
starts empty. This scratch storage is neither saved nor hashed.

The earlier connected 20,000-unit recordings still show very little passage through
the centre bridges. In the 1,800-tick comparison, recorded bridge crossings
involve four distinct units in combat and eleven in the hold-fire march.
The count requires a completed side change inside the bridge lane; touching
the bridge or crossing elsewhere on the map does not count. Sampling can
hide additional crossings between recorded ticks. These results do not
establish repaired bridge flow, despite the smaller steering fixtures'
progress.

A subsequent recovery-only comparison repeats 1,800 ticks with the same
20,000 starting units, goals and production routing limits. Mean Euclidean
goal-distance improvement rises from 1,008.65 to 1,028.38 pixels. The original
rear cohorts, 1,378 ARM and 1,219 CORE units, improve from 269.39 to 283.03
pixels on ARM and from 230.36 to 242.34 on CORE. All 601 recorded samples
have zero stock-footprint overlaps.
The rear still makes little goalward progress during the final ten seconds:
6.56 pixels on ARM and 8.41 on CORE, with medians near zero. Only five distinct
units cross the centre bridges, including repeated returns. This improves
stationary recovery but does not resolve sustained congestion or bridge flow.

Separate frozen-workload comparisons measure target-query and death-cleanup
changes apart from these route changes. Each uses 20,000 starting units,
Core Prime Industrial Area, seed 7 and 1,800 ticks without warmup. Both sides
of each pair retain digest `d13d0852756db488` and 18,902 final units.
Filtering nearby target slots by the sightings membership index before
sorting changes median tick wall time from 11.219 to 10.339 ms and p95 from
14.906 to 14.521 ms. P99 worsens from
19.120 to 28.634 ms in that pair, so this is not a uniform latency improvement.

Ground-only v2 matches also skip air-target cleanup until a flying driver or
air goal has existed. Thereafter every death keeps the cleanup, including
mirrored, arrived and restored goals. In its separate paired run, total
thread CPU time at the two large death bursts falls from 45.181 to 26.368 ms
at tick 147 and from 40.370 to 23.455 ms at tick 228. These are whole-tick CPU
measurements, not rendered frame times or a whole-battle tail-latency claim.

V2 now indexes owned orders and observer links in preallocated storage.
Removed orders are destroyed immediately; empty ownership slots compact in
stable creation order on a later operation tick. Full-capacity replacement
retains a linear storage-reuse fallback. Certified observer links retain
checks against their actual predecessor and target; unindexed records use
the original checks. Off and v1 keep their original order handling.

Separate native 20,000-unit fixtures on the same M5 compare common-target
command and death bursts. Each pair retains its world digest and every
non-timing activity field:

| Operation | Before thread CPU | After thread CPU |
| --- | --- | --- |
| Death releases 9,674 attack orders | 383.183 ms | 20.635 ms |
| Stop 10,000 units targeting a living enemy | 112.720 ms | 14.104 ms |
| Retarget 10,000 units through the ordinary attack command | 132.131 ms | 23.775 ms |

Death and Stop run for 600 ticks; Retarget runs for 90. These isolate order
cleanup rather than whole-battle performance. External scheduling still
causes occasional longer wall times: one 52.460 ms Stop-fixture tick used
only 11.949 ms of thread CPU. The larger installed-map congestion remains
unresolved in the hold-fire crossing workload.

## Version 3 installed-map checkpoint

Version 3 repeats the connected, native 20,000-unit capture on Core
Prime Industrial Area for 1,800 ticks. Troops retain stock two-cell footprints,
48-pixel initial spacing and 1,024-pixel bank offsets. Combat uses actual
attack orders, with 9,567 ARM and 9,499 CORE units surviving. All 934 recorded
deaths match the native per-tick census and lethal-hit records; the first is
at tick 145. The final world digest is `2c965e3f554a1a34`.

On the same M5, this Check-build capture records simulation thread CPU time:

| Mean tick | Median tick | p95 tick | p99 tick | Maximum tick |
| --- | --- | --- | --- | --- |
| 11.20 ms | 11.44 ms | 14.05 ms | 15.54 ms | 29.10 ms |

None of these 1,800 ticks exceeds the 33.3 ms CPU budget. Other host work
can still extend wall time. This is one scripted battle with no strategic
AI or rendered frames, and does not establish rendered frame rate, desktop
performance or a causal speedup over captures made under different host load.
Pose and census recording occurs outside the timed simulation step.

The matching hold-fire march still shows poor flow. Only 12 distinct units
complete a side change inside the centre bridge lanes. The original rear
cohorts improve their goal distance by means of just 15.74 ARM and 17.93 CORE
pixels during the final ten seconds, with medians of 2.52 and 2.64 pixels.
Every recorded footprint is physically legal and no sample has overlapping
units, but shared-route coverage and low CPU cost have not resolved the
waypoint pile-ups.

## Version 4 ground-step checkpoint

The swept ground guard passes both independent footprint-intersection checks
and Match movement tests. In a native sixteen-unit corridor comparison,
arrivals rise from eight to nine, but the remaining total goal distance rises
from 3,845 to 4,287 pixels. This is a collision-correctness improvement with
mixed movement results. The separate corridor-coordination prototype is not
enabled: it reduced arrivals to four in the same comparison.

Two Check-build combat runs on the M5 use Core Prime Industrial Area, seed 7,
10,000 units per side, 30 warmup ticks and 300 measured ticks. Every non-timing
report field repeats exactly, with world digest `54bc88091f949178`, active
combat and zero projectile allocation failures. Median simulation thread CPU
time is 10.23–10.48 ms, p99 is 13.93–14.20 ms and the largest tick is
15.57 ms. All measured wall and thread CPU times stay below 33.3 ms. Earlier
versions reproduce their pinned world digests in this build.

These short scripted runs contain no strategic AI or rendered frames. Other
host workloads were uncontrolled. They do not prove a speedup, sustained
whole-game frame rate or improved bridge throughput at 20,000 units.

## Remaining work

1. Measure simulation phases and AI work separately. Reduce the long ticks
   before making a whole-battle smoothness claim. Compare the same seeded
   workloads on the Mac and a gaming desktop.
2. Validate the current preset on sustained installed-map battles and extend it where
   measured stalls remain. The synthetic counterflow, crossing and choke
   fixtures check arrival, determinism and zero overlap; those results alone
   do not establish that every installed-map group finishes its trip. Preserve
   Off and earlier presets, and add complete tick batches for large network
   route bursts.
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
