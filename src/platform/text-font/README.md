# Text font

Game text in the modern fonts, the text the game's own 8-bit fonts
cannot draw, such as Greek, Cyrillic, Chinese, Japanese, Korean and emoji,
and OA's own screens in the modern look, drawn through FreeType from fonts
that travel with the game. Every platform
draws it the same way, pixel for pixel: macOS, Windows (XP included) and
Linux. The application draws game text through it
([runtime_game_text.cpp](../../app/runtime_game_text.cpp)), with the
outline, shadow and background of
[oa/present/game_text.hpp](../../present/include/oa/present/game_text.hpp).

## The fonts

`FontStack::open` opens the base fonts from one folder, `face_files` in
[text_font.hpp](include/oa/platform/text_font.hpp). Every base face is
required: a missing file fails the open. A file that FreeType cannot
read, or that is not scalable, fails it too. Each character comes from
the first open font of the stack's chain that has it:

| Face | File | Draws |
|---|---|---|
| `dejavu_sans_bold` | `DejaVuSans-Bold.ttf` | Latin, Greek, Cyrillic and symbols in the bold weight; skipped in the regular weight |
| `dejavu_sans` | `DejaVuSans.ttf` | the same scripts in the regular weight, and what the bold face lacks |
| `endonyms` | `NotoSansCJKsc-Bold-Endonyms.otf` | the languages' own names and the notice shown before a language pack is installed; required |
| `noto_emoji` | `NotoEmoji.ttf` | emoji, in one colour like any character; its weight axis follows the line's weight |

The interface faces open with the stack from the same folder,
`interface_face_files`, each when its file is there: a missing one leaves
its family to the sans faces. They are the `Face` values after the pack
faces' (`interface_face`), each named by a constant.

| Face | File | Draws |
|---|---|---|
| `barlow_regular`, `barlow_medium`, `barlow_semibold`, `barlow_bold` | `Barlow-Regular.ttf`, `Barlow-Medium.ttf`, `Barlow-SemiBold.ttf`, `Barlow-Bold.ttf` | the interface family's Latin letters, digits and marks at each weight |
| `barlow_condensed_semibold`, `barlow_condensed_bold` | `BarlowCondensed-SemiBold.ttf`, `BarlowCondensed-Bold.ttf` | the condensed interface family: SemiBold for a regular, medium or semibold line, Bold for a bold one |

The full Simplified Chinese face travels with that language's pack, not
with the game. A language pack may add up to `most_pack_faces` (4) faces
while the stack is open (`add_face`). An `ideographs` face is drawn at
the related size and no less than the style's least size. A `letters`
face is drawn at the sans faces' size. Bold looks in DejaVu Sans Bold,
DejaVu Sans, the letters pack faces in the order they were added, the
ideographs pack faces in that order, the endonym face, then Noto Emoji.
Regular is the same without DejaVu Sans Bold. Semibold looks as bold
does and medium as regular. `Style::family` chooses the faces looked in
first: the sans family (`Family::sans`, the default) is the chain above;
the interface family (`Family::interface`) puts the Barlow face of the
line's weight before it, and the condensed one
(`Family::interface_condensed`) Barlow Condensed. Barlow holds Latin
letters and no Greek or Cyrillic, so a line that holds a letter beyond
ASCII its face lacks is drawn whole in the sans family, its digits and
marks with it, and no word changes typeface part way; ideographs and emoji
come from their own faces either way. `FontStack::chain` gives that chain;
`fallback_chain` stays the base faces only.

A character no open font has draws the chain's first open font's
missing-glyph box. Control characters, variation selectors, zero-width
spaces and joiners, and the byte-order mark draw nothing and take no room
(`is_invisible`).

