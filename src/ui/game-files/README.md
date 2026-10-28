# Game files screen

The Game files screen's model (`oa/ui/game_files.hpp`, `oa::ui::game_files`):
what the screen that brings the player's Total Annihilation files into the
game's own storage shows, where it goes and what a press or a key on it
means, with no SDL and no app header. The app (`src/app/game_files_screen.cpp`)
maps the import's state onto a `Model`, lays it out with `lay_out` into a
`Layout` (the UI kit's display list of parts in canvas pixels, in painting
order, with their controls and the focus order) and paints the parts by
role; presses and keys come back through `press_down`, `press_up`, `key`
and `scroll` as a `Command` for the app to carry out. See
[docs/game-files.md](../../../docs/game-files.md) for the screen as players
see it.

## Entry points

- `device_class` gives the form from the canvas's size in points: a phone
  when the shorter side is under `touch_hud::phone_short_side_points`, else
  a tablet, so the phone form can be checked on a desktop window of a
  phone's size. A tablet window whose shorter side is under 600 points (a
  640x480 window) keeps the tablet's form with the phone's compact sizes.
- `lay_out(model, viewport, measure)` lays out the model's step (first run,
  looking, the nested offer, already there, Ready to copy, copying,
  checking, Ready to play, a problem, or the management state) with its
  banner and sheet, inside the viewport's safe area, every control at least
  `min_button_points` high. `TextMeasureHooks` (the kit's) measures text
  with the bundled fonts the app opens; the kit wraps and shortens every line
  with that measure.
- `control_id` and `screen_control` turn a `Control` (its kind and index)
  into the number its part and its kit control carry, and back.
- `hit_test` finds the control under a point with the kit's `reach` over the
  layout's controls: the containing one, the top one first, else the nearest
  enabled one within `pick_reach_points` (22 pt), which may be a fraction of
  a pixel at a fractional density. Parts painted before a backdrop (under a
  sheet) have no control, and the clipped-off parts of scrolled rows take no
  presses.
- `press_down`, `press_up`, `key` and `scroll` change the sheets, switches,
  focus and scroll themselves and return what the app must do (`Outcome`).
  Stop, Copy with a replacement, Show, Why, Remove, Remove old folder,
  Remove all, Add files… and Done with a change waiting open their sheets;
  a sheet's button closes it and returns its command. The app keeps the
  kit's `Interaction` (the focused and held controls by their numbers, and
  whether a key has shown the focus). Tab and Shift+Tab move the focus in the
  list's declared Tab order (`kit::next_in_tab_order`, the same as
  `Layout::focus_order`) and scroll a row into view; a focus not shown yet
  shows where it is first. Up and Down scroll the rows, as the page keys do.
  Return and Space press (Return with no focus shown presses the main
  button); Esc closes a sheet, else goes back (cancels the listing, opens
  the Stop sheet while copying, acts as Done in the management state).
- The app marks the focused and held parts with `kit::mark_states` before
  painting; `lay_out` leaves both clear.
- `platform_word`, `size_text`, `time_left_text`, `ready_text` and
  `summary_text` build the texts. Every player-facing string goes through
  `oa::data::languages::interface_text` whole, with its `{name}` places
  filled after the lookup, so a translation sees the whole sentence. The
  platform's own words come from `Model::words` (the app fills them from
  `GameFilesHooks::text`); without them the engine's neutral words are used:
  "device", "Copy your Total Annihilation folder into Open Annihilation's own
  folder with your file manager.", "Into Open Annihilation's own folder",
  "on this device or anywhere the system's file picker reaches", "anywhere
  the file picker reaches", "Free up space on this device", "In your file
  manager: Open Annihilation's own folder › Total Annihilation" and "the
  cloud".

## Built from the kit

The layout is made of the UI kit's parts (`oa/ui/kit/layout.hpp`), each a
`kit::Item` in canvas pixels with the screen's roles (`header_bar`, `badge`,
`header_text`, `version`, `title`, `text`, `panel` for a card or the rows'
panel, `icon`, `banner`, `row`, `divider`, `toggle` for an OFF/ON switch,
`button`, `button_main`, `button_danger`, `progress_track`,
`progress_fill`, `progress_busy`, `backdrop` and `sheet`), its lines, pixel
size and weight, its `TextStyle`, its colour, its `Glyph` and the fraction
of a progress fill. The kit's text measures, fits (`kit::fit`) and wraps every
line; its `text_part`, `line_part`, `button_part`, `button_row`, `icon_part`
and `plain_part` lay out the generic parts at the form's scale
(`kit::Typesetter`, `canvas_pixels`); its `scroll_column` places the column
and says what scrolls; `list_part_controls` lists the controls of the parts
above the last backdrop, the top one first, and the Tab order; and its
`reach`, `next_in_tab_order` and `mark_states` hit test, move the focus and
mark the parts.

