# Frontend renderer

This component turns the game's `FrontendX` PCX, `guipal` palette,
`MAINMENU.GUI` layout, and `MAINMENU.GAF` sprites into a portable RGB surface.
`load_main_menu` uses `oa::AssetStore`, so loose resources and mounted archives
keep the game's lookup order. An add-on archive may carry its own
`MAINMENU.GUI`, and no `FrontendX`, laid out for a main-menu overlay drawn
over the menu; it shadows the layout that goes with `FrontendX`.
`MainMenuLayout::with_overlay` takes the top copy, as 3.1c does.
`MainMenuLayout::base_game`, which the application takes whenever no overlay
stands over the menu, takes a loose copy first; else the first archived copy,
in lookup order, whose archive also holds `FrontendX` (`totala1.hpi`'s, whose
four buttons at x 139 and 409 sit inside FrontendX's pipe frames) or lies in
a mod's folder layered over the game folder; else the top copy. It passes
over a copy in any other archive, which 3.1c shows when it comes first. A mod
that brings its own `FrontendX` and no `MAINMENU.GUI`, its art drawn around
the game's buttons, so keeps those buttons in its frames, and a mod's own
`MAINMENU.GUI` applies in whichever of its archives it lies.

`load_screen` accepts explicit layout, PCX, palette, and GAF resource names for
the other frontend screens. The explicit names matter: the renderer does not
infer that every screen follows the main-menu convention. `load_screen_art`
loads the same names but the layout, and `load_screen_layout` lays a layout
out over such art, reading only its GUI file: the two give what `load_screen`
gives. `read_screen_layout` and `lay_out_screen` are the two halves of
`load_screen_layout`: the first reads and parses the GUI file, the second lays
it out over art it takes, moved rather than copied when the caller moves it
in, and `screen_background_path` names the file `load_screen_art` reads a
background from. The match's side column loads its art once a match and hands
it from page to page, each laid out over it, so that a page a click opens
reads one small file and the art is held once; `screen_art_test.cpp`
(`frontend-screen-art`) checks the installed game's Arm pages and in-game menu
so, copied and handed on.

`render_screen` draws a loaded screen into a new surface; `render_screen_into`
draws it the same way into a surface the caller keeps, whose memory is used
again, as the match's side panel and the menus are drawn on every frame.

`FrontendX` remains the active display palette. As in the game's screen setup,
`guipal` indices used by GUI primitives are mapped into that active PCX palette
with the nearest-color rule. Normal GAF pixels use the active PCX palette
directly.

The main menu is set up as in 3.1c. Type-1 buttons are filled and given
raised or sunken borders as the game draws its buttons.
GAF-backed gadgets use the normal sprite path and its explicit coverage mask.
In the shipped main menu, `Credits` resolves to the five-frame `Credits`
sequence in `MAINMENU.GAF`; the four named menu buttons fall back to the shared
`COMMONGUI.GAF` `BUTTONS0` sequence, selecting the closest frame among the
four-state group starts (0, 4, 8, 12, ...) and its following depressed partner.

Button captions use the `hattfont12.GAF` glyph path. Width, line height,
centering, pressed offset, space handling, and origin-aware GAF glyph drawing
are those of the game's captions.

Type-2 text lists: content begins two pixels inside the gadget, zero
`itemheight` selects font line height plus one, rows are clipped to the
authored rectangle, and the selected row uses level `0x1E` from the game's
`PALETTE.LHT` table. Type-5 labels keep the alignment bits, clipping, and the
multiline threshold and spacing.

A button's presentation may carry its quick key and the panel's keyboard
focus, as the match's panels do. A centred caption then underlines the key's
first appearance on the row under the text, in the GUI palette's entry 2
(entry 0 while the button is pressed, none while it is grayed), and the
focused record is ringed with the focus marker's six outlines, a pixel apart,
lit through `PALETTE.LHT` levels 31, 28, 24, 19, 13 and 6 and clipped to the
panel's root (`focus_rings`, shared with the gadget engine's own draw). Each
ring pixel is lit as its palette entry, the nearest one for a colour off the
palette. The frontend screens pass both, the focus only while the screen has
the keyboard.

Runtime image pointers are explicit presentation bindings. Dynamic `SIDEx`
buttons and `ally icons` bind to the screen GAF, while player colors bind to
`32xlogos` in the globally loaded `textures/LOGOS.GAF`; these are not inferred
from widget dimensions or stage counts.

## Scroll bars

