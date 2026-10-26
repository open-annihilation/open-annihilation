# Total Annihilation GAF sprite format

This portable C++20 component reads the pointer-based GAF animation structure
used by Total Annihilation 3.1c. It preserves sequence and frame metadata,
decodes raw and row-compressed palette indices with an explicit write coverage
mask, and composites normal recursive layers using the game's hotspot
relationship, keeping every layer that reaches past the frame.

## Behaviour

- The sequence count is the low 16 bits of the header word at offset 4, read
  as signed; a count of zero or less loads no sequences. Frame counts are
  uint16, frame-list items are eight bytes apart, and a frame's layer count is
  its byte at offset 10.
- A frame's duration is the low 16 bits of the second word of its frame-list
  item (sequence offset 44 + frame × 8).
- Normal drawing draws simple raw/compressed frames, subtracts signed origins
  from the point the frame is drawn at, and recursively visits layers. It
  selects special rendering for a child whose own byte at offset 11 is
  nonzero.
- A composite frame is drawn onto a canvas that covers the frame's rectangle
  and every layer's. The rendered origin moves with any rows or columns added
  above or to the left, so drawing the result at the same point puts every
  layer where drawing that layer at the point would. A layer that fits inside
  the frame leaves the rendered size, origin and pixels unchanged. The canvas
  keeps the frame limits: each side fits the width and height fields, and the
  pixels stay within one frame's pixel limit.
- Raw frames skip the transparency key; compressed frames are drawn by row
  commands. A copy onto the rendered frame is clipped to that canvas. Callers
  draw a rendered frame with its own width, height and origin.

`parse` keeps every simple frame's decoded pixels and coverage by default.
With `PixelData::checked` it checks them as decoding would, so a file
parses or fails exactly as it would, and keeps nothing: the archive holds each
frame's size, origin, duration and layers, and where each simple frame's
pixel data lies in the file (`pixel_data_offset`, `pixel_data_bytes`).
`parse_sequence` then decodes one sequence of the file, by its place in the
sequence table, with its pixels, for a reader that draws a few of a large
file's sequences, as the map's features do. `render_ranged` renders one
frame, with its layers, reading each simple frame's pixel data through the
caller's `ReadHooks` and decoding it as `parse` does, so that it gives what
`render_normal` gives for the frame decoded; a reader draws a file too large
to decode whole that way, a frame at a time. `decoded_bytes` tells what a
frame or a file's pixels and coverage take decoded.

`render_normal` models normal drawing. Special rendering and its blend table
are not implemented, so a special child is reported as
`unsupported_special_render` before drawing it. A root frame's own special
byte does not change how it is drawn; the game draws a top-level frame
normally or specially by context, so this API renders it normally.

## On-disk model

Frame byte `+10` is a uint8 layer count. Byte `+11` is a separate
special-render flag. Some published format descriptions treat those two bytes
as one uint16 layer count; a frame with `{+10=1,+11=1}` is one special layer
in the game, but 257 layers in that model. They also expose the full uint32
frame-list duration, while the game uses the low 16 bits. This implementation
follows the game in both cases.

The header's third word, the sequence header's second word and the frame's
word at offset 12 are zero in every shipped GAF; they are kept as `reserved`
fields and not interpreted. The frame's last word, tentatively named
`aux_plane_slot`, is not an offset and varies between shipped frames; it is
kept as read. Version is likewise retained: the game does not reject other
version values.

The reader caps input at 256 MiB, sequences at 4,096, frames per sequence at
4,096, all recursively reached frame records at 131,072, depth at 32, a single
decoded frame at 4096 by 4096 pixels, and all decoded buffers a parse keeps at
256 MiB; a checked parse keeps none, so its frames may decode past that. It checks
all offsets, row lengths, run widths, pointer cycles and model sizes, and a
frame's size, raw pixels or row lengths before it allocates the frame's
buffers. A caller may also name the widest and tallest frame it takes, as the
GUI font reader does for its 128-pixel glyphs.

The parsed and rendered coverage masks are needed to draw as the game does: raw
frames skip pixels equal to the frame's transparency key, while compressed
frames write every literal/repeated value and treat skip commands and
zero-length rows as transparent. A literal palette index equal to the
metadata transparency index therefore still overwrites a lower layer.
`RenderedFrame::coverage` preserves those final overwrite decisions so a later
consumer does not incorrectly apply the transparency key a second time.

```sh
cmake -S src/formats/gaf -B local/build-sprite-format
cmake --build local/build-sprite-format
ctest --test-dir local/build-sprite-format --output-on-failure

cmake --build local/build-sprite-format --target oa-sprite-format-inspect
local/build-sprite-format/oa-sprite-format-inspect path/to/anims/*.gaf
```

The inspector prints tab-separated file, sequence count, recursive record count,
simple-frame count, special-flag record count, top-level frames that require
special rendering, top-level frame count, and an FNV-1a checksum of normal render
metadata/pixels. An independent comparison must account for the byte
`+10`/`+11` difference above rather than treating disagreement on a special
frame as corruption.

The `anims` directory of a 3.1c install holds 415 GAF files, 4,532 sequences,
22,889 top-level frames and 67,794 recursively reached frame records (53,680
simple and 14,114 composite). Every file parses, and none uses a nonzero
special-render byte.
