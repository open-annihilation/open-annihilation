# data/script-host

`oa-data-script-host` runs one package's Lua in a sandbox. A script can use a
small set of libraries, `require` only the package's own `lua/` files, and
stop at an instruction allowance, a memory cap or a real-time limit. Nothing
it does reaches a file, a clock, the environment or a network, and a failure
comes back as a value that names the package, the phase, the chunk and the
line.

The interpreter underneath is Lua 5.4.9, built as the C library
`oa-data-script-host-lua`. Code that pushes values into a state includes
`lua.hpp` through that library. The sandbox header does not include it.

## Entry points

`oa/data/script_host/script_host.hpp` (namespace `oa::data::script_host`):

- `mutator_profile` is the profile a mutator runs under: the default
  instruction allowance and memory cap, and no extra globals. `Sandbox::open`
  takes that profile (or another), the package id and a `SourceHooks` reader.
  The reader returns one package file by its exact path, or false when the
  package has no such file. It matches the path byte for byte.
- `run_host`, or the profile's `install`, registers host functions, userdata
  metatables and globals before any script runs. Both run inside the
  protected region. `install` runs once, during open. `run_host` runs the
  caller's work on a later phase and calls no script.
- `run_file` loads a package path as a text chunk and runs it as one phase.
  `call` runs the function `push` placed on the stack, then `read` sees how
  many results came back. Each of those phases has a fresh instruction
  allowance and a fresh real-time limit.
- `call_banked` runs one call the way `call` does, but the allowance is the
  bank's balance and the real-time limit is the bank's. The instructions
  that call used come off the balance. `refill_bank` adds one tick's
  allowance, and the balance never passes the limit. A profile with no bank
  makes `call_banked` return an error and leaves the sandbox usable.
- `drain_printed` hands the lines `print` kept to the caller, oldest first,
  then forgets them, so the next call can print again. Call it between
  phases. A null receiver forgets the lines without handing them over.
- `MemorySources` holds a package's files and serves them through
  `SourceHooks`. A path matches byte for byte. The hooks point at the
  object, so it must not move while a sandbox uses them.
- A false return fills a `ScriptError`. `describe` prints
  `<package>: <phase>: <chunk>:<line>: <message>`, and leaves the chunk and
  the line out when the chunk is unknown.
- `compile_check` loads a chunk in a throwaway state and does not run it.
- `charge_work` adds instruction units to the running phase and raises when
  the allowance is spent, as the library charges do. The units come from
  element counts and byte lengths. Host code outside this module, including
  the mutator collections and rule forms, uses it. No C++ object with a
  destructor may be alive across the call: the raise leaves it unrun.

## State

Each sandbox owns one Lua state and the lines `print` kept. Two sandboxes
share nothing: no module cache, no globals, no budgets. Moving a sandbox
moves that state; the moved-from object has no state.

## Invariants

- A script cannot reach a file, a clock, the environment, a process or a
  network. `load`, `loadfile`, `dofile`, `collectgarbage` and `string.dump`
  are nil. `io`, `os`, `debug`, `package` and `coroutine` are absent. `math` is
  the library described below.
  Every chunk is loaded as text. Bytes that start with the precompiled mark,
  and a file past the source cap, are refused before the loader sees them.
- `require` accepts only lower-case names, digits and `_`, in parts joined by
  dots, and reads `lua/<parts>.lua` through the caller's reader. The match is
  exact, so a file system that folds case cannot change which file loads.
- `open`, `run_file`, `call`, `run_host` and `compile_check` do their Lua work
  inside one protected call. A failure marks the sandbox failed, and every
  later phase returns that same error and runs nothing.
- Once the instruction allowance is spent, the real-time limit is past, or
  the sandbox has failed, `xpcall` returns the error unchanged and does not
  run the handler the script passed. A handler that loops, or one that
  catches errors with `pcall`, cannot keep the phase alive. An instruction
  error, a real-time error or a memory error is reported even when the
  script's own `pcall` caught it, including when an allocation spent the
  allowance.
- No C++ object with a destructor is alive across a Lua raise. `install`,
  `push`, `read` and the `run_host` work may raise and must not throw.