`tools/bootstrap_text_fonts.py` fetches the fonts and FreeType, pinned by
SHA-256, into `local/deps`, and cuts Noto Sans CJK SC Bold down to the
languages' own names and the notice in `tools/text-fonts/endonyms.txt`.
Barlow and Barlow Condensed are the static faces of the project's release
1.422 (<https://github.com/jpt/barlow>), unmodified. The full CJK face
comes with the Simplified Chinese language pack. The build copies the ten
fonts into the `fonts` folder beside the game
(`cmake/OaTextFonts.cmake`), which `bundled_font_directory` finds at run
time: the bundle's `Contents/Resources/fonts` on macOS, the fonts folder
beside the executable elsewhere. Their licences are in the repository's
`licenses/` folder and travel beside the game with the others
([ATTRIBUTIONS.md](../../../ATTRIBUTIONS.md)).

## Entry points

- `FontStack::draw(text, style)` draws one line of UTF-8 as coverage, one byte
  a pixel. `Style` holds the DejaVu faces' pixel size, the weight, the
  rendering and the letter spacing. `Rendering::mono` hints the outlines to
  whole pixels and draws one bit a pixel, coverage 0 or 255, crisp like the
  game's pixel fonts; `Rendering::antialiased` hints them and draws 256
  levels. `Style::family` chooses the faces looked in first. The pen starts at the coverage's `origin` column and moves by each
  character's advance rounded to a whole pixel, plus the letter spacing
  after each character that moves it. Characters are not shaped, kerned or
  reordered, so scripts that need shaping (Arabic, Hebrew, the Indic
  scripts) are drawn letter by letter, left to right.
- The coverage is at least the line's ascent and descent tall
  (`FontStack::metrics`), and grows for a glyph that reaches past them;
  `baseline` gives the baseline's row and `advance` how far the pen moved.
  A line wider than `max_line_width` is cut off there.
- `FontStack::layout` gives each character's font and pen without drawing,
  and `FontStack::face_for` the font a character comes from.
  `FontStack::draws` tells whether every visible character of a text is
  held by a face of the chain, so a caller can choose between a language's
  own name and its English name.
  `fallback_chain` gives the base faces a weight looks in.
  `FontStack::chain` gives the faces one open stack looks in, pack faces
  included, leaving out a face that is not open.
- `add_face` adds a pack face. It returns false, changing nothing, when the
  stack already holds `most_pack_faces`, or the file is missing, unreadable
  or not scalable. On Windows it reads the file whole, up to 64 MiB, and
  closes it, since Windows does not move a folder while a file in it is
  open: a language pack is then updated, rolled back or removed while its
  font is shown. Elsewhere the face reads its file as the base faces do.
  `remove_pack_faces` drops every pack face and every glyph
  drawn from one, so a face added afterwards may reuse an index without
  drawing the glyphs of the face that left. `pack_face_count` and `has_face`
  report what is open. `face_file_opens` tells whether FreeType opens a file
  as a scalable font.
- `FontStack::face_metrics` gives one face's pixel size and rows at the size
  a style draws it at, and is empty when the face is not open. The sans
  faces, the interface faces and letters pack faces take the style's pixel
  size. The endonym
  face and ideographs pack faces take `related_pixel_size` of it, and no
  less than the style's least size. Noto Emoji takes `related_pixel_size`.
  `FontStack::metrics` is the greatest ascent and descent of the open base
  faces and of an interface family's face for the weight. A pack's faces
  never change the line's rows. The endonym face
  keeps the rows a CJK face has, so the line is 14 above the baseline and
  4 below at 14 px bold.
- `related_pixel_size` gives the size the endonym face, ideographs pack
  faces and Noto Emoji are drawn at beside the DejaVu faces: 12 px beside
  14 px, so ideographs stand a row or two taller than DejaVu's capitals,
  as the game's outlined capitals do. `Style::least_cjk_pixel_size` holds
  the endonym face and ideographs pack faces to a least size.
  The line's rows grow
  to hold a base face, not a pack face: the application draws ideographs at
  `least_cjk_language_pixel_size` (12 px) at the least while a Chinese,
  Japanese or Korean language is shown, since smaller ones fill in.
  `message_log_pixel_size` (14), `status_readout_pixel_size` (11) and
  `label_pixel_size` (11) are the sans faces beside the message log, the
  status readouts, and the labels and the chat line.
- `decode_utf8` decodes text, refusing what is not UTF-8.

The sizes that match the game's fonts: DejaVu Sans Bold at 14 px, mono,
beside the message log's `hattfont12` (capitals 10 rows, x-height 8,
2-pixel stems), and DejaVu Sans at 11 px beside `CONSOLE.FNT`, the chat
line's and the labels' font (x-height 6, 1-pixel strokes). At 14 px the
bold sans face has 13 rows above the baseline and 4 below; the line is 14
and 4, which is the endonym face at 12 px. The application
draws them at the player's Text size, from half to three times those
sizes and never under 7 px, mono at every size.

