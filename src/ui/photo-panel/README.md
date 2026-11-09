# Photo panel

The panel beside the battlefield in photo mode. It lays out and draws the
chips, the folding sections, the guides and the notice after a photo, from
a model the application fills. It names no file, clock, preference or match.

## Entry points

`layout_of` places the panel and its controls. `draw_panel` draws that
layout, the guides and the toast. `shown_list` adds the kit's focus ring
when the ring is showing. `pointer_event` and `key_event` report a press.
`next_guide` and `guide_name` walk the guides. `crop_centred` crops a photo
to a chosen shape. `covers_chrome` names the bands a photo comparison skips.
The words the panel looks up are `words.hpp`, listed for the catalogue in
`interface_words.hpp`.

The application hosts the panel in `src/app/runtime_photo_panel.cpp`.

## State

The module keeps nothing. `Model` is the whole of what a layout and a
drawing read: the window, which sections are open, the stops and the locks,
the chips, the guide and the toast. Strings on the model are already looked
up. A guide that crops is applied to the photo's rows by the host, not
drawn into them.

## Touch layout

`Model::touch` selects it. `Model::touch_row` is a finger's row, at least 44
points. One section is open. Leave and Take photo stay on screen, and the
rows between them scroll: `Model::scroll` is how far they have moved, and
`Layout::scroll` is that distance after the layout clamps it. Those rows are
one scroll area, `Layout::rows`. Every row stays in the list. A row the view
cuts is drawn and hit in the part that shows, clipped to `rows.view`.
`Model::reveal` names the focused row, and the layout scrolls that row into
the view. Every section's rows come from one list the keyboard layout shares,
so a new row reaches the touch layout too. A strip's parts are each at least
a row wide, so the look's six names wrap sooner than they do for a mouse. A
slider's knob sits at its row's middle. A caption bar raises the key line
over it, as on the keyboard layout, and the line then ends before the panel.

`pointer_event`'s `finger_reach` is how far a finger may land from a control
and still press it. A point on the part of a cut row that shows hits that
row, so the reach does not take Leave or Take photo across that part.

## Invariants

- Every control's visible part lies inside the panel, and the panel inside
  the window, except the hint and the saved-photo notice. A scrolling row may
  extend past the view; the part that shows is the row clipped to the view.
  While the panel is hidden, the hint takes its place and lies inside the
  window.
- A folded section's controls are not in the list. Tab follows the sections
  that are open. Clock, Size and File are words in their sections, not stops
  on that walk. Run, Slow and Freeze are one strip, and the mode that is on
  is lit. A section header says whether it is expanded.
- A locked control stays reachable. Its reason is on the next line, and the
  full reason is the control's text. The zoom's track stays wide enough to
  use; the tier's limit is that next line, only at the far stop.
- Guides are drawn only by `draw_panel`. A photo does not call it.
- The focus ring is the kit's ring, and `shown_list` adds it only once the
  kit would draw it.

## Tests

`ui-photo-panel-layout` checks the fit, the Tab order and the locks at
640×480, 1280×720 and 1920×1080, in each size class and both looks, and
measures every drawn label, value and caption in German, French, Spanish
and Italian at Compact.
`ui-photo-panel-draw` and `ui-photo-panel-draw-data` pin the pictures in
`tests/frames.txt`. `ui-photo-panel-crop` checks a centred crop.
`ui-photo-panel-input` checks that the ring waits for Tab, that the D-pad
walks every stop of a touch layout, and that a tap on a cut row hits that
row. The application check is `native-photo-panel`.

## Limitations

Size offers Screen only. There is no Render command.
