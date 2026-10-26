# Total Annihilation unit-script save state

This portable C++20 component encodes and decodes the unit-script state payload
that Total Annihilation 3.1c writes to save files, and claims a free script
context. It is an exact payload codec and state representation. The enclosing
save-file section framing (`Script_%i` unit sections) is handled elsewhere;
`oa::sim::script_vm::Vm` supplies the live import/export bridge.

## Behaviour

- Export writes the context core, statics, motion fields, transforms and piece
  flags.
- Import requires the exact payload size and a matching script identity, then
  restores the same fields.
- The first word is the hash of the whole COB file that the unit-script loader
  records for the loaded script; the API calls it `script_identity_token`.
- Context allocation claims the lowest of the eight contexts whose
  execution-state word is zero. It stores `0x01000000`, the selected COB entry
  word, stack pointer `-1` and signal mask `1`, then increments the
  active-context count with 32-bit wrap. Sleep, wait, child and stack slots are
  left unchanged. Context word 8, a return callback, is set to zero;
  `ContextRecord` has no callback field.
- Units of one type share one copy of their script's code. Each unit still
  has its own statics, contexts and piece motion, and a save still writes one
  payload per unit.

## Payload layout

All words are 32-bit little-endian values. The exact size is:

```
0x528 + static_count * 4 + piece_count * 0x6c
```

The fixed `0x528`-byte core begins with the script identity token,
followed by eight `0xA4`-byte contexts and the active-context count. Each context
contains nine header words and 32 stack/local slots. The ninth header word is
the return callback, which means nothing outside the running game: save always
writes zero, and restore always clears it. `ContextRecord` deliberately has no
serializable callback.

Statics follow the core as raw signed words. Each piece then occupies 27 words:

| Words | Field |
|---:|---|
| 0..2 | move target for X, Y, Z |
| 3..5 | move speed per scheduler tick for X, Y, Z |
| 6..8 | turn target for X, Y, Z |
| 9..11 | turn speed per scheduler tick for X, Y, Z |
| 12..14 | spin target speed per scheduler tick for X, Y, Z |
| 15..17 | spin acceleration per motion update for X, Y, Z |
| 18..20 | live piece position for X, Y, Z |
| 21..23 | live piece angle for X, Y, Z |
| 24..26 | visible, cached, and shaded values |

Angle integration wraps the low 16 bits, where 65,536 units are one
revolution. The scale of the integer linear-position unit is not known, so no
physical unit is named.

Restore additionally sets every internal piece-motion active flag and the global
motion-active flag after applying the records. Those are restoration side effects,
not serialized fields; the live-VM bridge reproduces them.

## Bounds and deliberate adaptations

The codec limits statics to 65,536 and pieces to 4,096, validates size
arithmetic, requires the exact payload length, and validates identity before
allocating the output vectors. These are safety bounds. Raw context state,
stack pointers, active count and motion words are preserved because the
payload format does not establish semantic validity for them.

`decode` is transactional and exposes no partial state on failure.

The live `script-vm` bridge exports all eight context records, statics, the six
motion fields per axis, and host transforms and flags. Import validates live-VM
invariants before mutation, clears return callbacks, restores through real host
callbacks, marks every piece active, and enables global motion scheduling. The
codec does not install no-op behavior or invent an enclosing save format.

```sh
cmake -S src/sim/script-state -B local/build-script-state
cmake --build local/build-script-state
ctest --test-dir local/build-script-state --output-on-failure
```
