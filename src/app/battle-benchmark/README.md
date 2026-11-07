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

`summarize` copies elapsed-nanosecond samples, sorts the copy and returns the
minimum, mean, maximum and nearest-rank p50, p95 and p99. Empty samples give
zeroes. Only successful measured ticks belong in these samples; warmup ticks
are counted separately.

`report_json` writes schema version 1, explicit completion and error fields,
the scenario and seed, requested and observed populations, successful tick
counts, Enhanced Mode and its version, simulation identity and timing facts.
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
percentiles are not additive.
Its default renderer is `none`: a simulation-only benchmark does not draw.
Strings are UTF-8, control bytes are escaped and numbers use the classic
locale. A non-finite mean is written as `null`.

`app-battle-benchmark` checks exact percentiles without mutating samples,
overflow-safe percentile ranks, map bounds and placement refusals, symmetry,
the population bound, report metadata and JSON escaping.
It also checks grouped arrival geometry with 10,000 mixed-size footprints
per side, skipped terrain candidates, cross-army reservations, malformed
inputs, detours, jitter rejection and trip-distance fairness bins.
