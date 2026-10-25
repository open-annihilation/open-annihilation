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
- `Dispatcher` turns one event into action events. `set_contexts` says which
  contexts are open for the next `feed`. A plain chord gives `pressed` on
  the down and `released` on that key's or button's up. `:double` gives
  `double_press`, `:release` gives `pressed` on the up, and `:hold` gives
  `held` from `advance` once it has been down for `long_press_ms`. A repeat
  action gives `repeated`; any other action ignores a repeat. A modifier
  role gives `active` and `inactive` as its own key goes down and up.
  `release_all` releases every held action and role. `role_active` reads
  the modifiers and the keys the caller reports now, not the events `feed`
  saw. The caller passes the time.

## State

A chord, a map and an event are values the caller keeps. The position
table and the named keys are fixed. A `Dispatcher` keeps the bindings, the
keyboard map, the open contexts, the keys and buttons that are down, the
actions each down started, the hold timers and the modifier roles that are
active.

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
  player's own changes are left as they were given. An action keeps at most
  four chords. Escape stays on `chat.cancel` whatever a step said. Every
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
back to the same layout.

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

`ui-key-bindings-sdl` (`tests/sdl_numbers_test.cpp`) links SDL and checks the
numbers the library holds: every scancode SDL names is in the position table
or in `scancodes_without_code`, every table entry equals SDL's scancode for
that key, and every named key's key code equals SDL's.

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
