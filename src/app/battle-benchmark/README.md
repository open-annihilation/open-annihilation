# Battle benchmark facts

`oa-app-battle-benchmark` (`oa::app::battle_benchmark`) prepares two-army grid
layouts, summarizes simulation timings and writes the benchmark's JSON report.
It reads no game state and uses no window, renderer, clock or filesystem.

`plan_layout` builds equal grids on either side of a central gap, with a fixed
distance between unit centres and a margin inside every map edge. Positions
are whole map pixels, limited to the positive range of 16.16 coordinates.
It rejects a population that cannot fit rather than clipping positions or
placing fewer units. The caller must check terrain, footprints and occupancy;
the layout alone makes no claim that a unit can stand on each point.
`maximum_layout_units` returns the smaller half's grid capacity, bounded by
one million candidate positions per side, so blocked terrain does not
consume the requested army population. The command accepts at most 10,000
units per side; its caller checks the selected mode and match capacity
before spawning.

`plan_group_arrivals` visits opposite-side candidate sites in contiguous
512-pixel regions, so many distinct terminal destinations can share a route
field. It uses a 16-pixel candidate spacing. `select_arrivals` takes each
arriving type's actual footprint, including its commander, and an optional
terrain predicate. It reserves occupied map cells across both armies and
returns all requested destinations or no layout. Call it before spawning
the additional armies, or supply a static-only predicate: destinations may
currently hold opponents that the workload is about to move.

`begin_progress`, `sample_progress` and `summarize_progress` measure assigned
goals outside the timed step. Whole-pixel Manhattan distances avoid floating
point and coordinate overflow. A minimum movement/improvement threshold
prevents one-pixel jitter from counting as meaningful progress. The summary
includes unique moved/progressed/arrived units, live units, stationary samples,
longest stationary runs, goal distances, and near/middle/far initial-distance
thirds. Best distance permits necessary detours without assuming monotonic
progress. Stationary samples alone do not establish an occupancy blockage.

A staged battle (the `stage` scenario) is followed by the helpers below,
outside the timed step; the application reads the match and hands them
plain positions, order targets and occupant words. `parse_crossing_line`
reads a measure line written `NAME=X0,Z0,X1,Z1` in whole map pixels.
`line_side` gives the sign of a point against the line extended without
end, and `crosses_line` is true only for a step whose two ends lie strictly
on opposite sides of the line and that passes within the segment, its ends
included. `begin_staged_unit` and `sample_staged_unit` track one unit the
stage left: its start, its last live position (a dead unit stays where it
was last alive, and a later unit in its slot is not it), its destination
when its first order is a move or patrol, the lines it has crossed away
from its start side (once each), and its live and stationary ticks in the
final `stationary_window_ticks` (300). Stationary means the caller saw no
change of the exact position during the tick. `summarize_staged` totals them
by player, in player order: mean progress toward the destination (start
distance less final distance, straight-line pixels), units that gained
`advance_distance_pixels` (16) or more, the stationary window counts and
crossers per line. `add_target_sample` takes the targets of one player's
attackers at a sample and adds the sample, the attackers, the distinct
targets and the commonest target's share. `audit_ownership` checks that
every footprint cell of the units it is given names the unit on its layer,
counts cells inside two ground footprints, skips a footprint past the map's
last row or column (the game leaves such a unit off the map's cells) and
notes the first tick with an error. `report_json` writes these as the
`staged_battle` object, or null for the other scenarios; the fields extend
schema version 1. `staged_battle` also holds `extra`, an empty object a
later measurement fills with a named number or one number a player.

`summarize` copies elapsed-nanosecond samples, sorts the copy and returns the
minimum, mean, maximum and nearest-rank p50, p95 and p99. Empty samples give
zeroes. Timing dictionaries also hold `total`, the cumulative nanoseconds,
and `over_budget`, the number of samples strictly greater than
`tick_budget_ns` (33,333,333 nanoseconds). A sum above the largest unsigned
64-bit value is held at that value and marked `total_saturated: true`.
Only successful measured ticks belong in these samples; setup and warmup
costs are excluded.

`report_json` writes schema version 1, explicit completion and error fields,
the scenario and seed, requested and observed populations, successful tick
counts, Enhanced Mode, its revision and the alias version, the features the
match played (`enhanced_features`, names in id order), the text of
`--benchmark-features` (`enhanced_features_spec`, empty when that flag was
absent), whether that flag chose the set (a boolean named by joining
`enhanced_features` and `_override`), and `data_faults`, the number of
distinct data-fault lines reported by the end of the run. Simulation identity
and timing facts follow.
Movement, damage, projectile high-water and activity-observed facts state
whether a labelled workload did corresponding work and when it was sampled.
Spatial projection preparations, projected slots, bucket queries and bucket
candidates describe query work. Projectile capacity and failed allocations
expose a saturated shot pool.
Shared-route and local-flow work counters have an explicit cumulative
setup/warmup/measured scope. `full_route_searches` counts traditional route
searches started; target queries, sighting-resolution attempts (including
fallback scans and radar), and full-scan counts show how much target selection
examines. Standable terminal cells do not establish route
connectivity; the progress summaries expose incomplete trips on the installed
map. `goal_metrics_available` distinguishes a movement
assignment from the zero-filled default summaries. `simulation_phase_ns`
holds timing dictionaries for named phases over measured ticks; their
percentiles are not additive. Totals and over-budget counts also apply to
these phase dictionaries.

`tick_ns` retains its wall-time meaning. The additive
`tick_thread_cpu_ns` dictionary measures CPU consumed by the benchmark's
calling thread over each measured step. The application reads
`GetThreadTimes` on Windows or `CLOCK_THREAD_CPUTIME_ID` where supported;
the benchmark module and simulation do not read clocks. Thread CPU excludes
time when that thread is not running and CPU consumed by other threads.
A wall/CPU gap can include preemption, waiting or I/O; these statistics do
not identify its cause or establish movement quality or drawn frame rate.
The two intervals include clock-reading overhead, and clock resolution can
produce zero CPU samples.

`thread_cpu_clock` identifies the clock or says `unsupported`.
`thread_cpu_available` is true when `tick_thread_cpu_ns` holds statistics,
and `thread_cpu_status` says `available`, `unsupported` or `read_failed`.
Unsupported clocks and failed reads leave `tick_thread_cpu_ns: null` rather
than substituting process CPU or wall time. A read failure discards any
partial CPU samples; wall timing remains available and the run can still
complete. These fields extend schema version 1 without changing existing
field meanings.

Its default renderer is `none`: a simulation-only benchmark does not draw.
Strings are UTF-8, control bytes are escaped and numbers use the classic
locale. A non-finite mean is written as `null`.

`app-battle-benchmark` checks exact percentiles without mutating samples,
overflow-safe percentile ranks, cumulative costs and threshold boundaries,
map bounds and placement refusals, symmetry, the population bound, report
metadata, CPU availability and JSON escaping.
It also checks grouped arrival geometry with 10,000 mixed-size footprints
per side, skipped terrain candidates, cross-army reservations, malformed
inputs, detours, jitter rejection and trip-distance fairness bins.
It checks crossing lines (reading and refusals, sides, steps across, past
the end, onto and off a line), staged-unit tracking and summaries
(crossings counted once and only away from the start side, the stationary
window, retirement), target spread samples, ownership audits (a stolen
cell, an overlap, an air unit, a footprint past the edge) and the
`staged_battle` report block.