- The instruction allowance is per phase and is not a clock. The memory cap
  is the Lua allocation only. Passing either one sticks for that phase.
- Each phase also stops when it has run longer than its real-time limit. The
  default is `default_time_limit`, 10 seconds. `OpenOptions::time_limit`
  sets another for that sandbox. A steady clock is read when the phase
  starts and checked in the instruction hook, every 100 instructions, and in
  every library charge. The hook counts the allowance on every tenth of
  those calls, so the count still steps by `budget_check_interval` at the
  same instructions. There is no thread and no signal. Past the limit the
  phase ends with `time_limit` and the message `<phase> ran for more than 10
  seconds; its work is too large or never ends` (a limit that is not a whole
  number of seconds names that length in milliseconds). A `pcall` or
  `xpcall` cannot keep the phase going. The charges above are unchanged: the
  limit sits beside them. A phase that finishes inside the limit has the
  same results, the same charges and the same messages as it would with no
  clock.
- The check lands at the next hook or the next charge, so a phase can run
  past the limit by the work of 100 instructions, or of one charge. Most
  instructions take well under a microsecond; the slow ones are the steps
  Limitations names. The slowest found is an assignment that makes a table
  rebuild itself: 10 to 30 milliseconds of processor time for a table of a
  million keys, about the largest whose rebuild fits the default memory cap.
  A loop can do that every 4 instructions, 25 times between two reads of the
  clock, so it can run a quarter to three quarters of a second of processor
  time past the limit, and longer in real time on a machine busy with other
  work. On the machine where this was measured (10 cores, load averages of
  20 to 80), loops that rebuild such a table stopped 0.01 to 0.9 seconds
  after a 2 second limit, and in three runs at the busiest 2.4 to 2.9
  seconds after it. A loop of 64 KiB zero-filled comparisons stopped within
  60 milliseconds of a 300 millisecond limit, and a tight loop within the
  same millisecond. The limit only ever stops a script; it never changes a
  result. A script near the limit can pass on a fast machine and fail on a
  slow one. In multiplayer the loadout digest refuses a match whose machines
  disagree, so the difference cannot desync a match. A dry run records
  `time_limit` like any other failure.
- A profile may carry a bank. The defaults are 2,000,000 instructions a
  tick (`default_tick_allowance`), 30 ticks (`default_bank_ticks`) and 250
  milliseconds (`default_call_time_limit`). The bank opens full. Its limit
  is one tick's allowance times the tick count, and that count includes the
  current tick, so the default limit is 60,000,000. `ticks` below 1 counts
  as 1, and the product saturates at the largest count. A banked call
  spends the instructions it used, and no more than the balance it started
  with. The count steps as it does for any other phase, by
  `budget_check_interval`, so up to 999 instructions of each call go
  uncounted. The balance is that count alone: no clock, thread or signal.
  A phase that is not banked (`open`, `run_file`, `call`, `run_host`,
  `compile_check`) still uses the profile's allowance and leaves the
  balance unchanged.
- A banked call's real-time limit is the bank's, 250 milliseconds unless
  the profile names another. Past it the call stops with `time_limit` and
  the message `<phase> ran for more than 250 milliseconds on this computer;
  the stop is for the computer's speed, not the script's instructions`.
  The length is spelled the same way as the phase limit above (`250
  milliseconds`, `1 second`). A phase that is not banked keeps
  `OpenOptions::time_limit` and today's sentence. A `pcall` or `xpcall`
  cannot keep a banked call going past a spent balance or a passed time
  limit.
- `drain_printed` hands out every kept line, oldest first, including the
  omission note when lines were dropped, then clears the print limits. The
  next call can keep `max_printed_lines` lines again. It is only for the
  gap between phases.
- `string.find`, `string.match`, `string.gmatch` and `string.gsub` count
  their work against that allowance. A step is one entry into the matcher,
  one character a `*`, `+` or `-` repetition tries, or one character a `%b`
  pair scans. When the count reaches the allowance, the phase stops with the
  same instruction-budget error as a loop that does not end, and a `pcall`
  around the call does not let the phase continue. Within the allowance the
  four functions return the same values, of the same types, and raise the
  same errors, as Lua 5.4.9. A call on a string value, such as `("x"):find`,
  uses the same functions.
