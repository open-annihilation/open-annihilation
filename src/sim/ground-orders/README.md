# Ground orders and navigation

This component holds the `Move_Ground` and `Standby` mission handlers of Total
Annihilation 3.1c. Their order kinds, indices into the unit-order name list,
are 26 and 41. The functions return the scheduler's result codes and work with
`simulation_state::Order`. They do not advance phase themselves: that is the
scheduler's job.

`OrderState` stores the target, tolerance and goal fields. Its command-flags
byte is kept apart from the scheduler's `preserve_flags` and `flags`, so each
byte has one owner. The owner must keep each `OrderState` and its goal alive
while the navigator references it, and detach the goal before destroying the
order.

The path is:

1. Move phase zero clears the command-announcement bit and plays sound
   category 5 if required; it constructs a ground goal from the target and
   tolerance+4 (`make_goal`). Attached units reject the request.
2. Goal replacement clears the previous attachment, removes old search work
   through the required Host boundary, signals replacement events, and
   installs the goal (`install_goal`). It sets the movement-event wait bits.
3. `install_goal` installs an initial two-point route from the current
   integer world position to the goal when a primary non-retry order exists.
   This is the game's behaviour, not a newly designed pathfinder. The
   navigator keeps its asynchronous-search-needed bit at the same time.
