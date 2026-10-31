# Code conventions

These are the rules every change to the engine follows, whoever writes it.
Each rule says what it asks, why, which directories it covers and what checks
it, with a good and a bad example. [CONTRIBUTING.md](../../CONTRIBUTING.md)
covers the process around a change (the licence of contributions, pull
requests and review), and [testing.md](testing.md) covers writing and running tests in
more detail.

Some rules protect compatibility with Total Annihilation 3.1c: its save
files, the game data players already have, and the results of its
simulation. They are marked **load-bearing**. Breaking one breaks players'
games, often in ways no single test notices, so a change that needs to bend
one starts with an issue. The other rules keep the code readable and
consistent: any one change can get by without them, but a codebase that
ignores them becomes hard to learn.

## Contents

- [Existing code and new code](#existing-code-and-new-code)
- [Compatibility](#compatibility): behaviour and save files, canonical
  records, packed layouts, file decoding, determinism
- [Describe behaviour, never derivation](#describe-behaviour-never-derivation)
- [Naming](#naming)
- [Language rules by layer](#language-rules-by-layer)
- [Seams between modules](#seams-between-modules)
- [Documenting code](#documenting-code)
- [Comments](#comments)
- [Tests](#tests)
- [Application, extensions and run.sh](#application-extensions-and-runsh)
- [Checks](#checks)
- [Open decisions](#open-decisions)

## Existing code and new code

Parts of the tree predate some of these rules, and the rules describe where
the code is going as well as where it is. New and changed code follows them.
When you edit a file, bring the lines you touch up to the rules, but do not
reformat or rewrite code you are not otherwise changing: it hides the real
change in review. Where a check counts the existing departures from a rule,
its baseline holds a count per directory or file that may only go down; when
your change removes some, lower the count in the same change (see
[Checks](#checks)).

The newer simulation modules under `src/sim`, `src/sim/air` and
`src/sim/ai` show most of these rules in practice: plain structs, free
functions over the canonical `World`, and no exceptions, virtual functions
or standard library containers. Copy them rather than the older code under
`src/sim`.

A few rules are the recommended default for a choice the maintainer has not
confirmed yet. They are marked "(default; maintainer to confirm)" and listed
under [Open decisions](#open-decisions). Follow the default until it
changes.

## Compatibility

### Keep 3.1c behaviour, save files and game data (load-bearing)

A change does not alter the bytes written to or read from save files, which
game data is read or how it is decoded, or what the simulation computes,
unless an issue agreed it first. Behaviour of 3.1c that looks odd is kept on
purpose and documented with `@quirk` (see
[Documenting code](#documenting-code)).

- **Why:** players load their own saves and play their own copies of the
  game and its add-ons. A silent change breaks them where our tests may not
  look.
- **Applies to:** the whole engine.
- **Checked by:** the pinned simulation digests (`match-determinism`,
  `match-shared-random`, `match-trace`), the writer goldens
  (`hpi-writer-golden`, `sqsh-writer-golden`, `persist-squash-golden`,
  `persist-bank-golden`), `scenario-condition-accounts`, the
  `installed-content` sweep over an installed game and the native checks,
  which play, save and load matches headless. See
  [testing.md](testing.md#pinned-values).

Good: a fix that moves a pinned digest says so in its commit message, with
the reason, and updates the pinned value in the same commit.

Bad: a change that updates a pinned digest to whatever the new code prints,
with no reason given.

### Share the canonical records (load-bearing)

Every system reads and writes game state through the canonical records in
`src/core` (`Game`, `Unit`, `Player`, `UnitDef`, `WeaponDef`, ...) and the
`World` that owns them. Do not add per-module copies of unit, player or
world records. Pass the records themselves (`const oa::Unit&`,
`const oa::Player&`) instead of copying their fields into intermediate
structs. State that has no 3.1c layout yet lives in a side table indexed by
unit slot and declared by the module that owns it, never in `src/core`.

- **Why:** with one copy of the state, saving, loading, the trace and every
  system agree. Copies drift, and a save written from one copy loses what
  another holds.
- **Applies to:** all engine code.
- **Checked by:** review.

```cpp
// Good: the function reads the canonical record.
bool unit_selectable(const oa::World& world, const oa::Unit& unit);

// Bad: a struct that copies three Unit fields for one function.
struct SelectableUnit {
    uint8_t owner{};
    uint32_t state{};
    uint16_t health{};
};
bool unit_selectable(const SelectableUnit& unit);
```

### Keep packed record layouts pinned (load-bearing)

The records in `src/core` are packed (`#pragma pack(1)`). Each has an
`OA_ASSERT_SIZE`, and every named field an `OA_ASSERT_OFFSET`. Never reorder,
unpack or unpin a record. To name part of a byte array that code reads by
part, split the array so that every other offset stays where it was, and add
the new field's offset assert. `src/core` headers compile as C11 and as C++20.

- **Why:** save files hold these records byte for byte. The asserts turn a
  layout slip into a compile error instead of a corrupt save, and the C11
  headers keep the records usable from C and from any language that binds to
  C.
- **Applies to:** `src/core`, and any other record declared packed and pinned.
- **Checked by:** the compiler; the `core-types-*` and `core-world-*` tests
  compile the headers in both languages.

```c
/* Good: unknown_2[0x4] was split; the size and every other offset hold. */
typedef struct Example {
    int32_t range;
    uint16_t reload_time; /* ticks */
    uint8_t unknown_3[0x2];
} Example;
OA_ASSERT_SIZE(Example, 0x8);
OA_ASSERT_OFFSET(Example, range, 0x0);
OA_ASSERT_OFFSET(Example, reload_time, 0x4);
```

Bad: inserting a field between `range` and `reload_time`, widening
`reload_time` to `int32_t`, or deleting an assert that no longer compiles.

### Decode files explicitly and bound what you read (load-bearing)

At file and wire boundaries, read fixed-width fields with explicit
little-endian decoding from a byte span. Never cast a buffer to a struct
pointer or depend on the host's byte order or alignment. Reject malformed
input: check every length, count and offset against the bytes that remain
before using it, bound every allocation by a named limit, and report what
was wrong and where.

- **Why:** explicit decoding gives the same result on every compiler and
  platform and makes each read's bounds visible. The input is players' files,
  third-party add-ons and what other players' machines send, which can be
  damaged or hostile.
- **Applies to:** `src/formats`, `src/data/defs`, `src/data/persist`, any code that
  reads a file, and every decoder of network messages and recordings
  (`src/netgame`, `src/session/demo`, `src/ui/frontend-multiplayer`).
- **Checked by:** review; a malformed-input test for every decoder (see
  [testing.md](testing.md#malformed-input)); the sanitizer build in CI.

```cpp
// Good
if (bytes.size() < header_size)
    return fail(DecodeError::truncated_header, 0);
const uint32_t entry_count = read_le32(bytes.subspan(entry_count_offset));
if (entry_count > max_entries)
    return fail(DecodeError::too_many_entries, entry_count_offset);

// Bad: host byte order, unchecked length, unbounded allocation.
const auto* header = reinterpret_cast<const Header*>(bytes.data());
std::vector<Entry> entries(header->entry_count);
```

### Keep the simulation deterministic (load-bearing)

Simulation code gets the same result on every platform, compiler and build
type. It uses the game's fixed-point types (`oa_fixed`, 16.16) and integer
arithmetic where 3.1c does, and `float` or `double` where 3.1c does, at the
same precision. It draws random numbers only from the match's own random
streams, and never reads the wall clock or the frame time, object addresses
or the iteration order of hashed containers. One exception is allowed: an
object's address may be the key of a container that only finds that
object's own record and is never walked in the key's order, so that the
address changes how long a lookup takes and never what it finds
(`Match::order_bucket` in `src/sim/match-runtime`).

Floating point needs care of its own, because the same source can round
differently from one compiler or processor to the next:

- Never turn 3.1c's floating-point arithmetic into fixed point, or its
  fixed-point arithmetic into floating point, and never change the precision
  of a step: each changes the results.
- State each floating-point result by its precision and rounding: which
  steps are rounded to `float` and which to `double`, and in what order, as
  [src/base/game-math](../../src/base/game-math/README.md) does.
- Every operation is rounded as written. Never rely on the compiler fusing a
  multiply and an add into one step or rearranging arithmetic: build
  simulation code with both turned off (`-ffp-contract=off -fno-fast-math`,
  or the equivalent `/fp:strict`), and never turn them on for a target.
  Where a module cannot count on its build settings, it rounds each step
  through an intermediate the compiler must keep, as `src/sim/air`
  does.
- Prefer the operations IEEE 754 rounds exactly: addition, subtraction,
  multiplication, division, square root and conversions. A host maths
  function such as `std::sin` or `std::atan2` may differ between platforms in
  its last bit, and `long double` has a different precision on each; use
  neither. The game's arctangent, sine, cosine, arccosine and vector length
  come from [src/base/game-math](../../src/base/game-math/README.md), which
  computes them with integer arithmetic at the precision 3.1c keeps.

- **Why:** a replayed, saved and reloaded or shared match must play out the
  same way on every machine; one differing bit grows into a different game.
- **Applies to:** the simulation layers (see
  [Language rules by layer](#language-rules-by-layer)).
- **Checked by:** `match-determinism`, which pins the digests of a whole
  match on every platform CI builds, and `match-trace`.

```cpp
// Good: 16.16 fixed point; the low 16 bits of the 64-bit product are
// dropped, rounding toward negative infinity.
const oa_fixed step = oa_fixed((int64_t(speed) * turn_rate) >> 16);

// Bad: the frame time inside a tick, and a step 3.1c rounds to float kept
// in double.
const double step = double(speed) * turn_rate * seconds_since_last_frame();
```

## Describe behaviour, never derivation

Sources, comments, documentation, build files, test names, commit messages
and pull-request text describe what the engine does and why, in plain terms
and in the game's own terms. They never cite addresses of other programs'
code or data, never name other programs' executables or libraries, and never
say how an implementation was worked out or with which tools. Names of game
data files and formats (`totala1.hpi`, TDF, FBI, GAF, 3DO, COB, TNT, OTA) are
fine, and so are documented file and wire formats with their byte layouts.

The boundary for arithmetic fits in one line: **state arithmetic by its
result (rounding, truncation, wrap-around, which bits are kept); never by the
instruction, register, control word, compiler or library routine that would
produce it.**

- **Why:** the engine is a program of its own, and its sources must stand on
  their own. Wording about how another program is built teaches nothing about
  the engine and cannot be published.
- **Applies to:** every file in the repository, and commit and pull-request
  text.
- **Checked by:** review, of the sources and of every commit message and
  pull-request text; `style-ratchet` also counts names built from addresses.

Good:

- "Rounded to a 53-bit significand."
- "Truncated toward zero at 64 bits; the low 32 bits are kept."
- "The linear congruential sequence `x = x * 214013 + 2531011`, drawing bits
  16..30."
- "3.1c ignores this field." "A zero period is not allowed."
- "Counts above 255 are clamped, as in 3.1c." (a bound the engine keeps)

Bad (described here rather than quoted):

- a comment naming the processor instruction or the compiler's helper routine
  that rounds a value;
- a comment giving the address in another program where a table or routine
  lives, or a name with such an address attached;
- a comment saying a value was found by watching another program run, or
  that a routine follows another program's code;
- a comment describing another program's objects, its uninitialised bytes,
  its reads past a buffer, its crashes, or the order its code calls things
  in. Say what the game does instead, and where the engine guards against
  bad input, state the bound it keeps.

Engine code also never refers to an extension that is not part of the
engine, or to a project that builds on the engine, by name or by path.

## Naming

| Thing | Rule | Example |
|---|---|---|
| Module directory | `src/<group>/<module>`, kebab-case | `src/sim/detection` |
| Main public header | `include/oa/<group>/<module>.hpp`, hyphens as underscores | `oa/sim/mission_units.hpp` |
| Further headers | `include/oa/<group>/<module>/<file>.hpp` | `oa/data/persist/save_units.hpp` |
| Namespace | `oa::<group>::<module>`, hyphens as underscores | `oa::sim::detection` |
| CMake target | `oa-<group>-<module>`, or `oa-<group>-<module>-<part>` for a module's further library; a library outside a module (a test helper, a build option) `oa-<group>-<part>`. Each with the ALIAS that turns the first hyphen into `::` and the others into `_` | `oa-sim-detection` (`oa::sim::detection`), `oa-test-game-data` (`oa::test::game_data`) |
| Header suffix | `.h` only for headers a test compiles as C11; `.hpp` otherwise | `oa/core/unit.h` |
| Types and enumerations | PascalCase; `enum class` | `MissionKind` |
| Functions, variables, members, enumerators, constants | snake_case | `ticks_per_second` |
| Macros | `OA_UPPER_CASE` | `OA_ASSERT_OFFSET` |
| Private data members | trailing underscore | `match_` |
| Record fields | what the field holds; a `?` comment marks a tentative name; bytes the engine only carries are named by role and position | `sight_bonus`, `block_after_unit_defs` |
| Flag bits | one named constant per bit, named by what the bit does | `OA_UNIT_FLAG_SELECTED` |
| Module seam | `<Name>Hooks` struct (see [Seams](#seams-between-modules)) | `ParalysisHooks` |
| Test file | `<module>/tests/<name>_test.cpp` | `src/sim/detection/tests/detection_test.cpp` |
| ctest name | lowercase words joined by hyphens: what it tests (the module, its group and module, or the area of a family of tests), then a case when there are several. `native-` begins exactly the native checks, whose command is the game or a `tools/check_native_*.py` script; `-data` ends a case that reads the installed game; `-selftest` ends a check's test of itself; never the word `test` | `sim-detection`, `unit-health`, `match-trace`, `hpi-data`, `native-saveload`, `style-ratchet-selftest` |
| Format modules | named after the format; three take fixed names | `objects3d` (3DO), `fnt` (fonts), `smacker` (SMK) |

- **Why:** people navigate by names. When a module's directory, header,
  namespace and target agree, any one of them leads to the others, and one
  style of name means nobody has to guess. A test is found by what it
  tests, and the name says whether it is a native check or needs the
  installed game; the many test names that leave out their group (`unit-health`) or
  name a family's area (`match-`, `net-`, `frontend-`) are kept, since the
  scripts and command lines that name them would change for nothing a
  reader gains.
- **Applies to:** all engine code and its build files.
- **Checked by:** review; `engine-layout` (module directories, public
  headers, the names of every library of the tree and their ALIAS, every
  platform's included); `ctest-names` (the form of every test name, the word
  `test`, `native-`, `-data` and `-selftest`). What a test name begins with
  is left to review.

Names say what a thing holds or does. Never build a name from a record
offset (a placeholder word such as field, flags or word followed by a
hexadecimal offset), from `unk_` and an offset, from where the code came
from, or from a workstream or task label.

```cpp
// Good
inline constexpr uint32_t mission_result_finished = 5;
uint16_t reload_time{}; ///< ticks
void clear_selection(oa::World& world);

// Bad: a magic number, a name that restates a type, a name after a task.
if (result == 5) { ... }
uint16_t value_of_short{};
void w5_selection_pass(oa::World& world);
```

### Name every constant

Give every number with a meaning a name: a typed `constexpr` value, an
`enum class`, or in C11 headers a macro. Name known flags, states, opcodes,
limits, units and field offsets. Test a flag word's bits through their named
constants, never through a raw mask.

- **Why:** a named constant says what the number means and lets the reader
  find every use of it.
- **Applies to:** all engine code.
- **Checked by:** review; `style-ratchet` counts raw hex masks applied to
  flag fields.

```cpp
// Good
if (unit.flags & OA_UNIT_FLAG_SELECTED)

// Bad
if (unit.flags & 0x10u)
```

### Name every field by what it holds

Every field, constant and flag bit has a name a later reader can rely on;
none is a numbered placeholder. Name it, in this order:

1. by what the engine's code does with it: what it reads and writes, the
   values it holds, the file field or TDF key it is loaded from;
2. by the evidence of what 3.1c does with it, when the engine does not use
   it yet. A name whose evidence is short of certain is tentative: mark it
   with `?` at the start of its comment;
3. when nothing is known, by what it is to the engine, its role and position
   (`block_after_unit_defs`, `reserved_after_pitch`), with a comment saying
   what the engine does with it: saves carry it unchanged, it is
   zero-initialised and never read.

When code starts reading part of a byte array, split that part out as its
own typed field (see
[Keep packed record layouts pinned](#keep-packed-record-layouts-pinned-load-bearing)).
A value that means "not known at run time", such as an allegiance not yet
known, is a real state and keeps its name.

- **Why:** a placeholder tells a reader nothing and invites a guess; a
  wrong name misleads every later reader. A name from the code's own use, a
  marked tentative one, or a plain statement of the bytes' role says what
  is known and no more.
- **Applies to:** every record, constant and flag bit, in `src/core` and
  elsewhere.
- **Checked by:** `style-ratchet`, which counts names that number what they
  do not know (`name-unknown`), names built from offsets, and `unk_` names
  that carry an offset.

```c
/* Good: named by use, a marked tentative name, and bytes named by role. */
uint16_t cursor_feature;       /* feature under the cursor, 0 for none */
int32_t sight_bonus;           /* ? added to sight distance on high ground */
uint8_t block_after_tile_map[0x14]; /* zero-initialised and never read */

/* Bad: placeholders that say nothing, and a guess with no mark. */
uint16_t unknown_11;
int32_t sight_bonus;
uint8_t unknown_16[0x14];
```

## Language rules by layer

The engine is C++20 built with CMake. How much of the language a directory
uses depends on its layer (default; maintainer to confirm):

| Layer | Directories today | Rule |
|---|---|---|
| core | `src/core` | C11 records: plain C structs, no standard library containers, exceptions or virtual functions; layouts pinned |
| base, sim | `src/base/game-math` and the geometry, timers and sine table in `src/ui/services`; `src/sim/*`, `src/sim/*`, `src/sim/*`, `src/sim/ai`, `src/sim/*`, `src/sim/ballistics`, `src/sim/scenario`, `src/sim/session`, and `src/sim/*` except the renderer and sprite animation | In code a simulation tick runs, and in the state it keeps: no exceptions, no virtual dispatch and no heap-allocating containers. `std::array`, `std::span`, `std::optional`, `<bit>` and `<algorithm>` are allowed. |
| formats, data | `src/formats/*`, `src/data/defs`, `src/data/persist`, and the SQSH writer in `src/ui/services` | C++20; decoders take a byte span; errors are returned as values and no exception crosses the public interface; format libraries do not open files themselves |
| platform, present, audio, media, ui, netgame, session, app | `src/platform`, `src/platform/preferences/*`, `src/present`, `src/present/world-renderer`, `src/sim/sprite-animation`, `src/audio`, `src/media`, `src/ui/*`, `src/ui/*`, `src/netgame`, `src/netgame/*`, `src/session/demo`, `src/app`, `src/app/netgame`, and the rest of `src/ui/services` (cursor, input, labels, preferences, console commands) | C++20, the standard library and virtual interfaces allowed; exceptions stay inside a layer. At the extension table a hook, or a check-host entry, reports an error by throwing `std::runtime_error`; the engine catches a hook's at the call and handles it as the hook's documentation says (`extension.hpp`); a hook documented as one that must not throw does not throw |
| tests and tools | every `tests/` directory, `tests`, `tools`, `tools/oa-tool` | C++20 and the standard library; see [Tests](#tests) |

`src/ui/services` holds code of three layers until the layout pass
splits it; each part follows the rules of its own layer.

With the table go these rules:

- Core, base and simulation code is C-style C++20: plain structs, free
  functions, fixed-width types, `enum class` and `constexpr` constants,
  `static_assert` layouts, and no inheritance.
- In every layer, fixed-width types are written unqualified: `int32_t`, not
  `std::int32_t` (default; maintainer to confirm). The unqualified names
  come from `<stdint.h>`; strictly, `<cstdint>` promises only the `std::`
  names, and every toolchain the engine supports declares both.
- In every layer, each scalar and pointer member of a C++ struct has a `{}`
  default member initialiser. C11 headers cannot have one; the code that
  creates a C11 record zeroes it, as `world_create` and
  `world_alloc_tables` do.
- Platform presentation and audio stay separate from game state: the
  presentation layers read simulation state and never write it.

- **Why:** the simulation must be deterministic and bounded in time and
  memory, and a tick that throws half-way leaves the world half updated. The
  bans that serve those aims apply where they matter, and the rest of the
  engine uses ordinary C++. One spelling of the fixed-width types keeps the
  code uniform and matches the C11 headers.
- **Checked by:** `style-ratchet` counts, per file, `std::`-qualified
  fixed-width types and members without an initialiser everywhere, and
  exceptions, virtual functions and heap containers in the core, base and
  simulation directories of the table, outside their tests. It leaves out
  `src/ui/services`, whose layers it cannot tell apart until the layout
  pass. The counts may only go down.

When a simulation operation finds its state broken, or is given input it
cannot use, it returns an error value, or, inside a match, notes the fault
in the match's fault record and stops, leaving what it had done
(`Match::fault`, `fault.hpp` in `src/sim/match-runtime`; default;
maintainer to confirm). The application reads the record after each tick
and reports it as a simulation error, and a load or a check stops with it.
Add no exceptions to tick code.

```cpp
// Good: tick code in a simulation module.
struct Salvo {
    std::array<uint16_t, max_salvo_shots> targets{};
    uint8_t count{};
};

// Bad: a heap container, an exception and a qualified type in tick code.
struct Salvo {
    std::vector<std::uint16_t> targets;
};
void fire(Salvo& salvo) {
    if (salvo.targets.empty())
        throw std::runtime_error("no target");
}
```

### Layer order

Dependencies point one way: core, then base, then platform and formats (not
each other), data, sim (not platform), present, audio and media (which read
simulation state and never write it), ui with netgame and session (network
play's session and match, and the playback of recorded games, which the
multiplayer screens drive and which drive them), and app, the only layer
that wires the others together. Tools and tests may depend on anything. A module links
every library whose headers it includes, and never reaches into another
module's directory with `../`.

- **Why:** a one-way graph lets each layer be built, tested and understood
  without the ones above it.
- **Applies to:** new dependencies everywhere. The tree does not keep the
  order everywhere yet; the layout pass fixes the existing edges.
- **Checked by:** review; the layout check once the layout pass lands.

### Link no graphics API

No target of the tree, tests and tools included, links a graphics API: a
graphics library or framework such as `d3d9`, `dxgi`, `opengl32`,
`vulkan-1`, `GL`, `EGL`, `Metal` or `QuartzCore`, or one of CMake's
`OpenGL::` and `Vulkan::` targets. Platform code that needs one reaches it
at run time, through the objects SDL has made or through the system's
loader. What SDL's own target brings with it is SDL's.

- **Why:** one package runs on every machine of its system. A link to a
  graphics library makes every start load that driver, and stops the
  program from starting where the library is missing: an older Windows, or
  a Linux system without it.
- **Applies to:** every build file and source of the tree.
- **Checked by:** `engine-layout` (`tools/check_layout.py`), which reads
  the links the configuration made and every link the build files make in
  any branch, so that a link made for one system fails on every system. It
  reads a variable a link names with every value any build file gives it,
  in any function or scope, so a variable a link names needs a name no
  other build file uses for something else.

### Turn paths into text through UTF-8

A `std::filesystem::path` becomes text, and text a path, through
`oa::platform::path_to_utf8()`, `path_to_generic_utf8()` and
`path_from_utf8()` (`oa/platform/files.hpp`). Never write a path out with
its `string()` or `generic_string()`, or give it to `operator<<`, and never
make one from a `std::string` or `const char*` that holds a name read from
a file, typed by the player, given on the command line or sent by another
player. A name in the game data, which the game reads byte for byte, is
taken apart as text, as `oa::data::defs::file_stem()` does, never through a
path.

- **Why:** on Windows a path holds UTF-16, and its narrow conversions go
  through the system's code page. `string()` throws on a name that holds a
  character the code page lacks, such as a user name, or a map or mod file
  named in another script, and the game stopped with a filesystem error;
  a path made from UTF-8 text reads it in the code page and names another
  file. SDL, the preferences, the logs, the catalogue and the network carry
  UTF-8.
- **Applies to:** every source outside tests.
- **Checked by:** `style-ratchet` (`path-narrow`) counts `string()` and
  `generic_string()` of a path and a path given to `operator<<`, finding a
  path by the call that made it or the name it was declared with; making a
  path from narrow text is left to review. `platform-path-text` tests the
  conversions, and a folder whose names no one code page holds.

```cpp
// Good
error = "cannot write " + oa::platform::path_to_utf8(file);
const fs::path folder = oa::platform::path_from_utf8(values.at(key));

// Bad: throws on Windows for a name outside the code page, and reads the
// preference's UTF-8 in the code page.
error = "cannot write " + file.string();
const fs::path folder(values.at(key));
```

## Seams between modules

Between simulation modules, prefer a direct call where the layer order
allows it. Where a seam is needed (for tests, for a dependency that would
otherwise point upward, at the platform or at the extension table), use a
struct of function pointers named `<Name>Hooks`, or `Hooks` when the
module's namespace already names it (`oa::sim::selection::Hooks`):

- `void* context{}` comes first, and every function takes it back as its
  first parameter;
- every member has a `///` comment saying what it does and what a null
  pointer means;
- every member is `{}`-initialised, so an unset hook is null.

Virtual interfaces remain allowed outside the simulation layers and are not
converted for their own sake.

- **Why:** one seam pattern is easy to learn, to fake in a test and to read
  in a debugger, and it keeps the simulation free of virtual dispatch.
- **Applies to:** new seams everywhere; existing seams follow it when they
  are touched.
- **Checked by:** review.

```cpp
// Good
/// Effects of paralysis outside the canonical records.
struct ParalysisHooks {
    void* context{};
    /// Runs TargetCleared(slot) on the unit's script; null skips it.
    void (*target_cleared)(void* context, oa::Unit& unit, uint8_t slot){};
};

// Bad: the context last and named differently, members undocumented and
// uninitialised, and a virtual interface in simulation code.
struct ParalysisHost {
    void (*target_cleared)(oa::Unit&, uint8_t, void* user);
};
class ParalysisListener {
public:
    virtual void target_cleared(oa::Unit&, uint8_t) = 0;
};
```

## Documenting code

Every function has one `///` block directly above its declaration in a
header, public or private. A function with no header declaration has its
block above its definition; a helper that exists only in a `.cpp` file needs
at least the one-line summary, and grows a full block as its file is
worked on. The block holds:

1. a verb-first summary sentence in the present tense;
2. optionally, what a caller needs: the order of effects, the canonical
   state it changes, units such as 16.16 fixed point or ticks;
3. `@param` for every named parameter: a lower-case phrase with no full
   stop, stating units. Write `@param[out]` or `@param[in,out]` when the
   function writes through the parameter. Leave an unused parameter unnamed;
   it needs no `@param`;
4. `@return` for every function that returns a value;
5. `@quirk` for 3.1c behaviour the engine keeps on purpose, described in game
   terms.

Trivial overloads get the full block too. Struct members, enumerators and
constants get a `///` or `///<` comment when it adds something the name
cannot say.

- **Why:** the block is where a newcomer learns what a function promises
  without reading its body; the tick helpers, where orientation matters
  most, need it most.
- **Applies to:** all engine code.
- **Checked by:** review; `style-ratchet` counts functions declared in
  headers without a `///` block, and with Clang the `doc-check` test
  checks the form of the blocks in public headers (see
  [Checks](#checks)).

A worked example; the record is made up for the purpose:

```cpp
/// Size of one encoded sound-table entry, in bytes.
inline constexpr size_t sound_entry_size = 10;

/// The decoded fields of one sound-table entry.
struct SoundEntry {
    uint16_t sound_id{};
    uint16_t unknown_1{}; ///< always 0 in the shipped data
    int32_t volume{};     ///< ? loudness, 0 to 255
    uint16_t unknown_2{};
};

/// Decodes one sound-table entry from its little-endian encoding.
///
/// Reads only the first sound_entry_size bytes of `bytes`.
///
/// @param bytes the encoded entry, at least sound_entry_size bytes
/// @param[out] entry the decoded fields; left unchanged on failure
/// @return true when `bytes` held a whole entry
/// @quirk A volume above 255 is kept as stored, so that sound plays at full
///        volume instead of being rejected, as it does in 3.1c.
[[nodiscard]] bool decode_sound_entry(std::span<const uint8_t> bytes, SoundEntry& entry);
```

Bad:

```cpp
// decode_sound_entry: decodes a sound entry.
// Bytes: The bytes.
bool decode_sound_entry(std::span<const uint8_t> bytes, SoundEntry& entry, int flags);
```

It restates the name, uses `//`, gives no units, capitalises and ends the
parameter phrase like a sentence, does not mark `entry` as written through,
names a parameter it never uses and omits `@return`.

A module's `README.md` says what the module is for, its entry points, the
canonical state it reads and writes, its invariants and quirks, its tests and
its known limitations.

## Comments

A comment adds information the code cannot: units, ranges, truncation, the
reason for an order of effects, a `?` for a tentative name. It never
restates a name.

Byte offsets appear in comments only where they document the byte layout
of a file format: the game data decoders and the saved-game format. A
record's offsets are its `OA_ASSERT_OFFSET` pins; everywhere else, name the
field: `Game.local_player_index`, not an offset into `Game`. Code reads fields by name, never by offset arithmetic on
a record.

- **Why:** an offset in a comment is neither a name nor a guarantee. The
  pins keep a record's offsets true; a comment's offset goes stale
  unnoticed.
- **Applies to:** all engine code and documentation.
- **Checked by:** review; `style-ratchet` counts offset comments outside
  the file-format directories (`src/formats`, `src/data/persist`).

```cpp
// Good
const uint8_t viewer = world.game.local_player_index; // the player whose screen this is

// Bad: restates the name, and gives an offset instead of the field.
const uint8_t viewer = world.game.local_player_index; // local player index (+0x2a42)
```

## Tests

Tests state their expected values plainly: constants, the game data of the
player's installation, or behaviour. [testing.md](testing.md) holds the
details; in short:

- A test that reads game data takes the installation from the `OA_GAME_DIR`
  environment variable at run time, never from a compile definition, and
  links `oa-test-game-data` (`oa/test/game_data.hpp`,
  `oa/test/game_assets.hpp`). It is registered with
  `oa_add_game_data_test()`, or `oa_game_data_tests()` after
  `set_tests_properties()`, from `cmake/OaGameData.cmake`.
- Without an installation such a test prints one line saying what it skipped
  and exits with code 77, which ctest reports as skipped. With
  `-DOA_REQUIRE_GAME_DATA=ON` it fails instead.
- A pinned digest or golden value changes only on purpose, with the reason in
  the commit message.
- Every decoder has a test that feeds it malformed input.
- Checks report the file, line and failed expression and make the test exit
  non-zero; no test relies on `assert()`, which Release builds remove.
  `OA_CHECK` (`oa/test/check.hpp`, target `oa-test-support`) is such a
  check.
- Test code may use the whole C++20 standard library, since it runs outside
  the tick, and may throw, for example from a fixture's checks, as long as
  the test reports the failure with its file and line and exits non-zero.
  The code under test keeps its layer's rules (default; maintainer to
  confirm).

- **Why:** a test whose expected values anyone can check, and which says
  when it did not run, is one people can trust.
- **Checked by:** review; ctest; `style-ratchet` (`assert()` and
  `#undef NDEBUG` in tests).

## Application, extensions and run.sh

[`run.sh`](../../run.sh) is the entry point for seeing the engine's actual
progress. It builds the current source and launches the current native
application, `open-annihilation` (the `oa-game` target); a failed build stops
it rather than starting an older executable. Keep it that way as the application changes, and never let
it start another executable or a mock game. State current limitations
plainly instead.

Extension libraries add optional features through the table of hooks in
`src/app/include/oa/app/extension.hpp`: each is registered with
`oa_add_extension` (`cmake/OaExtensions.cmake`) and the function that fills
its table at startup, and the engine combines their tables by the rules that
header states. A hook no extension fills keeps the engine's behaviour.
Network play is the engine's own: the engine always registers
`oa-app-netgame` (`src/app/netgame`), which takes the 3.1c network switches
and runs the multiplayer screens, the networked match and the replay of
recorded games. A project that builds the game may register further
extensions, and engine code never refers to one. An extension reaches the
engine only through that table and declared headers: meet a new need with a
hook or a declared header, never with a new `Runtime` member or friend or
another use of a private `Runtime` name. Network play keeps its state for
each runtime in its own object, freed through a hook as the runtime goes;
its use of `Runtime`'s private names, and the `Runtime` members an
extension still adds, are frozen and may only shrink. Raise
`OA_EXTENSION_API_VERSION` with any change to the table's contract. From
version 15 the OA UI kit's public headers, `oa/ui/kit/*.hpp`, and the
registrations and services the table names (`oa/ui/screen_registry.hpp`,
`oa/ui/screen_registry/kit.hpp`) are part of that contract: an extension
builds its Settings pages, kit screens, notices, prompts and overlay
components from them. A change to a kit header before 0.8.0, the first
release that carries version 15, keeps 15 and updates the engine's own
followers and the test extensions in the same change; from that release
on, any change to a kit header raises the version, though the kit stays
provisional and changes as the engine's screens need.
A hook reports an error by throwing `std::runtime_error`, as the engine
code around it does. The engine calls every hook through one guarded call,
`call_hook` (`src/app/include/oa/app/hook_call.hpp`), which catches what
the hook throws before it reaches engine code and handles it as the hook's
documentation says: the paragraph on errors in `extension.hpp` says which
errors are raised as the engine's own, which are reported while the
engine carries on, and which hooks must not throw. [src/app/README.md](../../src/app/README.md) describes the table and the
frozen members.

- **Why:** an extension that is not part of the engine must be able to
  build on it without the engine naming it, and to tell, at compile time,
  whether the engine it builds against offers the contract it expects.
- **Applies to:** `src/app`, `src/app/netgame`, `src/ui/kit/include`,
  `src/ui/screen-registry`, `cmake/OaExtensions.cmake`, `run.sh`.
- **Checked by:** `runtime-surface-names` and `runtime-surface-selftest`
  (`tools/check_runtime_surface.py`, within
  `tools/runtime-surface-baseline.json`); `hook-calls` and
  `hook-calls-selftest` (`tools/check_hook_calls.py`), which fail when
  engine code calls a hook pointer other than through `call_hook`, and
  `app-hook-call`, which throws from a hook of each handling;
  `app-extension-list` and the
  extension tests in `tests/extension`, among them `extension-kit`, whose
  test extension uses the kit through the table alone;
  `netgame-runtime-surface`; CI builds
  and starts `open-annihilation`, and builds and tests it with the recorder
  test extensions beside network play in the extension-recorder job.

Game data never enters the repository: no archives, maps, sounds, movies,
or captures of them, apart from the small screenshots that show the
standard hacks on their pages, in `docs/mods/standard-hacks/images/`, the
sections of the settings, in `docs/images/settings/`, the UI kit gallery on
three windows, in `docs/images/ui-kit/`, and the trailer's
picture at the top of the README, `docs/images/trailer.jpg`.

The settings pictures are drawn, not edited by hand.
`oa-ui-engine-settings-pictures DIRECTORY` (built with the tests) draws
each section of the dialog alone, unlocked, at Compact and 2×, 960 by 648,
in the installed game's fonts, or with `--modern FONTS` in the modern look,
and writes them under the names that folder uses (`engine-limits.png`,
`controls-1.png`, `downloads-2.png` for Downloads scrolled to its end, and
the same for the other sections). It needs `OA_GAME_DIR`. The data test
`ui-engine-settings-pictures` runs it into a scratch folder. Refresh the
manual by drawing into a scratch folder and copying the pictures, Updates
included (`updates-1.png` up to date, `updates-2.png` a release offered,
`updates-3.png` the end with Previous version)
([the settings' module](../../src/ui/engine-settings/README.md#the-manuals-pictures)):

```sh
oa-ui-engine-settings-pictures pictures --modern local/deps/text-fonts
```

## Checks

One command builds everything and runs every test, the source checks among
them, and, when `OA_GAME_DIR` names an installation, the game-data tests and
the native checks:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$PWD/local/deps/sdl-install" \
    -DOA_GAME_DIR="/path/to/Total Annihilation" \
  && cmake --build build --parallel 8 \
  && ctest --test-dir build --output-on-failure
```

The command is written for a POSIX shell. On Windows, use the PowerShell
commands in [CONTRIBUTING.md](../../CONTRIBUTING.md#build-and-test). A
generator that holds several configurations, such as Visual Studio's, also
needs the configuration named when ctest runs:
`ctest --test-dir build -C Debug --output-on-failure`.

The source checks alone:

```sh
ctest --test-dir build --output-on-failure \
    -R 'doc-links|runtime-surface|style-ratchet|format-check|licensing-check'
```

| Check | What it enforces | Script and baseline |
|---|---|---|
| `style-ratchet` | qualified fixed-width types; names built from offsets, addresses or generated placeholders; names that number what they do not know (`unknown_3`); raw masks on flag fields; missing `{}` initialisers; offset comments outside file-format code; functions declared in headers without a `///` block; exceptions, virtual functions and heap containers in the core, base and simulation directories; `assert()` and `#undef NDEBUG` in tests; paths written out through the code page outside tests | `check_style.py` and `style-baseline.json` in `tools/` |
| `doc-links` | relative links, anchors and repository paths in Markdown resolve | `tools/check_links.py` |
| `ctest-names` | every ctest name keeps to the [naming rule](#naming) | `tools/check_ctest_names.py` |
| `runtime-surface-names` | the `Runtime` names an extension uses only shrink | `tools/check_runtime_surface.py`, `tools/runtime-surface-baseline.json` |
| `format-check` | every C, C++ and Objective-C source is laid out as `.clang-format` says; skips where the pinned clang-format is not installed | `tools/format_sources.py`, `tools/format/requirements.txt` |
| `licensing-check` | every file states its copyright and licence | `tools/spdx_headers.py`, `REUSE.toml` |

One more check runs only with Clang: the test `doc-check` builds the
target `oa-doc-check`, which compiles every public header with Clang's
documentation warnings as errors. It fails on a `@param` that names no
parameter, a `@return` on a function that returns nothing, an unknown
command (`@quirk` is declared as one) or a malformed block. With another
compiler the test skips. No other test runs beside it, since it builds in
the build tree. The first run compiles every header, about a minute at
eight jobs; a later one compiles again only what a changed header reaches.
The same check alone is
`cmake --build build --target oa-doc-check`. CI builds it on macOS. It is
defined in `OaDocumentationCheck.cmake` in `cmake/`.

Each script also runs on its own, for example `python3 tools/check_style.py`.
A baseline holds the findings that predate a check, counted per path and
rule. A run fails when a count grows. The style check fails too when a
count falls below its baseline, since that slack would let a new finding in
where an old one was fixed: when your change removes findings, lower the
baseline in the same change, for the style rules with
`python3 tools/check_style.py --update`. A count never goes up.

### What the style baseline still holds

Every style rule but two is down to zero findings. What remains are the
virtual functions (`tier-virtual`, 220) and heap containers
(`tier-heap-container`, 168) of the core, base and simulation tiers, all of
them in code that predates the rule:

| Area | `tier-virtual` | `tier-heap-container` | What they are |
|---|---|---|---|
| `src/sim/match-runtime` | 42 | 47 | the match's service, spawn, script and attack-order interfaces; the match's own unit, order, trace and ground-mission tables |
| `src/sim/unit-spawn` | 30 | 14 | the spawn and runtime-type hosts; the loaded types and legacy views |
| `src/sim/combat-state` | 16 | 17 | the spawn-geometry, target-search, intelligence and attack hosts; the weapon registry and projectile tables |
| `src/sim/script-vm`, `src/sim/script-state` | 22 | 16 | the script machine's host; script stacks and threads |
| `src/sim/ground-orders` | 10 | 17 | the order and movement-map hosts; the path search's heap, trace and worker |
| `src/sim/map-runtime` | 3 | 32 | the feature asset reader; feature, plot and height tables |
| `src/sim/simulation-state` | 19 | 0 | the order and unit update hosts |
| `src/base/game-loop` | 15 | 0 | the loop's subsystem host |
| other `src/sim` modules (unit-effects, unit-health, weapon-execution, visibility-state, unit-activation, model-runtime, scenario, spatial-state, unit-movement, world-environment, gameplay-input, unit-script, state-hash) | 63 | 27 | per-module hosts; sight patterns, model pieces, condition lists and small work lists |
| `src/data/campaign` | 0 | 2 | the campaign's asset lists |

They go down module by module, each in a change of its own that keeps the
recorded games and the saved-game digest unchanged:

- **Virtual functions:** a host interface becomes a table of function
  pointers with a context pointer, as `sim::messages::Hooks` and the
  console's host tables are. The match fills the table where it now
  derives from the interface. Start with the modules whose host has few
  entries (unit-activation, unit-movement, world-environment, scenario),
  then the shared ones (simulation-state, unit-spawn, combat-state,
  match-runtime), whose tests move to the tables in the same change.
- **Heap containers:** a table whose size 3.1c bounds (units, players,
  projectiles, features, script threads) becomes a fixed array of that
  bound, or a block the match allocates once at its start and owns for
  its life; a list built and dropped within a tick becomes a bounded array
  on the stack or in the match's state. The path search's heap and the
  traces, which hold more than 3.1c bounds, are allocated once at the
  match's start with their largest size.

Each such change lowers the baseline with
`python3 tools/check_style.py --update` in the same commit, so the
remaining counts can only fall.

### Formatting

Every C, C++ and Objective-C source of the engine is laid out as
`.clang-format` at the root of the tree says, by the one clang-format release
that `tools/format/requirements.txt` pins, so that every platform lays code
out alike. Fragments that another file includes in the middle of itself
(`.inc`, `.inl`) are left as written, and so is the order of includes. The
tools of this section and the next read the files Git tracks and the new
files it does not ignore, so they find a file you have just created before
you add it.

```sh
python3 tools/format_sources.py --setup    # install the pinned clang-format under local/
python3 tools/format_sources.py            # format the engine's sources
python3 tools/format_sources.py src/sim    # or only the files and directories named
python3 tools/format_sources.py --check    # change nothing; list what differs
```

A path named that holds no source to format is an error. `format-check`
runs the check. It skips where the pinned release is not installed; CI
installs it and runs the check on Linux.

### Licence headers

Open Annihilation is licensed under the GNU General Public License version 3
only. Every file of the engine states its copyright and licence. Sources,
build files and scripts open with two SPDX lines in their own comment style
(after a `#!` line), for example in C++:

<!-- REUSE-IgnoreStart -->
```cpp
// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
```
<!-- REUSE-IgnoreEnd -->

The copyright line names the project's authors and points to
[COPYRIGHT](../../COPYRIGHT) at the root, which states the holder once for the
whole repository, so no file names a person. The documents, and files that
cannot hold a comment (images, JSON, data), are covered by an annotation in
[REUSE.toml](../../REUSE.toml) instead, and licence texts (`LICENSE`,
`licenses/`) state their own terms. Run `python3 tools/spdx_headers.py` to
add the header to the files you create; `licensing-check` fails on a file
that states no terms, or whose header names another licence or another
copyright line, and names each one. It, not a standard
REUSE lint, is what the engine's files are held to: `REUSE.toml` uses the
REUSE format, but the tree does not claim full REUSE compliance.

## Open decisions

These rules follow the recommended default until the maintainer confirms or
changes them:

1. **Language rules by layer**, including whether tests follow them: the
   table in [Language rules by layer](#language-rules-by-layer) and the test
   rule in [Tests](#tests).
2. **Fixed-width type spelling:** unqualified (`int32_t`), counted by
   `style-ratchet`.
3. **The match-fault policy:** a simulation operation that finds its state
   broken returns an error value or notes the fault in the match's fault
   record and stops, as [Language rules by
   layer](#language-rules-by-layer) describes.
