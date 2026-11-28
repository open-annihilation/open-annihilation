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

`steering_target.hpp` exposes the controller's existing eighty-pixel target
projection and the distance to the original next point. Native steering and
Enhanced traffic probes share that exact calculation. The original distance
continues to control speed limits, and Off and v1 retain their arithmetic.

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

`shared_routes.hpp` supplies the cache of the Enhanced Mode feature
`routes_fine_regions`. Construction preallocates canonical byte storage for
at most 64 coarse reverse fields by default, within a total of 2,097,152
coarse cells; the match asks for 80 fields within 8,388,608 cells. A coarse
node lies every two map cells. A field joins repeated destinations in one
96-cell region for the same movement class and player. Every coarse edge and
returned route edge is checked at footprint-cell resolution against current
static passability and player knowledge. Narrow corridors outside the
two-cell coarse graph fall back to ordinary search.

Clear direct routes keep each unit's own destination instead of converging on
the region's first destination or its coarse anchor. Direct visibility tests
share the extraction probe budget; a ray that cannot finish within the remaining
budget leaves that budget available for a coarse route prefix. Equal-depth
field expansion tries cardinal neighbours before diagonals, avoiding unnecessary
directional detours. The
shared-route tests check individual lanes, wall-detour symmetry, regions
split by an impassable wall, and a field turned by its player's crowd onto a
free way two levels longer.

Each tick clears at most 65,536 direction bytes, spends at most 2,048 seed
and expansion credits, and allows at most 65,536 cell probes for extraction.
Pending requests leave the full search's retry stamp untouched. The cache
retains its cursors and frontiers in validated little-endian save bytes;
it is owner-local state, excluded from shared simulation digests.

The saved state is format 4. A route prefix holds at most 32 coarse edges,
and when the goal's own branch of the tree is needed the route follows it.
A completed field keeps 60 ticks before a new destination may take its
storage. A blocked connection rejects that route and marks the field for
bounded refresh while preserving other useful branches. No field is rooted
at a goal that connects to no coarse node; a field whose root cannot be
connected when it is built is given up for 300 ticks, then freed. Restoring
another format fails without changes.

With landmass slots (`FlowLimits::landmass_slots`; the match asks for
eight), each movement class and player's coarse nodes are labelled by
landmass: a flood over footprint cells, a step between two cells a
footprint can stand on joining them, with unseen ground passable as the
sampler reports it. A landmass is named by its first footprint cell in scan
order and one below 256 nodes names none. A unit on a named landmass whose
goal is blocked, off the map or on another landmass is routed to the node
of its own landmass nearest the goal, by a two-pass chamfer map (5 a
straight step, 7 a diagonal one) built per landmass; a goal no coarse node
reaches is left as given. The landmass is part of the field key. A request
waits while its labels are first made or its nearest-node map is built;
labels are made again in the background, at least 300 ticks apart, when
the static revision changed or after 900 ticks. Landmass work is bounded
per tick (`landmass_work_per_tick`, 32,768: scanned or flooded cells and
probes, with cleared entries and nearest-map nodes counted more cheaply),
and is saved with the fields. Restore accepts every state the labelling
leaves and nothing a later save would refuse: below a clearing's cursor
every cell and node label is cleared; below a scan's cursor every cell is
looked at, and each node is labelled exactly when its cell is, blocked
with it; a running flood started at the cell before the cursor, put it
first in its queue, holds each cell it reached once, none before its first,
and has labelled its nodes for itself and counted them; a nearest-node map
is building only while the running job builds it. A cache without landmass
slots refuses a field key that names a landmass. A queue that repeated a
cell would let the flood put more cells than the queue holds.

With a crowd weight (`FlowLimits::crowd_weight`; the match asks for 6), a
field's breadth-first search puts off each node it discovers by
`(units * weight + 4) / 9` levels, at most 255, where `units` is the sum of
`FlowSampler::ground_units` over the 3x3 coarse nodes around it: the field's
player's own ground units, counted once a tick. A put-off node goes back to
the tail of the queue with one level less to wait, at the cost of one
expansion credit, instead of being expanded, so a crowded way is reached
later than a free one of about the same length and the field's routes take
the free one; the root waits for no crowd. The queue is a ring of one entry
a node whose head and tail count every entry taken and put; an entry holds
its node in the low 24 bits and the levels it still waits in the high 8, so
the delays need no storage of their own and a map of 2^24 nodes or more
builds by distance alone. A field whose build met a delay is marked; with
crowd rebuild ticks (`crowd_rebuild_ticks`; the match asks for 240), at each
30-tick refresh with no dirty field due, the marked completed field that
completed longest ago, at least that many ticks before, and that a request
asked for since it completed, is built again from the crowds of that moment.
A field nobody asks for keeps its tree, so the fields of destinations no
longer in use do not keep the building busy. The crowd section after the
landmass section holds the weight, the rebuild ticks and each field's mark,
8 bytes and 4 a field; without a weight there is no section and the layout
is as before.
Restore accepts every ring a build leaves, each discovered node in it at
most once, and refuses a node waiting in the queue that has a child, a
discovered node that does not lead to the root, more entries put than 256
for each discovered node, or a mark other than 0 or 1.

A route can also remove one unnecessary bend after the anchor shortcut when
the full chord is known and clear. It traces the actual fixed-point
footprint pose, including both sides of exact corner crossings. This optional
ray uses at most 128 probes per request within the existing per-tick extraction
budget.

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