- A new string longer than 40 bytes counts against that same allowance: one
  unit per 32 bytes of its length, rounded up. A string of 40 bytes or fewer
  is shared, and making one allocates only when no live copy exists, which
  depends on when the collector last ran, so it is not counted; the
  instruction or call that makes it bounds that work. The header in front of
  the bytes is not counted, and neither is any other block. Every count
  depends only on the script and the values the host gives it, so the same
  script spends the same count on every build and every run. Freeing a
  string does not give the units back.
- `string.byte` counts one unit per byte it returns. `utf8.codepoint` counts
  one per code point it returns. `utf8.codes` counts one unit per 32 bytes
  the iterator walks, including continuation bytes, on every call, including
  the call that finds the end. `utf8.len` and `utf8.offset` count one unit
  per 32 bytes of the interval they walk. A plain `string.find` counts one
  unit per 32 bytes each scan and each comparison examines, as the search
  goes. `require` counts one unit per caller it walks.
- `table.move`, `table.insert`, `table.remove`, `table.concat`,
  `table.pack` and `table.unpack` count one unit per element they move or
  visit. A range
  longer than the default memory cap could hold, one element per value the
  interpreter stores, is refused before the call loops. The message names
  the function, the number of elements and that limit. `table.sort` counts
  every comparison, including a comparison function written in C. Two
  strings cost one unit per byte of the two lengths, and at least 2 units.
  Any other comparison costs 2 units, about the time of one or two
  instructions. Within these limits the calls return the same values and
  raise the same errors as Lua 5.4.9.
- `next` counts 4096 units per call. `pairs` counts 4096 units for the walk
  that collects the keys, and each ordering of two keys costs the same as a
  `table.sort` comparison. `rawequal` of two strings counts one unit per 32
  bytes of the two lengths. `tonumber` of a string counts one unit per 32
  bytes of that text. `string.format` counts one unit per format byte, 2
  units per value it converts, and one unit per byte of each text it copies
  with `%s` or `%q`. `utf8.char` counts one unit per code point.
  `math.max` and `math.min` count a comparison, as `table.sort` does, for
  each value after the first. `fixed.min` and `fixed.max` do the same.
  `fixed.parse`, and `fixed` of a text, count one unit per 32 bytes of that
  text. `warn` counts 2 units per argument.
- `charge_work` is that same count for a walk the libraries do not make.
  One walk that collects keys costs `scan_call_charge` (4096). A `where`
  or `:change` table costs `table_walk_charge` (64), then `element_charge`
  (1) per entry. Past the entry limit only that count is charged, so the
  count does not depend on which keys `next` visits first. When every key
  is visited, each key's bytes cost `units_for_bytes`, one unit per 32
  bytes. A comparison costs `comparison_unit`. The count depends only on
  those sizes, so one script spends the same count on every build and
  every run. A call that stays inside the allowance keeps the values and
  the errors it would raise with no count.
- `next`, `rawequal`, `tonumber`, `warn`, `string.byte`, `string.format`,
  the charged `table` and `utf8` calls and `utf8.char` are copies of Lua
  5.4.9's with the count inside. The script calls them directly, so an
  error names the function, numbers the arguments of a method call such as
  `("%d"):format(x)` as Lua does, and starts with the script's chunk and
  line. `string.format` also holds the two refusals under Text.
- No string may be longer than 65,536 bytes. Every string the interpreter
  creates passes the allocator, and Lua has no switch that stops a longer
  one, so the cap is applied there. The header in front of the bytes differs
  by build, so each state measures it once, from one 200-byte string, before
  the libraries open. A longer string ends the phase with `string_length`.
  The message names the size and 65,536, at the line that made the string.
  `string.rep`, `table.concat` and `string.gsub` refuse earlier with the
  same error when they can see the size coming. `print` keeps at most one
  byte past its own 16 KiB line, so a long value is cut instead of refused.
  A `pcall` that catches the refusal does not let the phase continue. A
  value the host pushes counts as a string too, so text longer than 65,536
  bytes fails the script that receives it. A string literal that long fails
  the chunk that contains it.
