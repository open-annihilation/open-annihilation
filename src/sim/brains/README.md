# Brains

A computer seat played by a sandboxed Lua brain. The app reads the package
and hands its bytes here. The seat copies them, checks every network, and
calls `fits` and `start` before the match's first tick. A seat with no
brain plays as the built-in computer player.

## Entry points

- `seat_brain` loads one brain for one computer seat. On success the seat's
  hooks are installed and its play skips the built-in order tick. On any
  failure the sandbox is closed and no hooks are installed.
- `brain_seated` tells whether that seat's hooks are a seated brain's.
- `seat_sandbox` returns the sandbox, for tests and tools.
- The brain profile is named `brain`. `install_libraries` is the one place
  a later change adds a library: one call, after the ones already there.
  It opens `f32` and `geometry`, adds `geometry.sine_scaled` and
  `geometry.cosine_scaled`, and creates the global `brain`, `game` and `me`
  tables. It has to work when the seat has no match.

## State

The seats live in the match's `seat_state`, one slot per player, and end
with the match. A seat keeps the package's bytes in memory. This module
reads no files.

## Invariants

No heap container, no `throw` and no `virtual` outside `tests/`. No Lua
runs before every network's digest has been checked. `fits` and `start`
run under the phase allowance, the same allowance a mutator's phase gets.
Only a computer seat of this machine is seated. No brain seated changes
nothing about the built-in computer player.

Name the hooks table something other than `brain`. A file that starts
`local brain = {}` shadows the global `brain` table, so a hook in that
file that calls `brain.random` finds the hooks table instead. Use
`local hooks = {}`.

## Tests

`sim-brains-seat` (`tests/seat_test.cpp`) seats the made-up brain
`steady-commander` on the computer-build fixture: `me`, `game`, the
libraries, `require`, a reason from `fits`, load failures, a start that
runs past its allowance, seats that are refused, a seated brain that builds
nothing, and two brains destroyed with the match.

`match-seat-hooks` covers the controller switch with no brain loaded.

## Limitations

No events and no `onTick` yet (B02): the seat's play returns without calling
the script. The rest of `me` and `game`, orders, chat and networks in Lua are
not here. A failed seat is not handed back beyond leaving the built-in
player in place.
