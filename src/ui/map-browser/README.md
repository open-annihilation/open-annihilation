# Map browser

OA's map browser: one screen that shows every map the player could play,
the base game's and the expansions that are on, the mod's, those of the
map packs installed in the player's Maps folder and those the catalogues
offer, as one grid of cards with their pictures, says whether each fits
the game and the mod being played, and gets one in a click. Library
`oa-ui-map-browser`, headers `include/oa/ui/map_browser/`, namespace
`oa::ui::map_browser`.

The model (`model.hpp`) turns plain cards into what shows: it merges the
four sources, filters and orders them, and says what the primary button
does. It reads no file, clock or network and looks no word up. The screen
(`screen.hpp`) lays the model out at the three size classes, made only of
the UI kit's components ([src/ui/kit](../kit/README.md)), draws it through
the kit, and takes the pointer, a finger, the keys, typed text and the
wheel. Nothing here hosts the screen, fetches, installs or decodes a
picture: the app (`src/app/runtime_map_browser.cpp`) fills the cards from
the runtime's data, hosts the screen on the OA layer, asks its preview cache
([src/ui/map-previews](../map-previews/README.md)) for the pictures the
layout names, and acts on what the screen returns.

## Entry points

- `merge_cards(game, mod, installed, online)`: one card per game name. The
  game's maps come first, then the mod's, then the installed pack maps,
  then those online, and a card's origin is the list it came from. A name
  an earlier list already has is left out, so an online map an installed
  pack already provides shows once, as installed (the Library offers its
  updates and other registries' copies), and two registries' copies of an
  online map show once, the first's. No map is dropped for any other reason.
- `base_map_players(numplayers, start_positions)`: the counts a base map
  keeps. Each whole number from 2 to 10 in its numplayers list, sorted and
  without repeats, and the most of them; when that list names none, the
  start-position count alone.
- `shown_players(card)`: the most players a card shows, the largest of its
  counts.
- `apply_filters(browser)`: the cards that match the source (All, Game for
  the base game and the expansions that are on, Mod for the mod's own
  layers, Downloads for installed and online packs together), the one
  players choice (Any, or 2, 3, 4, or 5–10 when that count is among the
  map's), the one size choice (Any, or S a longer side of at most 10, M 11
  to 20, L above) and the search (the title, the name, the pack's name or
  the source's name holds it, ignoring ASCII case). Choosing the segment
  already chosen leaves it chosen; Any clears it. Every match is ordered
  by title, ignoring ASCII case, then by name, wherever the map comes from.
  A selection that no longer shows is cleared. `source_counts` counts each
  pill without the source filter, so the pills read "All (324)".
- `overlay_name(card, downloading)`: "Downloaded" when an installed pack is
  named for the map (its title, or the stem of its game name before '@').
  The same map offered by a catalogue and not installed shows "Available". While
  that card's pack is downloading it shows "Downloading…". Any other card
  shows its source name, or else the pack's name.
- `primary_action(browser)`: Select for a game, mod or installed map that
  fits; Download and select for an online map that fits or whose fit is not
  yet known; disabled, with its reason, for a map that does not fit;
  disabled "Downloading…" while a download runs.
- `card_word(name)`: a map's game name as a word of an automation name
  (`isle_of_ashes@archipelago` gives `isle-of-ashes-archipelago`).
- `browser_layout(browser, state, frame, fonts, previews)`: the window, the
  size class's dialog size (480×324 at Compact, 720×486 at Regular, 960×600
  at Large), in the sizes Settings gives its window: Kit 2's window header
  (Compact's band at Compact), window padding and window footer. The kit's
  header reads "OPEN ANNIHILATION" then "MAPS" in its second colour, as
  Settings' and the Library's do, with a close mark at the right and
  beside it "Skirmish · playing <mod>", "Battle room · playing <mod>", or
  "Skirmish · No Mod" with no mod. The filter bar is a row of Kit 2's
  control height (one line at Regular and Large when the search keeps 120
  points; else two: the source pills and the search, then PLAYERS and
  SIZE): the source pills are Kit 2's filter chips, PLAYERS and SIZE
  Settings' segmented strips at the control height, each segment a
  button's room round its widest word (a chip's where the line is short),
  and the search, at most 240 points wide, sits at the right. Then a scrolled grid of
  cards in 3, 4 or 6 columns (`kit::card_cells`, each card with
  `kit::add_card`: its picture or "Loading preview", a chip naming where
  the map comes from, its title cut to one line and the most players it is
  for, then "<W>×<H>"; a card that does not fit greyed with a "Doesn't fit"
  chip), and the footer: the selected map's title and details (a game or
  mod map leads with the same name as its chip; a pack leads with its pack,
  version and registry, then "<players> players · <W>×<H>" and, online, the
  download), its fit chips ("Fits No Mod ✓", "Doesn't fit <mod>", "Fit
  with <mod> is checked when it's installed"…) and, for a map that does not
  fit or a download that failed, the reason under them, a progress bar in
  their place while a download runs, and Cancel and the primary button at
  the right, Settings' buttons: the plain and the accent look at the
  control height, their words in the regular font, each at least twice as
  wide as it is tall, two spaces apart. At Compact the buttons sit at the
  footer's top beside the title, and the byline, the chips and the reason
  take the line under them. PLAYERS and SIZE are one segmented strip each,
  Any first. The close mark is the kit's quiet close button.
- `cards_in_view(...)`: the cards whose cells meet the grid's view and the
  row after them, which alone the layout asks pictures for
  (`PreviewHooks::ask`).
- `draw_browser(canvas, list, size)`: the kit's window face, the display
  list and the kit's window edge.
- `browser_pointer`, `browser_key`, `browser_text`, `browser_wheel`,
  `browser_find` (the controller's Y: the search takes the focus) and
  `focused_field`: the input. They return a `ScreenResult`: nothing, Cancel,
  or choose (the primary button on the selected card), and whether to draw
  again.

## Automation names

Every control is named for the automation endpoint, which lists it as `oa.`
and the name, its dialog `oa.map-browser`: `maps-filter-all`,
`maps-filter-game`, `maps-filter-mod`, `maps-filter-downloads`;
`maps-players` with parts `any`, `2`, `3`, `4` and `5-10`; `maps-size` with
parts `any`, `s`, `m` and `l`; `maps-search`; one `maps-card-<word>` per
card that shows, its text the map's game name, its word `card_word` of
that name, with `-2`, `-3`… added in grid order to a word already taken;
`maps-cancel`; `maps-select`, its text "Select", "Download and select" or
"Downloading…"; and `maps-close`. A part is listed as `oa.` and the
control's name, a dot, and the part's word (`oa.maps-players.4`).

## State

The model (`Browser`) holds the cards, the filters, the cards that show,
the selection, the picker that opened it, the name of the mod being played
and a download's name, progress and failure. The screen's state
(`ScreenState`) holds the hover, press and focus, the grid's scroll and the
wheel's carry, the search field's text and caret, a press in the grid
that may become a drag, and a hold on the grid's scroll bar with the row of
the thumb it took. The layout reads both and changes neither; the
input changes both. Pictures belong to the host's preview cache: a card's
picture refers to its pixels, valid until the host decodes again.

## Invariants

- The layout asks for the pictures of the cards in the grid's view and of
  the row after them, and for no other card's.
- Nothing is drawn outside the window, and nothing outside the kit: every
  part is a kit component or a generic part of the kit's display list,
  every colour a kit token.
- Two controls never share a name, and `kit::name_problem` finds nothing.
- The arrows move the focus by where the controls lie (`kit::focus_toward`),
  in the grid and between the bands. Left and Right on a players or size
  strip step its choice and stop at the ends. Tab follows the source pills,
  the players strip, the sizes strip, the search, the grid (entered at the
  selected card, else the first), Cancel, the primary button and the close
  mark. Enter and Space on a strip leave its choice and do not accept the
  dialog. The close mark closes the browser as Cancel does.
- Every card that shows is a control, scrolled out of view or not, clipped
  to the grid's view, so the arrows reach every card and a card a press
  cannot reach is never pressed.
- A press on the grid's scroll bar holds it and presses nothing: on the
  thumb it grabs the thumb at the row pressed, on the well the thumb's
  middle jumps to the pointer, and the thumb follows the pointer until the
  release, the mouse's or a finger's.
- A card that does not fit can be selected and never chosen: its primary
  button is disabled.
- Every player word is looked up in the interface catalogue
  (`oa::data::languages::interface_text`) as it is shown.

## Tests

`ui-map-browser` (`tests/model_test.cpp`, `tests/layout_test.cpp`, in a
synthetic font): merging, also when the pack is installed from another
registry; every filter and their combination; a map for 2 and 4 players
found by chips 2 and 4 and not by 3, its card and details reading 4; the
numplayers list ("2, 4", "4", "2,3, 4", an empty value, "12", junk); the
search; the order; the primary button's states; the cards' words. At each
size class: 3, 4 and 6
columns; nothing outside the window, scrolled to the top and the end,
downloading and failed; the footer's buttons; every automation name, sound
and unique, and two maps whose names give one word told apart by `-2`; the
pictures asked for only for the cards in view and the next row, counted on
fake hooks; the focus moving right, left and down by geometry, and up out of
the grid; Tab in the declared order, into the grid at the selected card;
what presses, taps, the keys, typing and the wheel and a drag do; the
scroll bar's thumb dragged down and above the well, a press at the well's
foot reaching the end as End does, and a finger dragging the thumb.

## Limitations

- A map's size chip reads the first two numbers of its size text; a map
  whose size cannot be read matches no size chip.
- The window is the size class's dialog size whatever the window's room
  beyond it; a larger window shows the same window, centred.
