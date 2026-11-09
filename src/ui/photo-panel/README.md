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

## Invariants

- Every control lies inside the panel, and the panel inside the window, except
  the hint and the saved-photo notice. While the panel is hidden, the hint
  takes its place and lies inside the window.
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
`ui-photo-panel-input` checks that the ring waits for Tab. The application
check is `native-photo-panel`.

## Limitations

Size offers Screen only. The looks beyond Show selection, Show health bars
and Guides are not on this panel. There is no Render command.
