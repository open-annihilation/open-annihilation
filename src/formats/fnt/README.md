# Font formats and GUI text raster

This component reads the game's FNT 1-bit bitmap format and the GAF-backed
GUI fonts. It retains palette indices and a separate coverage mask so a covered
pixel remains distinguishable from a transparent skip even when both have the
same numeric palette index.

GUI behaviour follows 3.1c. At startup the game loads `anims/hattfont12.gaf`
and `anims/hattfont11.gaf`; each shipped archive has one 256-frame sequence.
Text width is the sum of glyph frame widths, and line height is the height of
frame `0x49` plus two. While loading, the game also subtracts frame `0x49`'s
height from every glyph's signed Y origin; `hattfont12` consequently changes
from raw origin 11 to runtime origin -1. Drawing ignores control bytes,
advances but does not draw byte `0x20`, and draws the remaining bytes as GAF
frames, origin-aware and drawn normally. A font `parse_gaf` reads is marked
`gaf_backed`: its letters sit in the GUI fonts' slots, not an FNT font's, and
the slot of a character it lacks holds the picture of frame 0, its box.

Disk fonts load from `fonts\<name>.FNT`. When the language string is nonempty,
`fonts-<language>\<name>.FNT` is kept if that file opens. The last dotted
suffix is removed before `.FNT` is appended, including a dot in the language
directory. A missing or empty file ends the search, and the game treats it
as fatal: `load_named_fnt` returns a `not_found` error and its callers stop.

The FNT disk layout is a little-endian 16-bit height and a second 16-bit word,
256 absolute 16-bit offsets, then a width byte and a continuous row-major
MSB-first bitmap for each present glyph. The second word, 1, 2 or 3 in the
shipped fonts, is kept as `word_after_height`. Its low byte, read signed, is
the rows the game draws every FNT glyph above the pen row it is given:
`row_lift` returns it and `raster_text` applies it, so the messages, the
clock, the panel labels and the briefings sit on the rows 3.1c puts them on.
A GAF-backed font has no such word and draws from its glyph origins alone.
Parsing rejects out-of-range dimensions, offsets, truncated bitmaps, and
files over the component's named input limit. The parsers and loaders return
`oa::base::bytes::Decoded` values: the font, or the error's code, file
offset and message. Errors of the asset store itself, such as an unreadable
archive, still reach the caller as the store reports them.
