# Replay viewer

The playback bar a player sees along the foot of the battlefield while a
replay plays (`oa/ui/replay_viewer/replay_viewer.hpp`,
`oa::ui::replay_viewer`). It places the controls, says which one a point
hits, writes the game time, and decides when the bar hides. It draws through
the UI kit. It does not read a recording, a match, or a preference.

The app fills a `Model` for one frame: the tick, the length, whether playback
is paused, the speed, the markers, the heat, and whether the end card is up.
`lay_out` places the bar. `draw` paints it. `hit_control` and `marker_index`
say what a press landed on.

## Entry points

- `time_text` and `clock_text` write game time. One second is 30 ticks.
  `time_text(21000, 42960)` is `11:40 / 23:52`. Minutes have no leading zero.
  Seconds have two digits.
- `lay_out(width, height, model, shown)` places the bar along the foot. From
  the left: Back 10 s, Play/Pause, Forward 10 s, the time, the timeline,
  Speed, the view control, the camera control, Photo, Chat, Keys and Leave.
  640×480, 1280×720 and 1920×1080 each keep a timeline wider than 40 points.
  The time keeps the width the model's `widths.time` says its text needs.
  The speed button grows to its full caption ("16× · 9× now"). Where the row
  has no room for that, it takes the brief form ("16× · 9×") and then the
  asked speed alone (`Layout::speed_form`), so its text is never cut. The
  view and camera slots never give up width.
- `hit_control` returns the control under a point. `marker_index` returns the
  marker whose hit area holds the point, the nearest when two overlap. A hit
  area reaches 8 points from the marker. `tick_at` returns the game tick under
  a column of the timeline.
- `BarVisibility` hides the bar after 3 seconds with the pointer off the
  battlefield's lower edge (the bar and 16 points above it). `show` brings it
  back, which a playback key and a tap do. `toggle_pin` holds it on screen.
  The pin is not stored anywhere: a new `BarVisibility` starts unpinned.
- `fill_controls` lists the controls automation can press. Names start with
  `replay-viewer.`. A hidden bar lists none of its own controls. An open
  view menu lists a row per player, `view-item.N`, whose text is the name
  and, once the player has gone, "Defeated 18:02" or "Left 21:40".
- The view menu draws each player's colour, name and departure. The bar
  lays out no player list: the app draws the players' stats over the
  battlefield's top right.
- `draw` paints the bar in the kit's HUD colours. The watched part of the
  timeline is the lit green, a keyframed stretch is a lighter green, and the
  rest is dark. Commander deaths are diamonds in the player's colour. Pauses
  are gaps. Chat is a short tick. The end is a flag. A pointer on a marker
  draws that marker's line, and a pointer on the timeline draws the time
  there, while the model says the pointer is over the screen. The speed menu,
  the shortcut card and the end card are drawn only while the model asks for
  them. The speed menu, the shortcut card and the hover are drawn over the end
  card, and take a press before it.
- `speed_caption`, `marker_caption`, `catching_up_caption` and
  `damaged_caption` are the lines the bar shows, in the language on show.
  `fitted_speed_caption` is the speed caption in the form the button holds.
  The button's name for a screen reader is always the full caption.

## State

The module reads the `Model` and the `BarVisibility` the caller passes. It
writes nothing of its own. It does not keep a pin, a speed, or whether chat
is shown.

## Invariants

- The bar's rectangle spans the window's width and ends on its bottom edge.
- The row's rectangles sit inside the bar, in the order above, and do not
  overlap.
- The view and camera rectangles have a width, and are controls only while
  the bar is shown.
- A hidden bar still has its rectangles, so the lower edge does not move,
  and `hit_control` returns none for them.
- `tick_at` of the timeline's first column is 0, and of its last column is
  the length, when the length is known.

## Tests

`ui-replay-viewer` (`tests/replay_viewer_test.cpp`) checks the layout at
640×480, 1280×720 and 1920×1080, the marker hit areas, the time text
`11:40 / 23:52`, the hide and pin timing, the view menu's rows with no
player list, and a departure's room beside the name in five languages. It also measures the time and
the speed captions in five languages and requires them to fit on the
battlefield the bar covers in a match at those three window sizes (512, 725
and 832 points wide).

## Limitations

The module does not switch the view or move the camera: the app does, from
the view and camera menus. It does not draw the players' stats. It does
not decide which markers exist:
a recording's timeline is the app's to read. It does not play, pause, seek
or change speed. The app does that when a control or a key asks.
