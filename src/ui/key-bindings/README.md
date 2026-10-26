# Key bindings

Chords (`oa/ui/key_bindings/chord.hpp`, `oa::ui::key_bindings`). A chord says
whether a binding means the letter on a key or the key's place, so Attack
stays on A and the groups stay on the number row whatever the keyboard. The
module reads a chord, writes it back as one text, names its parts in English
for keycaps, and says how well it matches one event on a keyboard map.

It is the model the dispatcher, the reader, the layouts and the editor share.
It knows nothing of a window or a clock: an event carries its own time. The
dispatcher times a double press and a hold from that time.

## Entry points

- `parse_chord` reads a chord of at most 64 bytes. Modifiers may arrive in
  any order, each once. A lower-case letter is read as its capital. `@` and
  a code names that key's place; `@` and a named key (`@F1`) is that named
  key. A modifier alone (`Shift`) is a trigger with no gesture. A wheel turn
  takes no gesture. `chord_error_text` is the English for a refusal.
- `chord_text` writes the one canonical text: modifiers as Primary, Ctrl,
  Alt, Shift, then Meta; letters in capitals; a named key by its name; `@`
  for a place; a gesture after `:`.
- `match` compares one chord with one event. A character matches the event's
  key code (`A` is the code of `a`). A place matches the position number. A
  named key matches its key code, except NumpadEnter, which matches its
  position, so a keypad Enter whose key code was turned into Return still
  matches Enter and NumpadEnter matches only the keypad key. Ctrl is held
  exactly when the chord names it. Shift, Alt or Meta held beyond the chord
  still matches, as `with_extra_modifiers`. The gesture is not judged.
- `rank` orders the chords that match one event: twice the modifiers named,
  plus one when the chord matches by position.
- `held` says whether a modifier-alone chord is held. Primary is the command
  key where `Platform::primary_is_meta` is set, and Ctrl elsewhere.
- `keycaps` and `display_text` are what a screen shows. `display_text` joins
  the keycaps with `+` and puts the gesture after a space.
- `on_keyboard` is false for a character no key's events carry.
- `KeyboardMap::us` is the US layout. `set` records, for one position, the
  key code events carry and the character the key prints.
- `resolve` turns the base, the chosen layout, the mod's quick keys and the
  player's own changes into the bindings in use. Each chord records the step
  that set it. A name neither the registry nor a family makes is kept, with
  its chords. A chord that loses records why, and which action kept it.
- `find_clashes` lists pairs that share a key on one layer. A hack's
  precedence, an overlay and a stricter set of conditions are not clashes;
  `shadows` lists those pairs for the editor. Two modifier roles on one
  modifier, select and command on the left button, and the resource panel's
  view and lock on one double-click are not clashes either: the screen
  decides which, and both chords stay. `clashes_with` answers what one more
  chord on an action would clash with.
- `layers_meet`, `conditions_meet` and `same_key` are that rule in pieces.
  `same_key` treats a letter and a place as one key when the keyboard map
  prints that letter on that place.
- `layout_from_manifest` parses each chord of a manifest. A chord that does
  not parse, and a modifier on its own on an action whose kind is not a
  modifier, are errors at that chord's own line and column. An action this
  build does not know, a binding that leaves Escape out of a fixed action
  or gives Escape to another action in a context that meets it, a second
  binding of one chord, and a character the keyboard does not type, are
  warnings. The layout is left unchanged when any chord is an error. A
  warning keeps the binding.
- `manifest_from_layout` writes the layout back into a manifest. Each chord
  uses its canonical text. An action this build does not know is written
  back, and the other keys come from the manifest that was passed in.
- `key_positions` names every position a keyboard report can carry that the
  UI Events code list names. `scancodes_without_code` lists the rest, which
  no chord can name.