4. `tick_navigation` tests the goal-cell radius, raises arrival, detaches
   completed goals, consumes nearby route points, and marks a search needed
   after collisions or route exhaustion. An external search result is
   supplied through `accept_path`, which returns true when that result is
   empty and the unit is not already at the goal (it has raised
   `path_failed_event` on the goal's order) and false otherwise.

`search_ready` holds the pending bit and the 60-tick gate, and
`SearchController` owns the four fields of the active search record (unit,
navigator, goal and movement map). Its `cancel` method keeps the game's
identity check: a cancel for another navigator is ignored, while a matching
cancel clears the complete active record. `complete` releases the movement map
and clears only the unit and the movement map; the navigator and goal are
intentionally kept, and controller idleness is therefore determined by the
unit.

`MovementMap` holds the packed two-bit projection the path search reads,
rectangle refresh, the 30-tick occupancy-history projection and unit
release. Classification itself stays at a required host boundary, because it
reads the live terrain and occupancy grids. Values returned by the host are
packed unchanged; this layer does not invent passability rules.

`rebuild` is the whole-map build: the host's single-plot class for every
cell, then a row and a column pass that keep the footprint-wide minimum with
the game's running-minimum rescan and drop a clear window to class 1 when the
cell just outside either end is not clear. As in 3.1c, the match builds one
persistent map per movement class at match start and keeps it current
through rectangle refreshes: feature and building placement or removal, a
moved unit's old footprint, and the live unit list a search job feeds
`prepare_search`, which reads the searching unit's object tick as the
current tick so its own footprint is never a wall. `prepare_search` reads
that list only when the occupancy projection moves, on a class's first
search of a tick (`projection_moves`), so the match gathers it only then. Refresh rectangles carry
the unit's footprint (`Unit.footprint_x`, `footprint_z`), not the footprint
minus one.

5. `steer_ground` drives along three navigator waypoints: 80-unit lookahead
   clipping, fixed-point segment normalization, heading delta, the game's
   turn radius and braking comparisons, then the turn and acceleration
   routines from `unit-movement`. No waypoint is synthesized here.
   In a match that applied mutators (`FastUnitSteering::held_to_route`,
   which the match passes when it holds a mutator digest), a unit faster
   than any stock unit (more than 6 pixels a tick; the fastest stock unit,
   the ARMSS scout ship, makes 5.8) is then held to its route: its speed is
   held to the distance left to the waypoint, to the speed its braking can
   still stop at the third steering point, and, while it is still turning
   toward the target, to the speed at which it finishes the turn within half
   the distance ahead. Such a unit, as stacked speed mutators make one,
   otherwise steps over the cell its move must end in and back, for ever. A
   unit no faster, and every unit of a match without mutators
   (`FastUnitSteering::as_3_1c`), a mod's fast units included, steers
   exactly as in 3.1c, so that network games with 3.1c and recordings play
   as they did.
6. Move phase one returns completion on arrival (sound category 6), or the
   retry result for other movement events. Standby keeps weapon wakeups,
   timers, conditional auto-attack and the randomized delay.

`goals.cpp` holds the three ground goal classes: circle, ring and build-site
outline. Only the listed goal cells are marked for the search: a circle
marks its centre, a ring one cell at the mean range, an outline its border;
the search reaches tolerance through its threshold. `steering_points`
repeats the last point for requested missing trailing points and outputs Y
zero. `Navigation` has the game's 20-point capacity. Invalid counts are
rejected rather than reading outside it.

## Owned spawn adapter

`GroundRuntime` owns concrete movement and navigation state and projects it
to and from a `unit_spawn::Slot`. Its constructor builds the movement object
with the local navigator, or the mirrored navigator for an owner with status
3: zero velocity/previous vector/speed/turn, occupancy tick zero, copied
movement-class handle, occupancy flag1 and navigator changed flag8.
`save_mobility` / `load_mobility` handle the `u%04xmob` block.
`ConstructorStorage` supplies the flag bits and the last-motion tick that
construction leaves as they were; a new object starts them at zero.
`fit_height` adapts the real slot/model to terrain fitting. This adapter is
structurally tested.

## Path search

The asynchronous expander is in `search_worker.cpp`. `SearchWorker` holds
predecessor-relative expansion, pop, wall-follow seeding, the start-to-goal
axis test, job start, and the 100-expansion / fan-4-then-2 slice. Turn
tables are the game's turn penalty table plus the cardinal/diagonal move
costs. Successful slices rebuild the route through
`reconstruct_search_path`; failed slices produce an empty route.

`scan_player_jobs` is the whole job scan: heuristic refresh, per-player
credit, the unit round-robin that asks each navigator `search_ready`, job
start, 100-node slices and route publication. Examining a unit costs 1 and
starting its job 100 more, plus 1 for each step of the job's wall-follow
walk, so a long walk round a wall leaves less of the tick's credit for the
jobs after it. Units are reached through
`SearchUnitAccess` callbacks (the `Unit.movement` side table). The sight
test reads the job player's bit in `Game.sight_grid` (32-unit cells): cells
the player has not seen classify as passable (2). Cell classes come from
`MovementMapSampler`, which match-runtime backs with the spatial-state cell
classifier. The controller's player round-robin index (`round_robin`) is
reused as the sight-test shift; isolated jobs pass `mask_bit` explicitly. The
start node's payload extra is zero; it is never read, because the start cell
is closed on first pop. The credit a tick (`SearchPlayerJobState::tick_credit`)
is 1,333 path nodes in 3.1c; a match starts at its limits' budget
(`oa::data::limits::PathSearch`), up to ten million. A cell keeps its open
node's handle at the heap's full 32 bits, so no open set wraps a handle,
whatever the budget.

World spatial collision remains in `unit-movement::Host`. Ground-order Host
operations are required for search-job cancellation, sounds, weapon events,
target search, issuing an attack, and the shared RNG. No default success or
successful no-op implementations are provided.

The steering direction multiplies `std::atan2` by 3.1c's radians-to-heading
constant, a double, and distances use `std::hypot`. Both are host maths
functions whose last bit may differ between platforms, so identical results
on every host and at every rounding threshold are not yet established. This
component alone does not establish multiplayer compatibility. Goals and
routes use standard C++ allocations; an allocation failure throws.

## Verification

The native test exercises handler→goal→initial route→steering→arrival, plus
standby weapon/timer/attack branches, heap reset, wall-follow seeding, turn
penalties, and expander slices. The test Host only records or supplies
explicit fixture boundaries. Sight bits are an explicit fixture grid, never
synthesized.

```sh
cmake -S src/sim/ground-orders -B local/build-ground-orders -DCMAKE_BUILD_TYPE=Debug
cmake --build local/build-ground-orders --target oa-ground-orders-test oa-search-heap-test oa-search-worker-test oa-movement-map-test
ctest --test-dir local/build-ground-orders --output-on-failure
```