- The sources in `third_party/lua` are unchanged. Every string hash uses one
  constant seed, `0x4F414C55` (the letters OALU), so two states walk the same
  table in the same order wherever they sit. `table.sort` does not draw a
  pivot from the clock. Numbers are read and written with `.`: a sandbox
  opens only in the C numeric locale (below). Hexadecimal numbers are
  converted by Lua's own code on every platform. The interpreter is one C
  translation unit, with the engine's floating-point options: no fused
  multiply-add and no value-changing optimisation, and Visual Studio's
  strict floating-point mode.
- A sandbox opens only when the C library's collation, numeric and
  character-type locales (`LC_COLLATE`, `LC_NUMERIC` and `LC_CTYPE`) are each
  `C` or `POSIX`. String comparison and `table.sort` on strings then follow
  byte order, number text is read and written with `.`, and `upper`, `lower`
  and the pattern classes such as `%a` treat a byte past 127 the same way on
  every machine. Any other setting is refused at open, naming the category
  and the locale: `the C library's numeric locale is de_DE.UTF-8; scripts
  need C`.
- `setmetatable` installs a copy of the metatable. The copy keeps every
  field the caller's table had at that moment, including `__index`, so a
  methods table that `__index` points at still sees fields added later.
  A later write to the caller's own table does not change the copy, and the
  copy refuses `__mode`. `rawset` of `__mode` is refused on every table.
  No script can make a weak table. `getmetatable` returns the copy, or the
  `__metatable` field when the caller set one. Making the copy counts 64
  units, and one unit per field copied, so a class of 21 fields set on 5,000
  values costs under 1% of the default allowance.

## Maths

`math` is Lua 5.4's library. `sin`, `cos`, `tan`, `asin`, `acos`, `atan`,
`exp` and `log` (every base), and the `^` operator, are computed by the
engine, so a script gets the same bits on every machine. Sine, cosine and
tangent come from the engine's 64-bit angle functions and are rounded once
to a double. An angle at or above 2147483648 in magnitude, infinity
included, is refused. A NaN comes back as a NaN. `exp` and `log` are series
on the same 64-bit numbers, rounded once at the end. `log` of a power of
two, and of `10^k` for k from 0 to 22,
is the exact integer. `^` follows the usual power rules. An exponent of 2
is an IEEE multiply, and an exponent of 0.5 is an IEEE square root. An
integer exponent of magnitude at most 256 is repeated squaring at 64 bits.
A larger integer exponent is `exp(y · log x)` on two 64-bit words, and a
negative base keeps the sign an odd integer calls for. `sqrt`, `floor`,
`ceil`, `fmod` and the four
basic operations are the IEEE results. The compatibility names `cosh`,
`sinh`, `tanh`, `pow`, `frexp`, `ldexp` and `log10` are absent.

`math.random` is Lua 5.4's generator. It starts from `OpenOptions::random_seed`,
which the caller sets from the game's seed, and `math.randomseed()` with no
argument returns to that same seed. It does not read a clock or a place in
memory. Zero is a defined sequence.

## Fixed-point numbers

`fixed` counts 65536ths, the scale of the engine's 16.16 `oa_fixed`, in a
signed 64-bit integer. A whole number is that number times 65536. The whole
part runs from -2147483648 to 2147483647, so a count runs from -2^47 to
2^47 - 1. A result past either end raises and names the operation. Nothing
wraps or stops at the end.

`fixed(v)` takes a fixed value (that same value), an integer (times 65536),
a float or a string. `fixed.parse(text)` reads text the way `get_fixed`
does: `parse_double`, times 65536, truncated toward zero with
`truncate_to_int64`, and it keeps the full count rather than the low 32
bits. Text with no digits reads as 0, and characters after the number are
ignored, so `fixed("abc")` is 0 and `fixed("1,5")` is 1. `fixed.raw(n)`
builds a value from an integer count; a fractional count is refused.
`fixed.min` and `fixed.max` take one or more values, each converted as
`fixed(v)` is.

`f:raw()` is the count. `f:text()` and `tostring(f)` are `format_number` of
the count divided by 65536, so the decimal is exact. `f:tonumber()` is that
quotient as a float. `f:floor()` shifts the count right by 16, toward minus
infinity. `f:ceil()` moves the other way. `f:round()` takes a half away
from zero. Those three return integers, not fixed values.

