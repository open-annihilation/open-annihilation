# Pad controls

The gamepad controls' model (`oa/ui/pad_controls.hpp`, `oa::ui::pad_controls`):
what the pad in use is and has, the Controller section's settings as the pad
reads them, the layers and the maps from buttons to actions, the pieces that
turn pad input into pointer motion and button events over time, the feels the
pad plays and the button glyphs prompts draw. It knows nothing of SDL, the
screen or the match, and has no clock of its own: every call carries its time.
The gamepad dispatcher in the app turns SDL's gamepad events into calls here
and the answers into the paths the mouse, the keys and the touch layer already
use; the touch HUD draws the prompts from the chords and glyphs it gives.

## Entry points

- **The pad.** `pad_type_of(vendor, product, reported)` names a Steam Deck by
  its USB numbers (which Steam Input's virtual pad reports too), else keeps
  the reported type. `effective_scheme(chosen, traits)` gives the scheme that
  plays: Trackpads only with two trackpads or on a Deck behind Steam Input
  (whose right pad is Steam's mouse), Sticks otherwise.
- **Layers and maps.** `layer_for(held)` picks the layer from what is held or
  open. `binding_for(ctx, layer, button)` and `tap_binding_for(...)` give what
  a physical button does in a layer of a map (`MapContext`: the scheme, the
  fallback map of pads without grips, two trackpads, left-handed), and
  `mirrored(button)` the left-handed mirror. `chord_for(ctx, action, index)`
  gives the buttons that give an action, for prompts and badges.
- **Timing.** `TriggerButton` (on at 30 %, off below 20 %), `HoldTimer` (tap,
  hold, used, progress), `MenuRepeat` (a step at once, at 400 ms, then every
  120 ms), `DoubleClick` (300 ms, 12 points).
- **Rings.** `WedgeAim` turns an aim into a wedge with a dead zone and 8°
  hysteresis; `wedge_point` gives a wedge's point at mid radius, which the
  touch HUD's own hit tests pick unchanged.
- **Pointers.** `PadPointer` is the right trackpad as a mouse (relative with
  acceleration, landing dead band, click lock and glide; or absolute over an
  area through `pad_to_area`). `GyroPointer` is the gyro's fine aim, gated by
  the caller. `StickCursor` is the sticks scheme's pointer (radial dead zones,
  curve, ramp, friction and magnetism against target points the app gives).
  `FlickDetector` reads stick flicks. Every pointer counts its travel into
  ticks with `TickCounter`.
- **Feels and glyphs.** `rumble_for` and `trackpad_pulse_for` give a feel's
  rumble (the Deck plays the low motor on its left pad, the high motor on its
  right) and trackpad pulse at a Haptics strength. `prompts_shown`,
  `glyph_style_for` and `glyph_spec` choose and describe the project-made
  glyphs of the Steam Deck, Xbox, PlayStation and Nintendo sets.

## The maps

Buttons are named by place (`a` is the bottom face button). With grips, the
base layer is the pad pointer scheme: R2 and A and the right trackpad's press
are the left button, L2 the right; R4 QUEUE, L4 ADD, R5 the standing orders
layer and FORCE, L5 the groups layer; R1 the order ring, L1 the build ring; B
clear, X stop, Y the type under the pointer; L3 centre, R3 follow; the D-pad
commander, next unit, next report and SELECT ▾ (Sticks: zoom in, next report,
zoom out, SELECT ▾); View unit info on a tap and the game layer while held;
Menu the game menu; the left trackpad's press the minimap under the thumb.
Without grips (the fallback map): a tap of R1 or L1 toggles QUEUE or ADD and
holding them opens the rings, View held is the groups layer and Menu held the
game layer (a tap of Menu is the game menu). The groups layer puts groups 1 to
8 on the D-pad and the face buttons clockwise from the top, and the ring of
nine on the left trackpad; the game, standing orders, ring, SELECT ▾ and menu
layers have their own tables. Left-handed swaps left and right for the
trackpads, sticks, stick clicks and touches, triggers, bumpers and grips.

While photo mode is open the photo layer wins, before the menus and before
every match layer. The sticks still frame the shot (the app reads them); the
buttons are:

| Button | Action |
| --- | --- |
| A | Take the photo |
| B | Leave photo mode |
| X | Freeze, or hold the picture on a shared match |
| Y | Hide or show the panel |
| D-pad | Move the focus |

No button clicks and no orders. The grips mean nothing in this layer.

## Invariants

- Pure: no SDL, no app header, no clock, no allocation; every type holds its
  state in fixed members and touches no canonical state.
- Times never run backwards within a piece; a call earlier than the last
  counts as no time passing.
- The grips keep their meaning in every layer of the match, so QUEUE and ADD
  held still apply while a ring, SELECT ▾ or a held layer is up; they mean
  nothing in the menus, in the photo layer or in the fallback map.
- `chord_for` names only buttons the pad has, physically (a left-handed map
  names the mirrored button), preferring the main button of a role (R2 for a
  click).
- A step longer than `longest_pointer_step_ms` counts as that long, so a
  stalled frame never throws the pointer; `MenuRepeat` gives at most
  `menu_repeat_most_steps` at once.
- Every number with a meaning is a named constant in the header.

## Tests

`ui-pad-controls` (`tests/pad_controls_test.cpp`) runs timelines with
explicit times: acceleration Off, Low and High at a quarter, one and three pad
widths a second; Pointer speed 50, 100 and 300 %; the landing dead band; the
click lock; glide's start, decay, stop, whole travel and a landing ending it;
ticks every 32 px; the absolute mapping and its aspect fit; the gyro's gain,
gate and longest step; trigger hysteresis; holds (tap, hold, used, cancel,
progress); menu repeat at 0, 400, 520 and 640 ms; double clicks; wedge aim for
12, 8 and 9 slots with the dead zones and hysteresis, and `wedge_point`
checked against the touch radial's hit test; the stick cursor's curve, ramp,
friction and magnetism (a lone target eased onto in 80 ms, a rival within 8
points, a block, Magnetism off); flicks; every row of every layer's table in
the trackpads, sticks and fallback maps and in the left-handed mirror,
including the photo layer and that it wins while photo mode is open; the
chords of every badge; the effective scheme; the glyph sets and every glyph;
the feels at each strength.

## Limitations

The pointer's gains, the stick cursor's curve and the feels' strengths are
the design's starting values; they need tuning on a Steam Deck. The gyro maps
yaw and pitch only; roll moves nothing.
