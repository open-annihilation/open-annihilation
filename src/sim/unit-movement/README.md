# Unit movement

The portable structs name the `Unit`, `UnitDef` and movement-object fields
they carry. They are not on-disk or multiplayer wire layouts.

Each tick the ground movement driver turns, accelerates and brakes a unit
through `turn`, `accelerate` and `braking_distance`, and the movement object's
update then integrates the position. This component holds those steps and the
entire unattached branch of position integration (`Unit.attach_parent` is 0).

No floating-point replacements are used. The signed 1.13 sine lookup is kept
in `src/base/game-math/include/oa/base/game_math/trig_table.inc` (512
little-endian int16 entries), which the presenter's polygon code also reads. Lookup uses the
game's angle-index bias, quarter-turn offset, rounding bias and arithmetic
shift. The eleven slope speed percentages are the game's table. Intermediate
narrowing and wraparound in acceleration and braking are intentional.

Collision response clamps the attempted X/Z coordinates to the current
footprint cell interior, keeps the attempted height, reduces speed to half
the type maximum when necessary and recomputes velocity with the table trig.
Successful cell changes keep the game's remove/write/insert/dirty/update
ordering. Units simulated elsewhere reuse their movement object's previous
collision bit; the collision query is made only for locally simulated units.

Required Host operations cover collision and spatial occupancy only.
Ownership is an explicit argument. The caller must establish that the unit is
unattached before calling `integrate_unattached`.

Not covered here: path planning/target selection, the ground movement driver,
the flying-unit controller, transport attachment position/orientation, script
movement notifications or the complete movement-object dispatch. A zero
wrapped braking divisor gives no braking distance.

`movement_test.cpp` checks turning wraparound, velocity axes, underwater
penalty, braking, same-cell integration and blocked/accepted transitions.

## Enhanced traffic

`local_flow.hpp` supplies the optional Enhanced v2 traffic steering. Bounded
footprint probes select short avoidance legs around moving occupants; final
collision checks still reject occupied terrain. Waiting priority uses clocks
captured before any unit moves in the tick, with unit IDs breaking ties.
The original navigator keeps its mission and destination.

A retained avoidance leg completes within five world units of its target or
after crossing the target's forward plane. This lets a turning unit resume
its route without circling a short waypoint. A replacement route also clears
the previous avoidance leg. Neither operation resets the consecutive collision
clock that restores normal replanning after 30 blocked ticks.

`unit-local-flow` checks clearance, priorities, coherent turns, bounded retries
and canonical saved heads. `unit-local-flow-steering` runs the actual ground
steering and integration through sustained opposing traffic, checking arrival,
zero overlap, repeated-run determinism and a radius-only completion control.

## TNT terrain and four-point ground pose

`Terrain::height` extracts signed integer high words from 16.16 X/Z, locates
adjacent 16-unit cells, and interpolates four height bytes. Each signed
division truncates toward zero separately, including negative slopes.
Out-of-map queries return -1. TNT attribute heights are the same bytes
`MapPlot.height` holds.

`ground_quad` resolves the first four vertex indices of the root model's
selection primitive, keeping their order and applying the 3DO loader's X/Z
negation. Ground fitting does not add root offsets. Malformed selection
indices/counts give no quad instead of reading past data. A model with no
selection primitive, or a malformed one, leaves the ground pose unchanged.

`fit_ground` covers rotated sampling, early return at map boundaries,
front/back average heights, pitch/roll, and the hovercraft water bob. Its write
to height replaces only the integer high 16 bits of `Unit.position.y`, keeping
the previous fraction. `GroundClock` supplies the four scaled clock readings
of one fitting pass, one per quad corner, rather than assuming that all four
readings are identical. `scaled_bob_tick` computes the wrapping 32-bit product
and division from platform uptime and the game clock's scale, the multiplier
unit-script SLEEP uses too. A water bob with a zero speed divisor fits
nothing and leaves the pose unchanged.

The rotation and slope angles come from
[src/base/game-math](../../base/game-math/README.md) (`rotate_pair` and
`direction`), which computes the game's sines, cosines and arctangents with
integer arithmetic, so they are the same on every platform; this module
converts them to integers to nearest, ties to even. Fixed-point terrain
sampling and bob arithmetic do not use floating point.

```sh
cmake -S src/sim/unit-movement -B local/build-unit-movement -DCMAKE_BUILD_TYPE=Debug
cmake --build local/build-unit-movement --target oa-unit-movement-test oa-unit-terrain-test
ctest --test-dir local/build-unit-movement --output-on-failure
```
