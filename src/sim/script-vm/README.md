# Total Annihilation unit-script VM subset

This portable C++20 component implements the integer stack, control-flow,
signal, child-script and sleep-scheduling core of the Total Annihilation 3.1c
unit-script (COB) interpreter. It is a bounded subset of the game's VM rather
than a new script language or a complete COB runtime.

## Behaviour

- `find_script`: case-sensitive linear search of the COB function-name table.
  The span length is the script count. The first exact match is the index;
  otherwise the result is `-1`.
- Context allocation: claims the lowest of eight `0xA4`-byte contexts, sets
  its entry point, stack pointer `-1`, signal mask `1` and active state; fails
  for a script index outside the table or a full pool.
- `Vm::query`: synchronous four-local query. A null pointer pushes zero and is
  skipped on writeback. The stack pointer is forced to 3, only that context
  runs at elapsed zero, and stack slots 0..3 are copied back. A RETURN operand
  is not the result. Allocation failure leaves the arguments unchanged.
- `Vm::tick`: visits all eight contexts in ascending order once per update,
  then integrates piece motion; `Vm::tick_context` is the interpreter entry
  with its sleep/wait-state dispatch.
- Piece move/turn/spin, immediate transforms, visibility/cache/shade flags and
  effect callbacks.
- Stack, arithmetic, comparison, logical and branch operations.
- Opaque host calls named by opcode and host-interface slot.
- A bounded random call on shared engine state and its result push.
- Opcode `0x10063000`: bounded local argument copy, discard and three-word
  advance; nothing further executes.
- Asynchronous child-script launch. With no context claimed the arguments stay
  on the caller's stack and the caller runs on.
- RETURN: optional return-value callback, stop and caller wakeup.
- Synchronous child call and caller wait state. With no context claimed the
  caller waits on child `0xFFFFFFFF`, which nothing wakes.
- Signal-mask context termination and caller wakeup.
- Sleep scaling and yield.
- Per-update move, turn and spin integration, target clamping, 16-bit angle
  wrapping and completion state.
- `Vm::export_state`: exports the eight raw contexts, active count, statics,
  motion, host transforms and piece flags; return callback words are zeroed.
- `Vm::import_state`: exact-size and identity-gated restoration, callback
  clearing, host restoration and forced piece and global motion activation.

Opcode `0x10037000` is bitwise XOR and `0x10038000` unary NOT, although some
published COB opcode tables label them otherwise.

## Bounds and unsupported operations

The component keeps the game's eight contexts. Each has the stack and local
slots the program's deepest script needs (`Vm::stack_slots`, walked once
when the program is set): the 32 of the game's context record at least and
4,096 (`limit::stack_slots`) at most. 3.1c never checks its 32: a deeper
expression writes over the contexts after it and runs on. Devolution's
beacon tests a unit's type with a chain of 75 comparisons whose partial
results all wait on the stack, 78 slots deep. A save keeps the first 32
slots of each context; a context saved while an expression holds more loses
the rest, and restores with zeros past them. Code is capped at 1,048,576
words; script and static tables at 65,536 entries each.
`find_script` returns `-1` for a null query or a null name-table entry. Every
other comparison is the game's case-sensitive exact match, bounded by the
supplied script count. Every stack access, inline operand, static/local index,
jump, signed division and per-tick instruction count is checked. An
unsupported instruction returns `unsupported_opcode` without changing state
for that instruction; in a tick it stops its context, and the tick goes on
with the other contexts and the pieces' motion.

A piece instruction with a bad operand runs on, as in the game
(`PieceFault`). 3.1c checks neither the piece, the axis nor the stack of its
piece transforms (MOVE, TURN, SPIN, STOP_SPIN, MOVE_NOW, TURN_NOW), their
waits, the piece flags and the piece effects: it indexes its piece tables
with whatever it is given, and a pop from an empty stack reads the word below
the stack, the context's return callback (zero for a script started without
one), and lowers the stack pointer past the bottom. Here an instruction whose
piece or axis lies outside the script is skipped, taking the values it would
have taken that the stack holds, and a value the stack lacks reads 0 and
leaves the stack empty; the script goes on either way. TAUIP's Hellfire, for
one, moves a piece in Create with no value pushed. Each such instruction is
noted the first time a VM runs it (`Vm::piece_faults`), for the match to log
once per unit type and script; running it again costs what a sound
instruction does.

