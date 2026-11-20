# GUI input boundary

This component holds the gadget engine's input side: the per-screen GUI
context and its panel stack, the pointer and keyboard state machines that
drive gadget records, and a few small menu helpers.

- `button_result` checks the selected `0x15B` gadget record and performs a
  case-sensitive `strcmp` against its name. A null layout or selected index
  `-1` returns false.
- `gadget_geometry` reads signed `x`, `y`, `width`, and `height` fields and
  returns inclusive edges (`right=x+width-1`, `bottom=y+height-1`). A zero
  type byte uses origin `(0,0)`, as in 3.1c.
- `clear_selection` writes `-1` to the selected record field.
- `mark_label_shadows` reads the root panel's signed loaded gadget count
  and, for records 1 through that count, ORs attribs bit `0x8` onto
  type-5 labels. The root is not visited. That bit is the label offset-text
  flag the label drawing tests. No coordinates are written. A missing panel
  or a non-positive count is a no-op, and the walk stops at the last stored
  record.

The menu pointer traversal starts at record 1, skips the root panel, checks
only nonzero active records, and retains the last matching rectangle. With a
preexisting selection it processes at most the first record and then stops;
an inactive record stops immediately in that case. Coordinates are expected
after the caller's root-origin subtraction. Those rules are represented by
`hit_test(MenuObject, x, y)`; no generic z-order policy is added.

`physical_key` and `navigation_direction` keep the game's key mapping:
internal control codes `0xF4..0xF7` map to the left/up/right/down virtual
keys `0x25..0x28`; space and modifier mappings are also retained.
`key_is_down` counts a key-state word as down when any bit other than the low
toggle bit is set. These map key states only. The arrows still move by
place, which is `focus_nearest`'s job. Tab follows `tab_order`.

## Scroll bars and the lists they scroll

`oa/ui/gui_input/scroll_bar.hpp` keeps a type-4 gadget as a `ScrollBar`
and a text list as a `ScrollList`, one record each, for screens that keep
their gadgets in a model of their own (the frontend renderer's
`LayoutScrolls`, for one) rather than in the byte records above. The behaviour is the gadget engine's:

- `bind_scroll_bar` binds a bar as its panel's first draw does: the panel's
  own SLIDERS art, else the shared art of COMMONGUI.GAF from frame 10 for a
  bar wider than high and frame 0 for any other, whose first frame then sets
  the bar's thickness. The back and forward arrows are the size of their
  frames, at the bar's start and end, and the bar shrinks to lie between
  them; a horizontal bar's knob is as wide as frame 5 and its positions are
  its width less the knob and 4. Without art a bar has no arrows and its
  longer side less 6 positions.
- A press on the knob drags it pixel for pixel, once an update; a press
  beside it holds the bar, which steps the knob one position toward the
  pointer each tick and once more on release, so a click steps it once. An
  arrow steps at once and, held, repeats once a tick after 15 ticks; its
  bar's handler runs on every step, even at the bar's end. A hidden, grayed
  or unbound bar takes no pointer.
- `scroll_set_value` and `scroll_value` convert between the knob and a value
  up to the bar's maximum, rounding the knob up and the value down.
- A text list is filled (`scroll_list_fill`) with its pitch raised to the
  line height + 1, and its bar shows exactly when the rows overflow it, with
  a knob of rows / count of its length less 3, at least 10 pixels. The list's
  first row follows its bar's knob (`scroll_list_follow_bar`), and the knob
  its first row (`scroll_bar_follow_list`); a screen's pick
  (`scroll_list_select`), the Up and Down keys (`scroll_list_step`), a press
  (`scroll_list_press`) and the wheel (`scroll_list_scroll`) move it.
- `scroll_page_span` is how many positions Page Up and Page Down move a
  bar: one position when the bar has fewer than two, otherwise a tenth of
  the travel from its start to its end, and at least one.

## Tab order

`oa/ui/gui_input/tab_order.hpp` is the one Tab order and the one key rule
for the built-in screens and for a `GadgetPanel`. The screens keep their
own focus; they call the same functions a panel's `dispatch_key` calls.

- `tab_order` returns the gadgets that can take the keyboard and have
  area, as indices. A screen it knows is ordered as that screen is read:
  single player's columns, a new campaign, load game, skirmish, map
  selection, the options pages, a briefing and the campaign's end. Every
  other screen is read in columns. Left edges within 16 share a column,
  columns run left to right, and each column runs top to bottom. A
  horizontal run the columns would split, whose centres sit within 8 of
  the run's first centre and whose next control starts within 40 of the
  one before it, is read left to right as one row. When a file
  already lists its stops in that order, the result is the file's order.
  The main menu does not ask for this order. Its Tab still walks by place.
- `focus_key_action` says what the focused gadget does with a key. Tab
  always moves along the order. A horizontal scroll bar steps with Left
  and Right, pages with Page Up and Page Down by `scroll_page_span`, and
  jumps to its ends with Home and End. A list steps with Up and Down. A
  colour cycles with Left and Right while the focus ring is showing. A
  text box's caret keys, and every other key, stay with the caller, which
  keeps what that key does today.

`scroll_bar_test.cpp` (`gui-scroll-bar`) pins the binding, the pointer and
the lists. `gui_input_test.cpp` pins the orders over the installed GUI
files, and `gadget_panel_test.cpp` pins Tab against a place move and a
slider stepped from the keyboard.

Build independently with:

```sh
cmake -S src/ui/gui-input -B local/build-gui-input
cmake --build local/build-gui-input
ctest --test-dir local/build-gui-input --output-on-failure
```

The vectors cover root exclusion, active-record traversal and overlap behavior,
exact geometry boundaries, case-sensitive selected-name activation, selection
clearing, and the key mappings and predicates.