What stays the screen's: its metrics (`tablet_metrics` and `phone_metrics`)
and its forms rule (a phone, or a window whose shorter side is under 600
points, takes the compact sizes; these are not the kit's Compact, Regular
and Large size classes); its compositions, built from the kit's parts: the
header, banners, the first run's cards and option rows, the part rows and
their panel, the panels of the listing, the copy and the check, a problem
and the sheets; the model, the content and the texts; and its own look: the
screen's colour tokens (`kit::screen_colour`), rounded cards and buttons and
its glyphs, which the app's painter draws for these roles.

## Layout

A step is the header bar (the OA badge, "OPEN ANNIHILATION  GAME FILES",
the version, and OA · Aa except in the management state), then a column of
three blocks: a fixed top (title, banner), a body (the cards, the parts'
rows, a problem) and a fixed bottom (totals, warnings, buttons), then on
the first run a footer at the foot of the safe area. When the column does
not fit, the body scrolls in `Layout::rows` between the fixed blocks; when
that would leave the body under 96 points, the whole column scrolls. A sheet
is laid out the same way in its panel over a backdrop. Sizes are in points
(the tablet's after the 1194x834 mock-ups, the phone's after the 852x393
ones), converted with `px_per_point`.

Painting follows the parts by role: text roles draw their lines from the
top of the box, left-aligned, one line height apiece (the measure's line
height); buttons and the switches' halves centre their labels; a button's
glyph is a square of the label's pixel size, or of the part's `glyph_size`
when the button sets one, `button_glyph_gap_em` of the label's size left of
the label. A row's size is a text box exactly as wide as its text,
so right-aligned texts need no other rule.

The OA mark is the app icon, the plates alone. The badge is a box with no
text that the app fills with that icon, and OA · Aa is a button whose glyph
is `Glyph::oa` (the icon, 20 points on the tablet and 18 on the phone)
beside "Aa". The app draws the icon at the screen's own density, filtered
down, and without it the OA mark the OA button shows at rest: green "OA"
letters in a green outlined square. The Settings dialog's header is the
gears, not this icon.

## Sizes

`size_text` writes decimal units as the system shows them (1 GB is
1,000,000,000 bytes): one decimal under 10 and whole numbers above ("1.1
GB", "38 GB", "742 MB"), or with `precise` three significant digits ("1.12
GB", "11.3 GB"); trailing zeros are dropped ("3 MB").

## Tests

`ui-game-files` (`tests/game_files_test.cpp`) lays out every step, banner,
problem and sheet at 1194×834 points, at 852×393 points with a 59/21 pt safe
area (at 3 and 2 pixels a point) and at 640×480, scrolled to the top and to
the bottom, and checks control heights, the safe area, text inside its box
as the bundled fonts measure it, overlaps and the focus order; then hit
tests, every press's command, the problems' buttons, the keys, scrolling,
the texts with neutral and with scripted platform words, and the size and
time tables. It checks that the kit's controls are the live parts that draw
one, the top one first, that no two parts draw one control, and that the Tab
order is the focus order. Every layout of every case at every review size
must keep the SHA-256 that `tests/layout-digests.txt` records for it: the
digest of `format_layout`'s text, every field of every part in a fixed order
with roles, text styles, glyphs and kinds of control by name and colours by
the name of the screen's colour token whose value they are (so a token's
value may change and the digests hold). The digests were recorded from the
screen's own layout code before it moved onto the kit, and prove the move
kept every part. `oa-ui-game-files-test --write-digests` prints that file's
content, and `--dump <case>` prints the parts of the cases whose names hold
the text.

## Limitations

- Only English is shown until translations ship; the catalogue may hold
  the screen's sentences once they do.
- The layout shortens a line it cannot fit with an ellipsis; very long
  locations are cut to two lines.