Implemented: immediate/local/static push, allocation, local/static/discard
pop, wrapping 32-bit add/subtract/multiply, checked signed divide,
AND/OR/XOR/NOT, signed comparisons, logical operations, shared-host random,
absolute and conditional jumps, sleep, start/call/return including the optional
return-value callback, signal, signal-mask assignment, the synchronous
four-local query, and the observable argument-discard behaviour of opcode
`0x10063000`.

The `Host` interface is mandatory when bytecode reaches a host-facing
instruction. The VM keeps the game's piece-axis motion state and scheduling;
the host supplies current positions/angles, accepts updated transforms, piece
visibility/cache/shade flags and effects, and implements unit-value and
attach/drop callbacks. MOVE, TURN, SPIN, STOP_SPIN, immediate transforms and
their wait instructions keep the game's operand order and tick ordering.
The unit-script host resolves the four extra host slots: `0x10044000` asks
whether a unit id rides on this unit, `0x10045000` returns the carrier's id,
and `0x1000A000` (`dont_shadow`) and `0x10009000` (`ignored_piece_op`) are
no-ops there. RANDOM also uses `Host::random_bounded`, so its state remains in
the shared engine RNG rather than an invented per-VM generator. Mock-host tests
exercise the callback boundary.

`Vm::export_state` and `Vm::import_state` bridge the live interpreter to the
independent `oa-sim-script-state` payload codec. The public `ContextState` values
use the serialized words (`0x01000000` active and the `0x02x00000` wait
states), so save state does not depend on an invented enum numbering. Export
queries raw 32-bit piece flag values before transforms. Import validates
context and program bounds before mutation, clears all return callbacks,
restores flags before axis transforms, marks every piece active and enables
the global motion scheduler. Round-trip tests save a VM with simultaneous
running and sleeping contexts, local/stack/static data, partial motion,
transforms and flags, then require its restored execution to continue
byte-identically to the uninterrupted VM.

Live import additionally requires one of the six context-state words, a stack
pointer in `-1..31`, an in-range target for piece-wait states, and an active
count equal to the number of non-stopped contexts. These checks are a safety
policy. Program counters and child-context words remain bit-exact; the
interpreter bounds a program counter when execution reaches it, while the
no-free-child sentinel `0xFFFFFFFF` can intentionally wait forever.

Still unsupported: every instruction 3.1c does not handle, including extension
opcode `0x10039000`. The often-documented sound opcode `0x10072000` is not
handled by 3.1c either, so it is not invented here. Invalid PUSH/POP flag
combinations are rejected, and so are opcode `0x10063000` counts above four. A
host start or query that finds all eight contexts occupied reports
`no_free_context`, where the game returns 0.

The tests are known-answer cases constructed from the instruction semantics
above.

```sh
cmake -S src/sim/script-vm -B local/build-script-vm
cmake --build local/build-script-vm
ctest --test-dir local/build-script-vm --output-on-failure
```

## Engine calls (`oa-sim-unit-script`)

`oa/sim/unit_script.hpp` is the engine side of the COB interface over the
canonical `oa::World`. `ScriptFunction` enumerates every function name the
engine calls (Create, Killed, Activate/Deactivate, Start/StopBuilding,
Start/StopMoving, MoveRate1..3, SetSpeed, SetDirection, setSFXoccupy, the
Query/AimFrom/Aim/Fire weapon families, QueryNanoPiece, QueryBuildInfo,
SweetSpot, HitByWeapon, TakeDamage, TargetCleared, RockUnit, SetMaxReloadTime
and the transport set); `engine_call` records how each one is passed its
arguments. The engine uses three forms: a bare call (no stack arguments), an
argument call (four locals written and the stack pointer set to the argument
count minus one; TransportDrop relies on local 1 beyond its count), and a
synchronous query. Interpreters are attached per World and unit slot;
`unit_script_call`, `unit_script_call_bare`, `unit_script_query` and
`unit_script_tick` reach them. GET/SET unit values read and write the
canonical `Unit` fields through a small `UnitValueServices` boundary for piece
positions, trigonometry, terrain and activation side effects.
`unit-script-corpus` drives every engine function present in the 278 COBs of a
3.1c installation with those forms and requires no interpreter fault and
in-range piece answers.
