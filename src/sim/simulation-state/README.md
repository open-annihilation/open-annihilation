# Simulation state

The first slice of the per-tick unit update: player eligibility, the unit sweep
and its timers, health percentage history, both order schedulers, order-queue
changes and height correction. It is not a complete movement or combat engine.

Call `oa::sim::simulation_state::update_units(world, orders, host)` from the game
loop's unit-movement step (`Step::tick_unit_movement`) with the game tick
already incremented. It returns the first `StepFault` a unit's update met, or
none; a fault ends that unit's update, and the other units are still updated.
A type with no maximum health reads 0% health. Units, players, types and game
fields are the canonical `oa::World` records (`src/core`): populate
`Unit.owner`, `Unit.def` and the players' inclusive `first_unit`/`last_unit`
ranges with `oa_ref32` references. Order list heads are native, in the
`OrderQueue` side table indexed by unit slot. Keep all tables stable while
executing callbacks.

Every operation owned by another system is a mandatory `Host` virtual method: no
host that pretends scripts, order handlers, movement, damage, terrain lookup or
weapon work succeeded is supplied. `Host::dispatch_mission` runs the mission
handler of an order's kind; implementing the individual handlers is the next
main integration step.

`Host::order_ends` reports an order the primary scheduler is about to remove
because its handler ended it. `primary_orders` calls it before the removal,
with done true for result 5, and false for result 8, for a 9 that gives way
to the next order, and for the head order of a result that clears the queue.
A null report does nothing, and the report must not change the unit, the
order or the queue. A result that keeps, waits or rotates an order is not
reported, and a 9 that waits because nothing follows it is not reported.

Build independently:

```
cmake -S src/sim/simulation-state -B local/build-simulation-state
cmake --build local/build-simulation-state
ctest --test-dir local/build-simulation-state --output-on-failure
```
