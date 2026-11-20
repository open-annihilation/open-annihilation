# Total Annihilation GUI layout loader

This portable C++20 component is the text `.GUI` layout loader of Total
Annihilation 3.1c. It parses layout records and the loader's defaults only;
rendering, event dispatch and frontend flow live elsewhere.

## Loader behaviour

- A GUI opens with space for 200 records of `0x15B` bytes; the definition
  loader fills them and the panel is attached.
- The loader iterates top-level records, dispatches their low-byte type, and
  finally replaces the root `totalgadgets` with the loaded record count minus
  one.
- `COMMON` fields are loaded with their exact 8/16/32-bit reductions, then the
  panel, button, list-box, text-box, scroll-bar, label, and hot-surface fields
  by type.
- Text optionally passes through the translation table and otherwise
  stays unchanged. `TranslationLookup` exposes this boundary without creating
  a separate translation system.
- Gadget rectangles are inclusive (`right = left + width - 1`,
  `bottom = top + height - 1`). A zero type byte keeps that record's stored
  `xpos`/`ypos`. Any other type adds the root record's stored origin. Unlike
  the panel-relative rectangle, a type-zero record is not moved to `(0,0)`. A
  nonzero type at index 0 adds its own origin twice. An index outside the
  stored records is rejected rather than read past the span.
- `skin_tiles` places a nine-frame skin (three rows of left, middle and right
  frames, such as the common GUI art's BackTile) over a rectangle: tiles run
  from the top-left, the column reaching the right edge moves left to end
  flush with it, and the row that would run past the bottom moves up to end
  flush with it. A rectangle a whole number of tiles high has no bottom row.
  It gives frame numbers and positions only; the drawing lives elsewhere.
- `button_caption_pen` places a button's caption by the first of its
  alignment attributes: from the left edge, to the right edge, centred, or
  centred along the bottom, its line ending 4 rows above the bottom edge, as
  the game's build pages write their buttons' queued counts. A pressed
  button's caption moves down a pixel, and right unless right-aligned; a
  button with none of the four shows no caption.

The loader reads `help`, then immediately clears the complete destination
buffer before translation. The public model retains the bounded source value as
`source_help`, then passes the empty destination through the translation and
reports that translated runtime value separately. This odd behavior is
preserved rather than repaired silently.

## Types, widths, and bounds

Types are panel `0`, button `1`, list box `2`, text box `3`, scroll bar `4`,
label `5`, hot surface `6`, font resource `7`, file resource `8`, and the
type-10 `nuttin` value. The loader reads only `filename` for types `7` and
`8`: a font resource names an FNT font in the panel's font directory, and a
file resource names a file in its GUI directory. Type `9` has no loader branch
and nothing in the engine handles it (`unhandled`). Type `12` is an image
record, which occurs in shipped layouts and which the gadget engine draws; the
loader gives it no branch either. Both keep the common fields only.

Names and panel references retain at most 15 bytes, text/help 127 bytes, links
15 bytes, and filenames 31 bytes, matching the game's record fields. Text-box
`maxchars` is clamped only above 128; negative values are preserved. Quick keys
keep an alphabetic first byte or parse a signed decimal value into the low byte.
The loader accepts the missing semicolons, line comments, and the omitted final
top-level brace in shipped `SCORE.GUI`. Stray text with no `=` on its line,
such as a digit some mods' build pages leave after a closing brace, runs on
into the next line's field as in 3.1c: a name broken across lines is no field
the loader reads, so that field is left unread and the rest of the file loads.
Stray text with no field after it in its section is rejected. Input is bounded
to 4 MiB, nesting to eight levels, total sections to 600, fields to 128 per
section, source values to 4,096 bytes, and records to the 200 a 3.1c panel
holds.

A number is read as 3.1c reads one: leading spaces, one sign (`+` or `-`)
and the digits after it. Text after the digits is ignored, a value with no
digits, such as a letter O some mods write for a zero, reads 0, and a value
past 32 bits wraps before the field keeps its low bits.

The parser requires the first record to be a panel and every record to contain
`COMMON`; quick keys recognise ASCII letters only, and translated strings
longer than 127 bytes are rejected. A file that breaks one of these rules is
rejected with an error. 3.1c is more lenient, so some files it loads are
rejected here: it loads a file whose first record is not a panel, reports
nothing for a record without `COMMON`, and takes the accented letters of the
system code page as quick keys. It also accepts translated strings longer than
127 bytes.

An installed 3.1c game holds 188 GUI files. 187 are the text layout form
handled here; the shipped `ENDGAME.GUI` is compiled data and
is explicitly rejected as `binary_format` until that separate format is
supported.

```sh
cmake -S src/ui/gui-layout -B local/build-gui-layout
cmake --build local/build-gui-layout
ctest --test-dir local/build-gui-layout --output-on-failure

cmake --build local/build-gui-layout --target oa-gui-layout-inspect
local/build-gui-layout/oa-gui-layout-inspect path/to/guis/mainmenu.gui
```
