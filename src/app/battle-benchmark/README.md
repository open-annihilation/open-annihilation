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
the supported per-side population, so the caller can test candidate terrain
points before choosing its army's final positions.

`summarize` copies elapsed-nanosecond samples, sorts the copy and returns the
minimum, mean, maximum and nearest-rank p50, p95 and p99. Empty samples give
zeroes. Only successful measured ticks belong in these samples; warmup ticks
are counted separately.

`report_json` writes schema version 1, explicit completion and error fields,
the scenario and seed, requested and observed populations, successful tick
counts, Enhanced Mode and its version, simulation identity and timing facts.
Movement, damage, projectile high-water and activity-observed facts state
whether a labelled workload did corresponding work and when it was sampled.
Its default renderer is `none`: a simulation-only benchmark does not draw.
Strings are UTF-8, control bytes are escaped and numbers use the classic
locale. A non-finite mean is written as `null`.

`app-battle-benchmark` checks exact percentiles without mutating samples,
overflow-safe percentile ranks, map bounds and placement refusals, symmetry,
the population bound, report metadata and JSON escaping.