`+`, `-`, `*` and `/` convert a number the way `fixed(v)` does and refuse a
string. `*` builds the product and shifts it right by 16, so a negative
product rounds toward minus infinity, as `scale_vector_fixed` does. `/` is
the dividend times 65536, divided in 64 bits and truncated toward zero.
Dividing by zero raises. Unary minus negates the count.

Every operation is integer arithmetic. Three conversions touch a
floating-point value, and each one is exact or is the loader's own. A float
times 65536 is truncated toward zero. Text goes through `parse_double`, the
function the loader calls. Text and `tonumber` divide the count by 65536,
which is exact for every count in range. The same counts come out on every
build, the 32-bit ones included.

A rule file still computes on the field's text. `MaxVelocity=*1.5` on a
field written `1.6` stores `2.4`, and the game reads that as 157286.
`u:fixed("maxvelocity") * fixed("1.5")` multiplies the counts the game
holds, 104857 and 98304, and the shift leaves 157285. Compute stats with
`fixed`.

`fixed(1) == 1` is false. Lua calls that comparison only when both values
are userdata. `<` and `<=` accept a number on either side. `//` has no
metamethod, so it raises Lua's own arithmetic error. Use `(a / b):floor()`.
A fixed value is a poor table key: two equal values are two keys, and
`pairs` refuses a userdata key. Use `f:raw()` as the key. `getmetatable`
does not hand back the metatable.

## f32

`f32` holds one IEEE binary32 value, the width of a player's resources and
build progress. A profile opens it from `install` by calling
`open_f32_library`. A sandbox whose profile does not call that has no `f32`.
A mutator does not see it.

Arithmetic follows C++ promotion, so a script can mix these values the way
the built-in player mixes float and double. Two `f32` values, or an `f32`
and an integer, use float arithmetic and the result is an `f32`. The integer
is converted to float once, directly, and is not converted to a double
first. An `f32` and a Lua float widen the `f32` exactly and use double
arithmetic, and the result is a Lua float. Comparisons use the same rules.
`//`, `%` and `^` have no metamethod, so Lua raises its own arithmetic error.

An `f32` and a `fixed` value do not mix. The operation raises, names itself,
and tells the script to convert with `f32(x)`.

`f32(v)` takes an `f32`, an integer, a Lua float or a `fixed` value. Each
conversion rounds once, nearest, ties to even. A fixed count is divided by
65536, which is exact for every count in range, and that quotient is what
converts to float. `v:tonumber()` widens exactly. `v:tofixed()` multiplies
the widened value by 65536 and truncates toward zero, as `fixed(number)`
does. A NaN, an infinity or a count outside the fixed range raises.
`v:bits()` is the integer 0 to 4294967295, and `f32.from_bits` builds a
value from those bits. A NaN produced by an operation, or by narrowing a
double, is stored as the quiet NaN whose bits are 0x7fc00000, so every build
agrees. `from_bits` keeps the bits it is given until an operation.

`f32(1) == 1` is false. Lua calls that comparison only when both values are
userdata. `getmetatable` does not hand back the metatable.

`f32` follows the build exactly as the simulation's own floats do. Each
operation is rounded as written. Under `OA_X86_FLOAT=fpu` the simulation's
floats change too, and `f32` changes with them.

## geometry

`geometry` is a plain table. A profile opens it from `install` by calling
`open_geometry_library`, and later code may add fields to the table. A
sandbox whose profile does not call that has no `geometry`. A mutator does
not see it.

`geometry.direction(x, z)` is the engine's heading, an integer from 0 to
65535. `geometry.planar_length(x, z)` is the engine's planar distance as a
fixed value. `geometry.length(x, y, z)` is the engine's truncated length as
a fixed value, and a length past 2^31 stays negative. `geometry.scale(x, y,
z, factor)` is the engine's fixed-point scale and returns three fixed
values. Each function calls the engine's own geometry. An argument is a
fixed value or an integer. An integer is a whole number of world units,
times 65536. A count that does not fit in a 32-bit coordinate raises, and a
float is refused. Scaled sine and cosine are not in this table.

## Iteration