`oa/ui/frontend_renderer/scroll_bars.hpp` draws scroll bars on the RGB
surface as 3.1c draws them from the SLIDERS art: the track's start, middle
and end frames, the knob 3 pixels past its position (a vertical knob as its
start, middle and end frames, at most the bar's height less 6 long and
ending at least 4 pixels above the bar's end; a horizontal knob as its start
frame, at least its width and 2 pixels before the bar's last column), and
each arrow's frame, or its held face while the pointer holds it. A grayed bar
and its arrows are grayed through the palette's gray table and shaded at the
grayed level over their rectangles, as a grayed button with art is
(`grayed_paint`, `gray_and_shade`).

`LayoutScrolls` binds a loaded layout's bars and readies its lists as a
panel's first draw does (`bind_layout_scrolls`), writing the bound rectangle,
positions, knob size and knob back into the gadgets, routes the pointer's
presses, moves, releases and holds to them, keeps each list's first row in
step with its bar, and draws them over the screen (`draw_layout_scrolls`).
`bind_screen_buttons` binds a panel's buttons as the first draw does, for a
panel merged into a loaded one. `scroll_bars_test.cpp` (`frontend-scroll-bars`)
pins the art, the drawing and a layout's bars and lists.

## Blending

`blend_rect` blends a colour over a rectangle of an RGB surface: each
channel of each pixel becomes the opacity's share of the colour, in 256ths,
and the rest of the pixel, rounded to the nearest; the rectangle is clipped
to the surface. An opacity of 0 leaves the pixels and `blend_opaque` paints
the colour, so it fills as well as tints. The match's "+stats" panel darkens
the battlefield under it this way. `frontend-renderer` tests it.

## Drawing without the game's art

`oa/ui/frontend_renderer/artless.hpp` draws the Open Annihilation settings
dialog and its OA button on the RGB surface in flat colours. Everything is
given in source pixels (the 640x480 screen) and drawn through a `Placement`,
which puts source pixel (0, 0) at a surface pixel and draws each source pixel
as a square block of `scale` surface pixels; a scale below 1 draws nothing.
Every primitive clips to the surface, and to the placement's `clip`, a
rectangle of source pixels, when it is not empty; the settings dialog clips
the rows of a section that scrolls this way. A surface whose pixels do not
fill its size is left as it is.

- `fill_source_rect` and `blend_source_rect` fill or blend a rectangle as
  `blend_rect` does; a fill one pixel high or wide is a hairline, and a
  blend darkens what lies under the dialog.
- `draw_bevel` draws a one-pixel raised edge inside a rectangle: light along
  the top and left, dark along the whole bottom and right, so the top right
  and bottom left corners are dark. `draw_outline` draws a one-pixel ring.
- `draw_text` draws text with a game font's glyphs, placed as
  `oa::formats::fnt::raster_text` places them, in one colour, and returns
  the pen column after it; `text_width` measures it. Given a plain `Font`,
  every glyph pixel is drawn in the colour, which suits a one-bit FNT font.
  Given a `TextFont` from `text_font`, each glyph pixel is blended by its
  palette colour's brightness: the colour that rings most glyph edges, and
  anything darker, draws nothing; the brightest colour draws the text
  colour; the colours between draw in proportion. The game's shaded GUI
  fonts (`hattfont12`, `hattfont11`) so keep their shading and lose their
  dark outline, which on a flat panel would thicken every letter into a
  block. Their glyphs at 0xD7 and 0xB7 are empty boxes.
- `draw_mark` draws a one-bit picture. `oa_mark_thin` (9x5, strokes one
  pixel wide) and `oa_mark_bold` (13x7, uprights two pixels wide) hold the
  letters "OA" of the OA mark.
- `draw_picture` draws an RGBA picture (`RgbaPicture`), such as the Open
  Annihilation icon, scaled to fill a rectangle at the surface's own
  resolution, not in source pixel blocks: each surface pixel shows the
  alpha-weighted mean of the picture's pixels its share covers, laid over
  the surface by their mean alpha, so a large picture shrinks smoothly and
  its clear parts leave the surface as it is.

`artless_test.cpp` (`frontend-artless-draw`) checks every primitive pixel by
pixel at several placements and scales, a picture's means, its blend and
its surface-resolution columns included, clipping to the surface and to a
placement's clip included, against synthetic fonts; with the installed game
(`frontend-artless-draw-data`), it checks that text in `hattfont12` and
`hattfont11` covers exactly what `raster_text` covers and blends each pixel
at its ink.

## Game text

[game_text.hpp](include/oa/ui/frontend_renderer/game_text.hpp) draws game
text as the player's Language settings choose
([oa/present/game_text.hpp](../../present/include/oa/present/game_text.hpp)):

- `gui_font_characters` and `fnt_font_characters` give the characters a GUI
  or FNT font draws, with the byte of each glyph (a GUI font's glyph that is
  the picture of glyph 0, its box, is none); `gui_font_baseline` and
  `fnt_font_baseline` the rows from the pen down to the font's baseline;
  `gui_font_face` and `fnt_font_face` the modern face that stands for it.
- `split_game_text` reads the bytes as the settings say and splits them into
  the font's runs and the modern fonts' runs: game text the settings draw in
  the modern fonts is one modern run, at the settings' Text size; a
  character the font lacks is drawn at the font's own size. `needs_text_runs`
  tells whether a text needs splitting at all: text of ASCII that keeps the
  font is drawn as before.
- The screens' gadgets are laid out for the game's fonts, so the drawing
  here holds modern text to their size (`screen_text_size`) and keeps it on
  the font's baseline; a smaller Text size draws it smaller.
- `draw_gadget_text` takes a `game_text` flag: the match's own text, what
  players type and send and their names are game text, the labels of menus
  and dialogs are not, and keep the font for every character it has. The
  loading screen's labels, and a network load's names and status, are drawn
  as labels, so the loading screen keeps the game's fonts.
  `draw_gadget_glyphs` draws a font's bytes as they are.
- `draw_fnt_game_text` and `measure_fnt_game_text` do the same with an FNT
  font on the RGB screens. Both take the most size the modern runs are
  drawn at: a button's caption or a one-line label wider than its gadget,
  as a wordier language's may be, draws its modern text smaller, a step of
  5% at a time down to 50% (`fitted_fnt_text_size`,
  `present::fitted_text_size`), before a caption still too wide, or one the
  game font draws whole, loses characters from its end as before. Every
  caption that fits, all of 3.1c's English among them, draws as it did. The
  8-bit panels (`gadget-render`) fit their buttons' captions and one-line
  labels the same way.

`frontend-gadget-draw` (`test_gadget_text_game_runs`) checks a missing
character drawn in the modern fonts on the font's baseline, the text after
it, a width limit, game text drawn whole and a label kept in the font, and
game text at the Text size, held to the game fonts' size, with a missing
character kept at the font's.