- `legacy_key_chord` maps one key the options dialog stores, of the
  sixteen it offers, to a chord. Alt, Ctrl and Shift are that modifier on
  its own. A letter, Tab, `/` or `\` follows the action's `by`: a place is
  the position the caller passes, and a letter is the character. Any other
  key code maps to nothing. The caller passes the position; this module
  does not ask which key it is.
- `imported_list` is an action's default chords with that stored key in
  place of each trigger. Extra modifiers and the gesture stay. A default
  that is a modifier on its own loses it before the key is applied, and a
  stored modifier replaces the whole chord.
- `Dispatcher` turns one event into action events. `set_contexts` says which
  contexts are open for the next `feed`. A plain chord gives `pressed` on
  the down and `released` on that key's or button's up. `:double` gives
  `double_press`, `:release` gives `pressed` on the up, and `:hold` gives
  `held` from `advance` once it has been down for `long_press_ms`. A repeat
  action gives `repeated`; any other action ignores a repeat. A modifier
  role gives `active` and `inactive` as its own key goes down and up.
  `release_all` releases every held action and role. `role_active` reads
  the modifiers and the keys the caller reports now, not the events `feed`
  saw. The caller passes the time. `set_timings` holds each time to its
  range. Slow keys hold a key down back until `advance` has seen it held
  for `slow_keys_ms`; it fires once then, at most a frame after it was due,
  and a release before that drops it. Bounce keys drop a second press of
  the same key within `bounce_keys_ms` of its last accepted release, and
  that press's release. Mouse buttons, the wheel and keys typed into a
  field are never held back. With a repeat delay and a rate both set, the
  platform's repeats are ignored and a `repeat` action fires from `advance`.
  A latch or a sticky modifier applies only to an event the bindings accept,
  including a key `advance` admits late.
- `Latches` keeps a latchable action named in `set_toggle`. A tap turns it
  on and a second tap turns it off. A press held for `long_press_ms`, or
  used while it is held, is a plain hold and leaves the latch as it was.
  `StickyModifiers`, on while `set_sticky_modifiers` is set, takes a
  modifier tapped with no other key or button between: the first tap arms
  it for the next chord or click, a second tap within `double_press_ms`
  locks it, and a tap while it is locked lets it go. `consume_sticky` lets
  an armed modifier go and keeps a locked one. `release_all` clears the
  latches and the sticky modifiers, which is what losing the window does.
  `shown` is the order they were turned on. A repeat does not toggle a latch.
- `PointerKeys` moves the pointer from the numpad while pointer keys are on.
  Eight directions add up, and a diagonal travels at the same speed along
  its line. The speed starts at `pointer_keys_start_speed` (60 points of the
  640×480 game a second) and rises in a straight line to
  `pointer_keys_top_speed` (480) after `pointer_keys_ramp_ms` (1,000 ms).
  5 clicks the left button and, a second time within the double-press time,
  double-clicks. 0 clicks the right button. + holds the left button until it
  is pressed again. . lets a held button go. `clear` does that too, and
  drops every direction; the button comes up on the next `step`. The caller
  passes the time and sends the movement as mouse events.
- `pointer_keys_bindings` sets a chord aside while pointer keys are on, when
  that chord belongs to the base, the chosen layout or the mod and a
  pointer-key default takes the same key. Faster then keeps `=` and loses
  NumpadAdd; a page key loses its plain numpad digit and keeps the digit
  row. The player's own chord stays. A group chord keeps its Ctrl or Alt, so
  it is not the same key. While pointer keys are off the bindings are
  unchanged. The layout file is not rewritten.
- `WheelButton` tells a click, a drag and a long press apart. The caller
  passes the time and the drag threshold. A distance at or under the
  threshold is a click at the press's point. Past the threshold, the press
  is a drag and the motion that crossed is kept. Held still for the
  long-press time, the wheel opens. After a decision the press point stays
  readable until `cancel` or the next press. `cancel` drops the press. The
  class reads no clock.

## State

A chord, a map and an event are values the caller keeps. The position
table and the named keys are fixed. A `Dispatcher` keeps the bindings, the
keyboard map, the open contexts, the keys and buttons that are down, the
actions each down started, the hold timers, the modifier roles that are
active, the latches and the sticky modifiers.

## Invariants

- The library does not include or link SDL, and it does not read a clock.
  `feed` and `advance` take the time the caller counted.
- `chord_text` of a parsed chord is the canonical text, and parsing that
  text gives the same chord. Every registry default is already canonical.
- A letter matches a key code and a place matches a position number. Shift
  or Alt held beyond a chord does not turn a match into none. Ctrl held
  beyond a chord does not match.
- A keycap is English words, or a character, and never both in one string.
  An F-key is one label (`F1`), not a word. A digit in a name (`Numpad` and
  `1`, `Button` and `6`) is its own keycap. `display_text` puts `+` between
  keycaps and the gesture after a space.
- Resolution reads its input and the registry, and does not read a clock.
  The number-key swap is applied to the result only: the layout and the
  player's own changes are left as they were given. It exchanges
  `group.select-n` with `build.page-n` and flips Alt on `group.add-n`. A
  chord that flip lands on is dropped, lost to the swap. A page that kept an
  Alt chord also keeps it with Shift, so Alt+Shift+1 stays that page. An
  action keeps at
  most four chords. Escape stays on `chat.cancel` whatever a step said. Every
  other action, `ui.settings` included, takes the chords the steps give it.
- `layout_from_manifest` reports a bad chord at the chord, not at the list.
  It keeps an unknown action and its chords. It does not change the layout
  when a chord is an error.

## Tests

`ui-key-bindings-chords` (`tests/chord_test.cpp`) round-trips every registry
default, including each left-click default and each hack default, and checks
each trigger form, each modifier order, lower-case letters and `@F1`. It
checks every refusal and the byte it was found at. On the US map it matches
`A` by key code, `@Digit1` by position, Enter against a keypad press whose
key code was made Return, and NumpadEnter only against that key's position.
Primary is the command key on one platform and Ctrl on the other. On an
AZERTY map, `A` matches the key whose events carry `a`, `@KeyA` matches the
A key's place, and `@Digit1` is labelled `&`. It checks keycaps for each
form, and `on_keyboard` for a backtick the map does not carry.

`ui-key-bindings-files` (`tests/layout_file_test.cpp`) reads a manifest into
a layout. A chord `Ctrl+Q:sometimes` is an error at the chord's column. An
unknown action is a warning and is written back. `chat.cancel` without
Escape, and `chat.send` on Escape, are warnings; `ui.settings` on F9 is
not. A repeated chord, and a backtick the keyboard does not type, are
warnings. Every registry action's defaults round-trip through the paste
back to the same layout. It also maps each of the sixteen option keys, and
a key code outside them, and checks that a stored Q keeps `build.rotate`'s
Shift chord and replaces Alt on `build.snap-override`.

`ui-key-bindings-layouts` (`tests/layout_test.cpp`) resolves `oa-default`
with no clash for no hack, for each hack alone and for all of them, in both
pointer schemes and with the number-key swap off and on. It checks a chosen
layout, a mod letter, a player's own change, a hack's default and its
replacement, the left-click scheme, the swap, fixed Escape, and the pairs
that share a key without clashing: modifiers, the left button, a stricter
condition, chat's Enter, a hack's Tab, and a pointer aimed at a different
place.

`ui-key-bindings-dispatcher` (`tests/dispatcher_test.cpp`) resolves
`oa-default` and walks synthetic timelines: Attack on A, including with
Shift or Alt; Ctrl+M matches nothing; Tab across the megamap, the next
primary and the previous primary; Enter in chat and in play; `=` for speed
and, with the debug keys, refill; text entry keeping only Escape; Cmd+/
reaching the keys overlay in global; a repeating pan and a press that does
not repeat; the status strip's press and release around a Shift; a
double-click selecting the type; a hold that fires once after 500 ms; a
key-up never seen down; `release_all`; and the queue role following Shift
whether or not a Shift event was fed.

`ui-key-bindings-latches` (`tests/latches_test.cpp`) walks the same kind of
timeline for the hold helpers: a tap latches and a second tap lets go; a
hold past the long-press time stays off; a hold used by a click does not
latch; an action left out of `toggle`, and one that is not latchable, never
latches; a sticky tap arms the next chord and then lets go; two taps within
the double-press time lock until a third; a tap with another key between,
and a modifier held during a click, stay plain; `release_all` empties both;
and `shown` keeps the order turned on.

`ui-key-bindings-pointer-keys` (`tests/pointer_keys_test.cpp`) walks a
synthetic clock: the speed at 0, 500 and 1,500 ms, a diagonal of two keys,
a click and a double click inside and outside the double-press time, Hold
left and Let go, and `clear` releasing a held button. It resolves
`oa-default` and checks that pointer keys on set NumpadAdd aside from
Faster while `=` stays, that a player's own NumpadAdd stays, and that
pointer keys off changes nothing.

`ui-key-bindings-confirm` (`tests/confirm_test.cpp`) checks the hold rule
and `HoldConfirm`. With the setting off, nothing needs a hold. With it on
and no list, a confirmable action needs one and an action that is not
confirmable does not. A list that names the action needs the hold. A list
that leaves it out does not. The hold is `done` at the long-press time and
not before. A release at 90% reports `none`. Progress at the quarters is
0, 250, 500, 750 and 1000 thousandths.

`ui-key-bindings-timing` (`tests/timing_test.cpp`) walks synthetic
timelines: a slow-key tap of 100 ms fires nothing and a hold fires once at
300 ms; a bounce at 200 ms is dropped and one at 400 ms is kept, including
when the dropped press's release sat in between, while another key is kept;
two presses 550 ms apart match `:double` at 600 ms and two 650 ms apart do
not; a `:hold` fires at 800 ms and not at 700; a repeating key held 1,000 ms
at a 200 ms delay and 10 a second fires nine `repeated` events and the
platform's repeats do not add one; a press action never repeats; with the
system's repeat the platform's repeats pass through; a mouse button and the
wheel are not held back; typed text is not held back; `drag_click_ticks(32)`
is 0x19 and `drag_click_ticks(96)` is 75; `clamped` holds every limit and
keeps a zero that means the system's own time.

`ui-key-bindings-sdl` (`tests/sdl_numbers_test.cpp`) links SDL and checks the
numbers the library holds: every scancode SDL names is in the position table
or in `scancodes_without_code`, every table entry equals SDL's scancode for
that key, and every named key's key code equals SDL's.

## Hold to confirm

`needs_hold` is the rule. Hold to confirm off never asks for a hold. On, a
confirmable action needs the long-press time when the resolved layout names
no `hold-to-confirm` list, or names that action in the list. A list that is
present and empty names nothing. `order.self-destruct` is confirmable in the
registry. `gui.yesorno.choice1`, the Yes of the surrender question, is
confirmable by this rule: a made `gui.` action has no registry row.

`HoldConfirm` keeps one press. `start` names the action, the time and a
target: the pointer's point, or a button's rectangle, in match canvas pixels.
`tick` reports `waiting` until the long-press time and `done` once it has
passed, while the press is still held. `release` before that, or `cancel`,
makes the next tick report `none`. `progress` is how far the ring has filled,
in thousandths. The app fires the action when `tick` reports `done` and then
cancels the hold, so the ring goes and a key that is still down does not
fire again.

## Limitations

A position SDL names and the UI Events code list does not (a soft key, Call,
and the others `scancodes_without_code` lists) cannot be bound. A few codes
on that list have no position in a keyboard report and are not in the table:
Fn, FnLock, Hyper, Super, Turbo, Abort, Resume, Suspend, Hiragana, Katakana,
LaunchApp1, LaunchApp2, LaunchMail, Unidentified, NumpadStar, Open and Props.
The words on the keycaps are English; drawing them, and translating them, is
left to the screens that show them. Matching one chord does not time a
double press, a hold or a release. The dispatcher does, from the time the
caller passes. A pad chord matches no event.
