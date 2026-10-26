# Key editor

The key editor's list: every rebindable action, by category, with its keys, where it works and whether the player changed it. Library `oa-ui-key-editor`, headers `include/oa/ui/key_editor/`, namespace `oa::ui::key_editor`.

It reads no file, clock, preference or network. The host fills `EditorInputs` and lays the screen out for its frame. Every word a player reads comes from `text.hpp`. Nothing here hosts the screen on the OA layer. The pointer, a finger, the keys, typed text, the wheel and the controller drive the model and the screen state, as [Input](#input) describes.

## The screen

`window_size` is the size class's dialog (480×324, 720×486, 960×600 points), no larger than the frame. `editor_layout` lays a modal kit window out from the model. The header names the layout and its own changes, with LAYOUT ▾ and a search. Large keeps a 152-point category nav. Regular and Compact use a category drop-down, and Compact puts the filters in that menu. The filters are All, Changed, Clashes, Unbound and Latchable. Each row has a mark, the action's name with a quiet aside, and up to four chord chips (one chip and "+n" at Compact). The chosen action's details sit beside the list at Regular and Large, and on a page of their own at Compact: what it does, where it works, its kind and its base, each chord with an explanation and a ✕, Latch and Hold to confirm when the action allows them, and RESET TO <base>. Groups starts with Number keys select groups. The footer is RESTORE DEFAULTS, CANCEL and SAVE. Compact's footer is MORE ▾, CANCEL and SAVE. Large's footer aside names the number-keys switch.

The controls a journey presses, as `oa.` and the name:

| Name | What it is |
|---|---|
| `key-editor.layout` | LAYOUT ▾. Items `key-editor.layout.<id>`, `.duplicate` and `.rename` |
| `key-editor.search` | The search field |
| `key-editor.category` | Regular and Compact's category drop-down. Items `key-editor.category.<id>`, and at Compact the filters |
| `key-editor.nav.<id>` | One Large nav entry |
| `key-editor.filter.all`, `.changed`, `.clashes`, `.unbound`, `.latchable` | The filter pills. At Compact they are items of the category menu |
| `key-editor.list` | The list. Its scroll bar is `key-editor.list.bar` |
| `key-editor.row.<id>` | A row. Its text is the name and its chords. A `gui.` id longer than 80 bytes, or any id that would make the name unsound or longer than 100 bytes, is `key-editor.row.n<k>` |
| `key-editor.number-keys` | Number keys select groups, at the head of Groups. Parts `off` and `on` |
| `key-editor.detail.chord.<n>.remove` | A chord's ✕, numbered from 1. A fixed chord has none |
| `key-editor.detail.latch`, `.confirm` | Latch and Hold to confirm. Parts `off` and `on` |
| `key-editor.detail.reset` | RESET TO <base> |
| `key-editor.back` | Compact's details page's BACK |
| `key-editor.more` | Compact's MORE ▾. Its item is `key-editor.restore` |
| `key-editor.restore`, `.cancel`, `.save` | RESTORE DEFAULTS, CANCEL and SAVE |
| `key-editor.close` | The header's × |
| `key-editor.summary`, `key-editor.footer-note` | Labels. No press |
| `key-editor.details.bar` | The details' scroll bar |

Tab's order is LAYOUT ▾, the search, the categories, the pills, the list, the details' controls, the footer and the ×. At Compact the details page's order is its controls, BACK, RESET and SAVE. A switch's halves are `oa.<name>.off` and `oa.<name>.on`. A click on either half toggles the switch.

## Input

`editor_pointer`, `editor_key`, `editor_text`, `editor_wheel`, `editor_command` and `editor_stick` change only the model and `ScreenState`. They return what the host must do. `cancel` closes the editor. `save` writes the draft when `can_save` is true. `ask_keep_changes` names the layout being chosen. `ask_leave` asks before unsaved changes are left. `ask_name` asks a layout's name: its id is `duplicate` or `rename`.

On the list, Up and Down move the chosen row, Page Up and Page Down move by the rows shown, and Home and End go to the ends. Enter on a row opens Compact's details page, or moves the focus to the details' first control at Regular and Large. Delete on a focused chord removes it. Escape closes an open menu, else clears a search that holds words, else leaves Compact's details page, else asks to leave or closes. Typed text goes to a focused search. The search matches names, descriptions, ids and category titles, folded as the Library folds them. The wheel and a finger scroll the pane under them. A tap selects the row. A press on a chord's ✕ takes that chord off.

`find` focuses the search. `back` closes a menu, or leaves the details page, or answers `cancel`. `save` answers `save`. The host maps a controller as it does for the Library: Y is `find`, B is `back`, Menu is `save`, and L1 and R1 are Shift+Tab and Tab. The right stick is `editor_stick`.

`focused_field` returns the search field's rectangle while it has the focus, so the host can place the system's text input.

## Entry points

- `categories` lists the categories in order, with counts, tints and letters. Developer Mode adds Developer. Actions this version does not know are "Not in this version", read only.
- `rows` is one category, one filter and the search. `details` is the chosen action. `counts` is what the pills say.
- `remove_chord` takes one chord off. Escape's fixed chord on `chat.cancel` and `ui.screen-cancel` is refused, and so is any change to an unknown action. The action can take more chords.
- `reset_action` drops the action from the draft. `restore_defaults` clears the draft's known bindings and its helpers, and puts number keys back. Unknown actions stay, so a save writes them back.
- `set_latch` and `set_confirm` write the draft's helpers. Turning one latch on while the helper was unset writes a list of just that action. Turning one confirm off while it was unset writes every confirmable action except that one.
- `set_number_keys` is SwitchAlt. It is saved with the layout and is not one of the own changes the summary counts.
- `choose_layout` chooses a base. Setting the changes aside clears the draft and leaves the previous own folder for the host to leave untouched. `duplicate` makes a new layout from the current one. An empty name does nothing. `rename` renames the chosen layout. OA default is not renamed.
- `changes` counts the draft's bindings and helpers. `unsaved` counts those and a layout that was chosen, duplicated or renamed, and the number-keys switch. `clashes` is how many clashing pairs the bindings in use have, including a clash that lives only in the mod's data. `draft_clashes` counts the pairs that name an action the draft binds. `can_save` is false while `draft_clashes` is not zero. `result` is the manifest to write, SwitchAlt, and any layout just made or renamed.

## Tests

`ui-key-editor-model` checks the categories, the filters, the search, removing and resetting a chord, Latch and Hold to confirm, the number-keys switch, choosing, duplicating and renaming a layout, a clash in the mod's data that stays listed while SAVE stays allowed, a draft chord that blocks SAVE, an unknown action written back, Developer Mode, and Escape's fixed chord.

`ui-key-editor-layout` lays the screen out at Compact, Regular and Large. It checks that every part lies in the window, that names are sound, that Tab's order is the one above, and that every text fits.

`ui-key-editor-input` drives the keys, the pad's commands, and a finger's scroll and tap.

`ui-key-editor-draw` and `ui-key-editor-draw-data` draw the five pictures, in the block font and again in the installed game's fonts, and check them against `tests/frames.txt`.

## State

`Editor` holds the host's `EditorInputs`: the base layout, the chosen layout, the mod's keys, the hacks that are on, the scheme, the keyboard map, the platform, the own draft and its helpers, SwitchAlt, Developer Mode, Hold to confirm's setting, the played mod's name and the installed layouts. A call changes the draft. The host fills the inputs and writes `result()`.

`ScreenState` holds the hover, the press and the focus, the list and details scrolls, the search, which menu is open, the category, the filter, the chosen action, whether Compact's details page is showing, and whether the editor was opened over a match.

## Invariants

- A row shows the chords resolution gives for that action, from the base, the chosen layout, the mod's data and the draft.
- Changed means the draft names the action. Unbound means it has no chord. Clashes means `find_clashes` reports a pair. Latchable means the registry's flag.
- `can_save` stays false while a clash names an action the draft binds. A clash in the mod's data alone stays in the Clashes filter and leaves SAVE allowed. The module writes no file; the host writes `result()` when the player saves.
- Escape's fixed chord on `chat.cancel` and `ui.screen-cancel` stays. An action this version does not know is listed, refuses every change, and is written back.
- Developer rows are listed while Developer Mode is on.
- Number keys select groups is SwitchAlt. The summary's count of the player's own changes leaves it out.
- Every word a player reads comes from `text.hpp`.

## Limitations

Nothing here captures a chord, draws a clash card, finds a binding by the key pressed, or draws the keyboard and mouse views. Nothing here imports, shares or pastes a layout. The host opens the screen, asks the questions, writes the layout and reloads the bindings. A layout's name is asked by the host. The module reads no preference and does not pause a match.
