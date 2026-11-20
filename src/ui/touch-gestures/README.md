# Touch gestures

The touch controls' gesture recogniser (`oa/ui/touch_gestures.hpp`,
`oa::ui::touch_gestures`). It turns finger reports into the gestures the
touch controls act on: a press, a tap or double tap, a hold and its release,
a one-finger drag (a selection box or a scroll), a two-finger pan, a pinch
and a two-finger tap, and a cancel. It knows nothing of SDL, the screen or the
match, and has no clock of its own: every report carries its time, and
`Recogniser::advance` moves time on between reports so a hold fires while the
finger rests.

## Entry points

- `Recogniser::finger(phase, sample)` feeds one finger report (canvas
  pixels, nanoseconds) and returns the gestures it completed, at most
  `max_gestures_per_step` of them, in order.
- `Recogniser::advance(now_ns)` returns a hold that came due. The app calls
  it every frame.
- `Recogniser::set_thresholds` takes the distances (in points, converted
  with `Thresholds::px_per_point`), the hold delay the Touch settings choose
  and what a one-finger drag does. New thresholds take effect for the next
  finger that lands.
- `Recogniser::reset` drops every finger without a gesture, for a screen
  change.
- `fingers_down` and `dragging` say what the recogniser is tracking.

## Rules

- One finger: `press` at landing; moves within the slop give `rest_moved`
  (the hover point); a lift within the slop before `hold_ms` gives `tap`
  (2 taps when it lands within `double_tap_ms` of the previous tap's lift
  and lifts within `double_tap_points` of it, else 1; a third quick tap
  counts 1 again); still for `hold_ms` gives `hold_started` once; then a
  move past the slop gives `drag_began` with role box and `from_hold`, or a
  lift gives `hold_released`. A move past the slop before the hold gives
  `drag_began` with the role `one_finger_drag` names; a drag ends with
  `drag_ended`.
- `drag_began`, `pan_began` and `pinch_began` are each followed in the same
  batch by a `drag_moved`, `pan_moved` or `pinch_moved` that carries the
  travel (or the spread's change) since the press, so a scroll, a pan or a
  zoom keeps the map under the fingers from the first event.
- A second finger before a drag or hold starts two-finger mode, with no tap
  for the first; during a drag or after a hold it gives `cancelled` first.
  In two-finger mode the centroid passing the slop starts a pan and the
  spread changing by `pinch_points` starts a pinch, together if both
  happen (the pan first in the batch); both fingers lifted within
  `two_finger_tap_ms` of the second landing, with neither begun, give
  `two_finger_tap`. Lifting one of two ends the pan (`pan_ended` with the
  centroid's velocity over the last 100 ms) and the pinch; the other finger
  gives nothing more until it lifts, or until a finger lands again, which
  resumes two-finger mode without a two-finger tap.
- The pan is read at every report. The spread is read once both fingers
  have reported since it was last read, or one has reported twice (the
  other is still): fingers report one after the other, and one finger's
  report alone changes the spread even when both move together, so a fast
  pan never reads as a pinch.
- Twist is off unless `twist_degrees` is above 0. It uses the same
  both-fingers gate as the pinch: one finger's report alone is not a turn.
  Once both have reported, a change in their angle of at least
  `twist_degrees` gives `twist_began`, then `twist_moved` (its `turn` is
  the degrees since the previous twist event). Lifting one finger while a
  twist runs gives `twist_ended`. A turn below the threshold, a slide or a
  spread with twist off, gives none.
- A cancelled finger gives `cancelled` and drops every finger. A third
  finger is ignored until one of the two lifts; reports of fingers that are
  not tracked are ignored.
- Time never runs backwards within one sequence of fingers; a lift after the
  hold delay with no `advance` in between still gives `hold_started` before
  `hold_released`.

## State

The recogniser holds its fingers, timers and a ring of recent centroids in
fixed members; it allocates nothing and touches no canonical state.

## Tests

`ui-touch-gestures` (`tests/touch_gestures_test.cpp`) runs timelines of
finger reports with explicit times and checks the gestures each gives: taps
and double taps (time, distance, the third tap), the slop in both drag roles
and at 3 pixels a point, the hold at 250, 350 and 700 ms and the drag after
it, two-finger taps, pans with their velocity, pinches whose scales multiply
to the spread's ratio, a pan and a pinch together, a fast pan that is no
pinch, a twist past `twist_degrees` (and none from a slide, a spread, or
while twist is off), a second finger during a drag, a third finger, cancels
and reset.

## Limitations

Fingers are tracked two at a time; three-finger gestures do not exist. The
recogniser reads only what one claimed set of fingers does: the app decides
which fingers reach it (only fingers that land on the battlefield do).