`pairs` visits number keys in ascending order, then string keys in byte
order, then `false` and then `true`. The key list is taken when `pairs` is
called. A value changed before the walk reaches it is the value the walk
sees, and a key whose value became nil is skipped. Keys added during the
walk are not visited. A table whose keys include a table, a function or any
other type with no fixed order raises, and tells the script to keep those
values in a sequence and walk it with `ipairs`. A table with a `__pairs`
metamethod keeps that metamethod. `ipairs` is unchanged. `next` returns the
same key the interpreter would, so `next(t) == nil` is still the emptiness
test, and its order is not fixed: it depends on the table's history, and a
key that is itself a table has no order that two machines share. The call
is charged as the invariants describe. A mutator that lets the last write in
a loop win, or that joins text in loop order, has to use `pairs` or a
sequence. Depending on `next` builds different game data on different
machines.

`name_less` compares names without regard to ASCII letter case, then breaks
a tie by the raw bytes. `sort_names` uses that order and does not call Lua.
`push_names` pushes a sequence in the order it is given; the caller sorts
first, outside any protected region. `push_sorted_keys` pushes one table's
keys in the `pairs` order and raises as `pairs` does.

## Text

`tostring` of a float is the same characters on every machine. A NaN of
either sign is `nan`, the infinities are `inf` and `-inf`, and every finite
value is `%.14g`, with `.0` added to an integral value the way Lua does.
Joining a float to a string uses that same text.

A table, a function or any other value without `__tostring` prints as its
kind, a colon and `hidden`, for example `table: hidden`. It does not print
where the value sits. `__tostring` is used when the metatable has one, and
`__name` is used as the kind when it has no `__tostring`.

`string.format` is the charged copy of Lua's own format, with two
refusals inside it, each raised at the script's line. `%p` is refused,
because it would show where a value sits. `%a`, `%e`, `%f` and `%g`, in
either case, are refused when the number is a NaN or an infinity; the
script tests the number first, or uses `tostring`. A finite number, `%q`,
`%s`, `%d`, `%i`, `%x` and `%c` are unchanged. `%q` of an infinity is
`1e9999` or `-1e9999`, and `%q` of a NaN is `(0/0)`.

`string.pack`, `string.unpack` and `string.packsize` are nil. Their sizes
and alignment are not the same on every machine the engine builds for.

## Profiles

A profile is a name, an instruction allowance, a memory cap, an optional
`install`, and an optional bank. One interpreter serves every profile.
`mutator_profile` is the default and carries no bank, so every phase still
gets the allowance in `budget`. A profile whose bank has a `per_tick` above
zero opens with that bank full: a computer player can spend a saved balance
on one heavy turn, and a refill puts one tick's allowance back, up to 30
ticks including the current one. The profile adds its own functions from
`install` without a second interpreter. Each sandbox still has its own state.

## Plain data

`oa/data/script_host/plain_data.hpp` turns one Lua value into bytes a saved
game can keep, and pushes those bytes back as a value. The bytes are the
same on every build, and the same whatever order a table was filled in.

Plain data is nil, booleans, integers, floats, strings, fixed counts and
tables of those. Nil is only the top value, the one a save that returns
nothing produces. A fixed count is the userdata `fixed` builds. Anything
else is refused, and the refusal names the place from the top, as
`saved.units[3]: a function cannot be saved`. A string key that is a name
is written after a dot. Any other string key is `["…"]`. An integer key is
in brackets.

The bytes are little-endian. They start with the four letters `OAPD` and a
format byte `1`, then one value, and nothing after it:

- `0` nil, `1` false, `2` true;
- `3` and an 8-byte two's-complement integer;
- `4` and the 8 bytes of a double. A NaN is written as the quiet NaN
  `0x7ff8000000000000`. A signed zero stays signed;
- `5`, a 4-byte length and the bytes, at most 65,536;
- `6` and an 8-byte fixed count inside the fixed range;
- `7`, a 4-byte entry count, then each key and its value. Integer keys come
  first, ascending. String keys follow, in byte order. A key is itself an
  integer or a string value.

