# Key editor

The key editor's list, keyboard and mouse: every rebindable action, by category, with its keys, where it works and whether the player changed it, and the player's keyboard and mouse with what each key and button does. Library `oa-ui-key-editor`, headers `include/oa/ui/key_editor/`, namespace `oa::ui::key_editor`.

It reads no file, clock, preference or network. The host fills `EditorInputs` and lays the screen out for its frame. Every word a player reads comes from `text.hpp`. Nothing here hosts the screen on the OA layer. The pointer, a finger, the keys, typed text, the wheel and the controller drive the model and the screen state, as [Input](#input) describes.

## The screen

`window_size` is the size class's dialog (480×324, 720×486, 960×600 points), no larger than the frame. `editor_layout` lays a modal kit window out from the model. Large and Regular put LAYOUT ▾, the aside and the search on one row: Large holds the aside between the drop-down and a 280-point search, and packs the pills at the right of the next row; Regular puts All categories on the left of the pills. Compact's header names the layout and its changes, and the row under it is the category drop-down and the search. LAYOUT ▾ is under MORE. Large keeps a 152-point category nav whose first entry is All with its count, and a long title wraps onto another line. Regular and Compact use a category drop-down, and Compact puts the filters in that menu. The filters are All, Changed, Clashes, Unbound and Latchable. Each row leads with the mark, then the action's name with a quiet aside, and up to four chord chips (one chip and "+n" at Compact). The category's tint square is on the nav and on the details' title. The chosen action's details sit beside the list at Regular and Large, and on a page of their own at Compact: what it does, where it works, its kind and its base, each chord with an explanation and a ✕, Latch and Hold to confirm when the action allows them, and RESET TO <base>. Groups starts with Number keys select groups. The footer is RESTORE DEFAULTS, CANCEL and SAVE. Compact's footer is MORE ▾, CANCEL and SAVE. Large's footer aside names the number-keys switch.

List, Keyboard and Mouse are tabs. At Large and Regular they lead the filter row and the pills sit to their right. At Compact they head the screen. The keyboard is ANSI, ISO or JIS, from the system or the drop-down. Holding is Nothing, Shift, Ctrl and Alt, and Cmd where Primary is the command key. Each key shows what the player's keyboard prints and the short name of what that plain chord does while playing, tinted by its category's letter. No key is dashed. The layer's modifiers are drawn held, and a clash is marked. At Compact the picture scrolls sideways in its well, and the legend, the grey-keys note and the card sit under it. At Regular and Large the card sits to the right of the legend. The card lists every action on the key the pointer or the focus is on. The mouse shows left, right, the wheel, middle, back and forward, and buttons 6 to 8 once the game has seen them pressed. At Regular and Large the picture is large and on the left, with a callout for each button, and the table on the right groups each place's roles as a chord and its role. At Compact the roles stay in a list. The scheme strip is Right-click and Left-click. It redraws the roles and does not change the layout's bindings.

The controls a journey presses, as `oa.` and the name:

| Name | What it is |
|---|---|
| `key-editor.layout` | LAYOUT ▾ on Large and Regular. At Compact the same items are under MORE. Items `key-editor.layout.<id>`, `.duplicate` and `.rename` |
| `key-editor.search` | The search field |
| `key-editor.category` | Regular and Compact's category drop-down. Items `key-editor.category.<id>`, and at Compact the filters |
| `key-editor.nav.<id>` | One Large nav entry |
| `key-editor.filter.all`, `.changed`, `.clashes`, `.unbound`, `.latchable` | The filter pills. At Compact they are items of the category menu |
| `key-editor.list` | The list. Its scroll bar is `key-editor.list.bar` |
| `key-editor.row.<id>` | A row. Its text is the name and its chords. A `gui.` id longer than 80 bytes, or any id that would make the name unsound or longer than 100 bytes (a long `select.tag-` id among them), is `key-editor.row.n<k>` |
| `key-editor.number-keys` | Number keys select groups, at the head of Groups. Parts `off` and `on` |
| `key-editor.detail.chord.<n>` | A chord, numbered from 1. Enter changes it. A fixed Escape chord is not a control |
| `key-editor.detail.chord.<n>.remove` | A chord's ✕, numbered from 1. A fixed chord has none |
| `key-editor.detail.add` | + ADD KEY |
| `key-editor.capture` | The capture prompt. Its text is the title, the chord and the status line |
| `key-editor.capture.well` | The well. A click or a wheel turn here finishes the chord, or starts again once one is showing |
| `key-editor.capture.means.letter`, `.place` | Means: the letter, or the key in that place |
| `key-editor.capture.starts.press`, `.double`, `.hold` | Starts on: a press, a double press or a hold |
| `key-editor.capture.status` | The line that says the chord is free, what it clashes with, or why it is refused |
| `key-editor.capture.cancel`, `.ok` | The capture's CANCEL and OK. OK is disabled while the chord is refused or still waiting |
| `key-editor.clash` | "{chord} is taken" |
| `key-editor.clash.replace`, `.swap`, `.cancel` | REPLACE, SWAP and CANCEL. SWAP is absent for + ADD KEY |
| `key-editor.card.keep-here`, `.keep-there`, `.choose` | The draft card: KEEP IT HERE, KEEP IT FOR the other action, and CHOOSE ANOTHER KEY |
| `key-editor.search.chord` | The chord chip in the search, with its ✕ |
| `key-editor.detail.latch`, `.confirm` | Latch and Hold to confirm. Parts `off` and `on` |
| `key-editor.detail.reset` | RESET TO <base> |
| `key-editor.back` | Compact's details page's BACK |
| `key-editor.more` | Compact's MORE ▾. Its item is `key-editor.restore` |
| `key-editor.restore`, `.cancel`, `.save` | RESTORE DEFAULTS, CANCEL and SAVE |
| `key-editor.close` | The header's × |
| `key-editor.summary`, `key-editor.footer-note` | Labels. No press |
| `key-editor.view.list`, `.keyboard`, `.mouse` | The List, Keyboard and Mouse tabs. Only the open tab takes Tab |
| `key-editor.layer.none`, `.shift`, `.ctrl`, `.alt`, `.cmd` | Holding. Cmd is absent where Primary is Ctrl |
| `key-editor.keyboard-shape` | The shape drop-down. Items `.system`, `.ansi`, `.iso` and `.jis` |
| `key-editor.keyboard.<code word>` | One key. The card under the picture is `key-editor.key-card` |
| `key-editor.mouse.mouse-left`, `.mouse-right`, `.mouse-middle`, `.wheel`, `.mouse-back`, `.mouse-forward` | The mouse's buttons. `.mouse-6` to `.mouse-8` appear once the game has seen them pressed |
| `key-editor.scheme.right-click`, `.left-click` | Clicks give orders with |
| `key-editor.mouse-table.<place>.<n>` | One role in one place, numbered from 1 |
| `key-editor.detail.find` | FIND ON KEYBOARD, in the details |
| `key-editor.details.bar` | The details' scroll bar |

Tab's order is LAYOUT ▾, the search, the categories, the pills, the list, the details' controls, the footer and the ×. At Compact the details page's order is its controls, BACK, RESET and SAVE. A switch's halves are `oa.<name>.off` and `oa.<name>.on`. A click on either half toggles the switch.

## Input

`editor_pointer`, `editor_key`, `editor_text`, `editor_wheel`, `editor_command` and `editor_stick` change only the model and `ScreenState`. They return what the host must do. `cancel` closes the editor. `save` writes the draft when `can_save` is true. `ask_keep_changes` names the layout being chosen. `ask_leave` asks before unsaved changes are left. `ask_name` asks a layout's name: its id is `duplicate` or `rename`.

On the list, Up and Down move the chosen row, Page Up and Page Down move by the rows shown, and Home and End go to the ends. Enter on a row opens Compact's details page, or moves the focus to the details' first control at Regular and Large. Delete on a focused chord removes it. Escape closes an open menu, else clears a search that holds words, else leaves Compact's details page, else asks to leave or closes. Typed text goes to a focused search. The search matches names, descriptions, asides, ids and category titles, folded as the Library folds them. The wheel and a finger scroll the pane under them. A tap selects the row. A press on a chord's ✕ takes that chord off.

`find` focuses the search. `back` closes a menu, or leaves the details page, or answers `cancel`. `save` answers `save`. `add_key` is + ADD KEY on the chosen row: the controller's X. The host maps a controller as it does for the Library: Y is `find`, X is `add_key`, B is `back`, Menu is `save`, and L1 and R1 are Shift+Tab and Tab. The right stick is `editor_stick`.

+ ADD KEY, or Enter on a chord, opens "Press the keys for {action}". While that prompt is waiting, keys go to the capture, and a button or a wheel turn in the well finishes the chord. Held modifiers show as keycaps. A lone modifier finishes only a modifier action. Escape cancels; Escape held for a second binds Escape. Once the chord is finished the prompt is an ordinary dialog: Tab and Enter reach OK, and Escape cancels. OK on a clash opens "{chord} is taken", with REPLACE, and SWAP only when a chord is being changed. CANCEL changes nothing. A click on OK or CANCEL is not itself a chord. The host must feed the dispatcher nothing while the prompt is open, and its tick runs the Escape timer. The platform's remaps run before the prompt sees the key.

In the empty, focused search, a chord with Ctrl, Alt, Meta or Primary, a function key, Print Screen, Pause, Insert, Page Up, Page Down, Home, End, a numpad key, or a side button over the field lists every action on that chord. A plain letter, digit or punctuation mark types into the search. The chip `key-editor.search.chord` clears that chord.

On the keyboard, the arrows and the controller's D-pad move between keys by where they sit. Enter on a free key fills the capture for the action chosen in the list, with the layer's modifiers and that key, and waits for OK. Enter on a bound key selects that action. Tab stops on the open view, the chosen layer, the shape drop-down and the focused key, each as one stop. A key's focus or the pointer fills the card. A finger's first tap is a hover and the second tap acts. FIND ON KEYBOARD outlines that action's keys.

On the mouse, a click on a button or a role in the table opens the capture for it. The scheme pills change the draft's scheme at once. The host writes Interface Type when the player saves, and CANCEL leaves it.

A clash already in the draft shows a card on the chosen action: KEEP IT HERE, KEEP IT FOR the other action, and CHOOSE ANOTHER KEY. KEEP IT HERE leaves the other action without that chord. The editor opens on the Clashes filter when the draft already names a clash and the host did not ask for another list.

`focused_field` returns the search field's rectangle while it has the focus, so the host can place the system's text input.

## Entry points

- `categories` lists the categories in order, with counts, tints and letters. Developer Mode adds Developer. Under the played mod's name (or "Tags" when no mod is named) the Tags group lists every Category word the loaded units carry, in the registry's order, named from the `select.tag` template with the data's spelling. A word another action already selects lists that action, with the word as its aside, and All still counts the action once. A layout's `select.tag-` binding the loaded units lack is a greyed row in Tags, with the aside "not in the loaded units", not under "Not in this version". Other actions this version does not know are "Not in this version", read only.
- `rows` is one category, one filter and the search. `details` is the chosen action. `counts` is what the pills say.
- `remove_chord` takes one chord off. Escape's fixed chord on `chat.cancel` and `ui.screen-cancel` is refused, and so is any change to an unknown action. The action can take more chords.
- `reset_action` drops the action from the draft. `restore_defaults` clears the draft's known bindings and its helpers, and puts number keys back. Unknown actions stay, so a save writes them back.
- `set_latch` and `set_confirm` write the draft's helpers. Turning one latch on while the helper was unset writes a list of just that action. Turning one confirm off while it was unset writes every confirmable action except that one.
- `set_number_keys` is SwitchAlt. It is saved with the layout and is not one of the own changes the summary counts.
- `choose_layout` chooses a base. Setting the changes aside clears the draft and leaves the previous own folder for the host to leave untouched. `duplicate` makes a new layout from the current one. An empty name does nothing. `rename` renames the chosen layout. OA default is not renamed.
- `changes` counts the draft's bindings and helpers. `unsaved` counts those and a layout that was chosen, duplicated or renamed, and the number-keys switch. `clashes` is how many clashing pairs the bindings in use have, including a clash that lives only in the mod's data. `draft_clashes` counts the pairs that name an action the draft binds. `can_save` is false while `draft_clashes` is not zero. `result` is the manifest to write, SwitchAlt, and any layout just made or renamed.
- `keyboard_look` is the keyboard for one shape and one layer. `key_card` is every action on one key. `mouse_look` and `mouse_table` are the mouse for one scheme. `keys_of` is the keys FIND ON KEYBOARD outlines. `set_scheme` changes the draft's scheme and leaves the bindings.

## The keys overlay

`overlay_groups` lists the keys of the contexts that are active now, from the resolved bindings it is given. An action is listed when one of its contexts is active, its conditions hold, and its first chord is a key. A numbered family whose chords run in order collapses into one range. Actions that exist only because a hack is on are grouped under "This mod". `overlay_layout` places one column of 240 points at Compact, and two columns of 420 at Regular and 520 at Large, inside the battlefield and to the right of the side column. `draw_overlay` blends the panel over the battlefield. The screen is `keys-overlay`; the button is `keys-overlay.edit`, the close mark `keys-overlay.close`, the list `keys-overlay.list` and the scroll bar `keys-overlay.bar`.

## Tests

`ui-key-editor-model` checks the categories, the filters, the search, removing and resetting a chord, Latch and Hold to confirm, the number-keys switch, choosing, duplicating and renaming a layout, a clash in the mod's data that stays listed while SAVE stays allowed, a draft chord that blocks SAVE, an unknown action written back, the Tags group's order, a bound tag, a tag the loaded units lack, Developer Mode, and Escape's fixed chord.

`ui-key-editor-layout` lays the screen out at Compact, Regular and Large, including the keyboard and the mouse, and the ISO and JIS keyboards at Large. It checks that every part lies in the window, that names are sound, that Tab's order is the one above, and that every text fits. A key scrolled past the well is clipped.

`ui-key-editor-overlay` lists the keys of the contexts active while playing, while watching and while placing a building. A numbered family whose chords run in order collapses into one range, and a broken run does not. Hack actions sit under This mod. The On line carries the latches the caller names. The panel at each class sits inside the battlefield, to the right of a 128-point column. The automation names are sound.

`ui-key-editor-input` drives the keys, the pad's commands, and a finger's scroll and tap.

`ui-key-editor-capture` captures a chord, settles a clash and finds by key.

`ui-key-editor-views` checks the labels, the layers, Escape, Settings, an AZERTY map, a free key's click, the mouse in both schemes, and FIND ON KEYBOARD.

`ui-key-editor-draw` and `ui-key-editor-draw-data` draw the list, the keyboard, the mouse, the capture, clash and card, and the keys overlay at each class, in the block font and again in the installed game's fonts, and check them against `tests/frames.txt`. The ISO and JIS keyboards are drawn at Large. The overlay pictures are `keys-overlay-<class>.png`.

## State

`Editor` holds the host's `EditorInputs`: the base layout, the chosen layout, the mod's keys, the hacks that are on, the scheme, the keyboard map, the platform, the own draft and its helpers, SwitchAlt, Developer Mode, Hold to confirm's setting, the played mod's name and the installed layouts. A call changes the draft. The host fills the inputs and writes `result()`.

`ScreenState` holds the hover, the press and the focus, the list and details scrolls, the search, which menu is open, the category, the filter, the chosen action, whether Compact's details page is showing, whether the editor was opened over a match, the capture or clash prompt, and a chord the empty search is listing. It also holds which view is showing, the keyboard's shape and whether it follows the system, the modifier layer, how far the compact keyboard is scrolled, the key whose card is showing, the key the arrows move, the action FIND ON KEYBOARD outlines, and whether buttons 6 to 8 are shown.

## Invariants

- A row shows the chords resolution gives for that action, from the base, the chosen layout, the mod's data and the draft.
- Changed means the draft names the action. Unbound means it has no chord. Clashes means `find_clashes` reports a pair. Latchable means the registry's flag.
- `can_save` stays false while a clash names an action the draft binds. A clash in the mod's data alone stays in the Clashes filter and leaves SAVE allowed. The module writes no file; the host writes `result()` when the player saves.
- Escape's fixed chord on `chat.cancel` and `ui.screen-cancel` stays. An action this version does not know is listed, refuses every change, and is written back.
- Developer rows are listed while Developer Mode is on.
- Number keys select groups is SwitchAlt. The summary's count of the player's own changes leaves it out.
- Every word a player reads comes from `text.hpp`.

## Limitations

Nothing here imports, shares or pastes a layout. The host opens the screen, chooses the keyboard's shape, feeds it nothing while a capture is waiting, asks the name questions, writes the layout and Interface Type, and reloads the bindings. A layout's name is asked by the host. The module reads no preference, no clock and does not pause a match. A mouse or wheel chord carries no keyboard modifiers: the screen's wheel and pointer do not say which keys are held. The screen's wheel is the vertical turn. The keyboard is drawn by the kit's `add_keyboard`, and the mouse by `add_mouse`.
