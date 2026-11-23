# zstd

One zstd frame, for a replay's recording, into and out of buffers the caller
owns. Target `oa-base-zstd`, header `oa/base/zstd/zstd.hpp`, namespace
`oa::base::zstd`. The bytes are zstd 1.5.7, kept in
[`third_party/zstd/`](../../../third_party/zstd/) and compiled unchanged, with
assembly, threads and the legacy frame formats left off. Each vendored `.c`
file is compiled through a unit in this module.

The base tier holds no heap container, so this module never allocates the
bytes it returns. The caller asks for a size, makes room for it, and passes
the room in; `formats/oademo` is the caller that allocates, and only after
the size check.

## Entry points

- `compress_bound` is the most bytes a frame of an input of that size can
  take. It is zero for a size zstd does not take.
- `compress(input, level, out)` writes one frame at a level from
  `lowest_level` to `highest_level` into `out` and returns its size, or
  `bad_level`, `buffer_too_small`, `out_of_memory` or `damaged`. The frame's
  header names the size of the input.
- `compress_parts(first, second, level, out)` writes those two spans as one
  frame, without joining them first. The header names the sum of their
  sizes, and `decompress` reads the frame back as their concatenation. A sum
  that does not fit in `size_t` is `damaged`. The window cap is the same as
  `compress`.
- `compress_watched(input, level, out, watch)` writes one frame the way
  `compress` does, in chunks of 256 KB. `watch` is told how much of the
  input has been taken, and may stop the frame before it ends. A stop
  leaves no frame and returns `stopped`. The bytes need not match
  `compress` of the same input. The window cap is the same.
- `frame_writer_begin`, `frame_writer_write`, `frame_writer_end` and
  `frame_writer_free` write one frame from input that arrives in pieces,
  handing the frame's bytes to a sink as they are made. The input's size is
  given at the start and the header names it; the window cap is the same as
  `compress`. The bytes do not depend on how the input is cut, so two
  writers given the same input at one level write the same frame, but they
  need not be the bytes `compress` writes for that input in one piece:
  zstd splits and matches a stream it is given in pieces differently from
  one it holds whole. A writer holds zstd's work area and its room.
- `frame_reader_begin`, `frame_reader_read` and `frame_reader_free` read one
  frame whose bytes arrive in pieces, into room the caller gives each time.
  It is one data frame and nothing else, as `decompress` takes: a skippable
  frame and any byte after the frame's end are `damaged`. Its window is at
  most `most_read_window_log` (27 bits, 128 MiB), zstd's own bound for a
  frame read in pieces.
- `content_size(frame, most_bytes)` reads the size a frame names from its
  header alone and refuses a size past `most_bytes` with `too_large`, before
  anything exists to hold it. A header that names no size gives
  `size_unknown`, and one that cannot be read gives `damaged`.
- `decompress(frame, out)` reads one frame into `out` and returns the bytes
  written. A frame that names a size larger than `out` is refused before it
  is decoded (`buffer_too_small`). A frame that names no size is decoded
  until it ends or `out` is full.
- `lowest_level` and `highest_level` are the levels zstd accepts.
- `most_window_log` is 23: the most bits of window a frame written here
  declares.

## State

None for the calls on whole buffers: each reads its arguments, writes into
the buffer it is given, and makes and frees its own zstd context. A
`FrameWriter` or `FrameReader` holds its zstd context from its begin to its
free, and the caller owns the struct and the room it gave.

## Invariants

- A frame whose header names a size above the limit is refused by
  `content_size`, from the header. A size equal to the limit is accepted.
- The same input and level compress to the same frame.
- `compress` pledges the input size, so a frame it writes names that size.
- **Every frame written here declares a window of at most 8 MiB**
  (`most_window_log`, 23 bits), whatever the level. Levels up to 19 stay
  inside it on their own. Levels 20 to 22 would ask for 64 or 128 MiB on an
  input past 8 MiB, and are held to 8 MiB through `ZSTD_c_windowLog`, with the
  size still pledged. A reader that refuses a window above 8 MiB therefore
  reads every frame this module writes at any level. A level inside the cap
  is left as zstd sets it, so its frames do not move.
- The input of `decompress` must be exactly one frame: a second frame, a
  byte after the frame, and a frame cut short are all `damaged`.
- Nothing is fetched while the module builds. The assembly file under
  `third_party/zstd/decompress/` is copied and not compiled.

## Tests

`base-zstd` checks a round trip at levels 1, 3 and 19 (with the size the
header names, and the same frame twice), an empty input, junk bytes, a real
frame cut at three places and with a byte added, two frames stuck together,
a skippable frame,
a level zstd does not accept, two spans compressed as one frame, a watched
frame that reads back and one that is stopped before it ends, a buffer too
small for the result, a header
that names a gigabyte against a limit of 64 bytes, a frame with no size in
its header, and that a level-19 and a level-22 frame of an input over 8 MiB
declare a window of 8 MiB or less (`ZSTD_getFrameHeader`). It also checks
that a frame written in pieces reads back whole and in pieces, names its
size, is the same bytes however its input is cut and keeps the window cap at
level 20 on an input over 8 MiB; that a writer refuses more or less input
than it began with and stops when its sink refuses; and that a reader takes
a frame `compress` wrote a byte at a time and refuses a frame cut short, a
byte after the frame, junk, a skippable frame and a window past its cap.

## Limitations

One frame only: a dictionary, a skippable frame and a second frame are
refused. The level is zstd's own range. A frame that names no size cannot be
sized from its header, so its caller grows a buffer up to its own limit and
decodes again (`formats/oademo` does).