A table reached twice, without being inside itself, is written twice and
comes back as two tables. A cycle is refused. A table with a metatable is
refused. A key that is not an integer or a string is refused. Lua already
stores a float key whose value is a whole number as an integer, so that key
is saved as an integer and `math.type` of a real integer stays `"integer"`.
A float and an integer stay distinct.

The value may be 64 levels deep, counting the outermost, and the bytes may
be 1 MiB, header included. One more level, or one more byte, is refused.
Encoding charges `units_for_bytes` of the encoded size plus one unit per
value written, keys included, and only after the walk has dropped its
containers: the walk makes no call that can raise, and the charge can.
Nothing is charged when the value is refused. Pushing checks the bytes
first, without Lua, and raises `the saved data is damaged: …` when they are
not one plain value. It then charges the same count as it builds the
tables and strings. A string longer than 40 bytes also pays the usual
string charge when it is created.

The caller keeps its `PlainData` and `PlainDataProblem` outside the frame
a raise can skip. `data-script-host-plain-values` checks the bytes, the
refusals, the round trip and the charge.

## Tests

`data-script-host-lua` loads a text chunk and checks that the state frees
every allocation, checks the libraries that are present and absent and the
number text they write, checks that three states walk one table in one
order, and refuses a precompiled chunk.

`data-script-host-sandbox` uses a made-up package. It calls a function the
chunk defined, checks the libraries a script can and cannot see, the
`string.rep` cap, the refused metatable keys, `print`, `require`, errors
with chunk and line, a failed sandbox, `compile_check`, `run_host`, and two
profiles in one process.

`data-script-host-budgets` uses a small test allowance and one run at the
mutator default. A plain loop, a loop inside `pcall`, and an `xpcall` whose
handler loops or catches with `pcall` all stop on the instruction allowance.
Two phases that each
use part of the allowance both pass. Keeping strings stops on the memory
cap, including inside `pcall`. A required module that loops forever is
reported at that module's chunk.

`data-script-host-time-limit` gives a phase a short real-time limit and an
instruction allowance it cannot spend. A tight loop, a loop of 64 KiB
zero-filled comparisons, a loop of calls with 300,000 arguments, and the
same tight loop inside `pcall` and inside `xpcall` with a looping handler
each stop with `time_limit` within a second of the limit. A script that
finishes inside the limit returns the same value and spends the same
instruction count as it does under the default limit, and the next phase
starts its limit again. The protected-metatable error from `setmetatable`
matches Lua's own text, chunk and line included.

`data-script-host-maths` checks the engine maths against
`tests/maths_reference.py`, the exact powers of two and ten, and the math
library's argument rules. `tests/random_reference.py` prints the generator's
draws. Both scripts are run with `python3 -I` and print the tables the test
holds. `data-script-host-fixed` checks every count against
`tests/fixed_reference.py` the same way, through the C++ functions and
through a sandbox. `data-script-host-f32` checks every value against
`tests/f32_reference.py` the same way, through the C++ conversions and
through a sandbox whose profile opens `f32`. The reference is exact rational
arithmetic, run with `python3 -I`, and prints the table the test holds. A
sandbox on the mutator profile has no `f32`. `data-script-host-geometry`
checks direction, planar length, truncated length and scale against the
engine's own functions, including a length that wraps past 2^31, and a
mutator sandbox has no `geometry`. `data-script-host-iteration` checks
`pairs`, the refusal of unordered keys, and the name helpers.

`data-script-host-patterns` runs each pattern case through the sandbox and
through Lua's own string functions and compares every returned value, its
type, and any error text. It also checks that a match which backtracks past
the allowance stops with an instruction-budget error, including when the
script catches that error itself, and that a long match which stays inside
the allowance adds to `instructions_used`.

`data-mutators-charge` runs the mutator collections and rule forms through
`charge_work`. A `where` over a 4,000-key table, repeated, and a collection
`pairs` loop each stop on a small instruction allowance. A small plan stays
inside one hundredth of the default allowance and spends the same count on
two runs.