## Invariants

- A stack draws only with its own FreeType library, made without FreeType's
  default properties, which an environment variable could change, so every
  machine draws alike.
- The fonts are read from their files as glyphs are needed, through the
  module's own file stream, so a folder whose path is not in the Windows code
  page opens too. A stack keeps the tables FreeType reads and the glyphs it
  drew, not the fonts' outlines. The Windows 95 build reads each base and
  interface font whole when the stack opens and closes its file, since the
  C library Windows 95 ships keeps only a few files open at once and the
  game's archives and a saved game need them.
- Each glyph is drawn once per font, size, weight and rendering and kept, up
  to `FontStack::kept_glyphs`; past it, the glyphs used longest ago are
  forgotten, and only between lines.
- One thread at a time uses a stack. The drawing is the view's alone: it
  reads no game state and writes none.

## Tests

`platform-text-font` (`tests/text_font_test.cpp`) checks the UTF-8 decoder
and what it refuses (`decodes_only_utf8`), the limits on size, length and
spacing (`refuses_what_it_cannot_draw`), the fallback chain
(`falls_back_through_the_chain`), the sizes that match the game's fonts:
DejaVu Sans Bold at 14 px gives an H of 10 rows and an x of 8, DejaVu Sans at
11 px an x of 6, and ideographs at 12 px stand 11 or 12 rows, and each face's
rows at that size are what `face_metrics` reports, the line taking the
greatest of its chain (`matches_the_game_fonts_sizes`), mono, anti-aliased
and spaced lines and the glyph store (`draws_mono_and_antialiased`), a store
that stays within its bound and forgets the glyph used longest ago
(`keeps_the_glyphs_used_last`),
and ideographs held to a least size while Latin letters keep theirs
(`holds_ideographs_to_a_least_size`), the endonym face matching an
ideographs pack face's rows from 7 px to 48 px, 14 and 4 at 14 px bold
(`endonyms_keep_the_cjk_rows`), and drawing 简体中文
(`endonyms_draw_without_the_cjk_face`), pack faces joining the chain,
matching the endonym face's drawing of an ideograph and refusing a fifth
face or a file that is not a font (`pack_faces_join_the_chain`), a removed
pack face leaving none of its glyphs for the face that reuses its index
(`removed_faces_leave_no_glyphs`), the interface faces: each open, first in
their family's chain at each weight, the sans faces, the endonym face and
the emoji face drawing what Barlow lacks, a Cyrillic word drawn whole in
the sans faces while ideographs leave Latin letters in Barlow, Barlow
narrower than DejaVu Sans and its condensed face narrower again, their
glyphs kept when pack faces leave, and a folder without them opening with
their families in the sans faces (`interface_faces_go_first`), and
`face_file_opens` accepting each bundled file and refusing a text file and
a missing path, and in the Windows 95 build an open stack leaving each of
its font files free for an exclusive open
(`an_open_stack_closes_its_font_files`).
`platform-text-font-pixels`
runs the same program with `--pixels`: "Ab", a Chinese character and an emoji
in bold at 14 px, mono, pixel for pixel as FreeType 2.14.3 draws them; it
skips with another FreeType. `--show TEXT` prints a line as it is drawn.

## Limitations

- No shaping, kerning or bidirectional text, and no colour emoji.
- Barlow holds no Greek or Cyrillic: a line in those alphabets is drawn in
  DejaVu Sans in the interface families too.
- Ideographs other than the languages' own names draw from a pack's face.
  Without that face they draw the missing-glyph box.
- A pack's ideograph face is the Simplified Chinese face, so Japanese and
  Traditional Chinese text takes its forms of the characters the languages
  share.