`data-script-host-c-work` compares every copied call with Lua's own
results and error messages, chunk and line included, method calls among
them, and checks that a loop or an impossible range in each one stops the
phase. That includes a plain find, a direct `utf8.codes` call, `table.sort`
of a C comparison and of long zero-filled strings, an allocation loop
caught with `pcall`, `string.format` copying long texts before an error
the script catches, `table.pack` of 300,000 values, and `setmetatable`
with a metatable of 30,000 fields. It checks that a loop of short strings
costs the same with an empty heap and with 20,000 live tables, and that
walking, clamping, formatting and sorting the fields of 400 units stays a
small part of the allowance. It also checks that `utf8.codes` returns one
function, that the pack functions stay absent, and runs a phase that reads
several hundred small tables and rewrites their text.

`data-script-host-string-length` checks that a string of 65,536 bytes is
kept for each way of making one, and that each way of making a longer one
stops with `string_length`, naming the place it happened. It also checks
that catching the refusal does not let the phase continue, and that filling
memory with shorter strings is still a memory error.

`data-script-host-text` checks the float spellings, `hidden` in place of
where a value sits, the `string.format` refusals, the absent pack functions,
C collation, the refusal to open under another collation, numeric or
character-type locale, and that no route makes a weak table.

`data-script-host-bank` uses a made-up package. The bank opens at 30 ticks,
a refill stops at that limit, and a profile with no bank stays usable. A
banked call spends exactly the instructions it counted, and two sandboxes
on the same calls and refills hold the same balance. Spending the balance
down, refilling, and overspending stops with the banked instruction
sentence. A loop, including one inside `pcall` and one inside `xpcall`
with a looping handler, stops on the balance and the failure sticks. A
banked call that cannot spend its allowance stops on the bank's real-time
limit, with the sentence that names the computer's speed; a phase that is
not banked in that sandbox keeps the sandbox's own limit and today's
sentence. `run_file`, `call` and `run_host` keep the profile allowance and
leave the balance unchanged, and after a banked call a `call` reports
today's instruction sentence again. `drain_printed` hands 300 printed
lines out as 256, in order, ending with the omission note, and the next
call can print again. A drain with no receiver forgets the lines.

`data-script-host-memory-sources` stores files and reads them back by the
exact path, including through `SourceHooks`. A different case and a
trailing slash do not match. Replacing a path keeps one file. A sandbox
opened on those hooks runs `brain.lua`, and `require("a.b")` reads
`lua/a/b.lua` from the same object.

`data-script-host-plain-values` encodes plain values to fixed bytes, including
one table built five ways and from two sandboxes. A round trip keeps
integers, floats, signed zero, infinities, a NaN, strings, fixed counts and
nested tables, and encoding the result gives the same bytes. It refuses a
function, userdata that is not fixed (including an `f32`), a metatable, a
cycle, a boolean key, a float key, 65 levels and 1,048,577 bytes. Checking
refuses a truncated value, a bad tag, unsorted and repeated keys, a string
past 65,536 bytes, a fixed count out of range, trailing bytes and a nil
below the top. Damaged bytes fail the phase. The same value charges the
same count twice, and a larger value charges more.

## Limitations

Comparing two strings with `<` or `==` is one instruction of the
interpreter, and the library charges above do not see it. Equality of two
65,536-byte strings takes about 1.3 microseconds on the machine where this
was measured. Ordering two such strings of ordinary bytes takes about 3.3
microseconds. Ordering two strings of that length filled with zero bytes
takes about 480 microseconds, because each zero ends a segment that is
ordered on its own. A phase cannot build a longer string. `table.sort` and
`pairs` charge a comparison of two strings by their lengths, one unit per
byte, so a sort of those zero-filled strings stops inside the default
allowance. A bare `<` of two zero-filled strings does not. Other single
steps of the interpreter are not charged either: a call or `select` with
hundreds of thousands of values passed through `...`, a chain of thousands
of `__index` tables, arithmetic on a long string of spaces (`s + 0`), and a
table that grows and shrinks its parts again and again. `next` on a table
emptied after millions of entries does more work than its 4096 units pay
for, and so does `setmetatable` with such a table as the metatable, for
its 64 units. The real-time limit above is what stops a phase that hides its
work in those steps. A `__gc` entry added to the installed metatable after
`setmetatable` is not caught. `instructions_used` counts Lua instructions in
steps of the check interval, so a short phase can report zero; pattern
steps, string lengths, scans and table work are counted in the units above.
