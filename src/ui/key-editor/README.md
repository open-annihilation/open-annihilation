# Key editor

The key editor's list, keyboard and mouse: every rebindable action, by category, with its keys, where it works and whether the player changed it, and the player's keyboard and mouse with what each key and button does. Library `oa-ui-key-editor`, headers `include/oa/ui/key_editor/`, namespace `oa::ui::key_editor`.

It reads no file, clock, preference or network. The host fills `EditorInputs` and lays the screen out for its frame. Every word a player reads comes from `text.hpp`. Nothing here hosts the screen on the OA layer. The pointer, a finger, the keys, typed text, the wheel and the controller drive the model and the screen state, as [Input](#input) describes.

## The screen

`window_size` is the size class's dialog (480×324, 720×486, 960×600 points), no larger than the frame. Its header is Kit 2's window header, 36 points, at Regular and Large, and the kit's 26-point band at Compact, as Settings and the Library have it; the close mark sits in its middle. `editor_layout` lays a modal kit window out from the model. The editor opens on Common commands: the commands players change first, in their own order, each with its area beside it (Orders, Selection…). Every other category follows, and All actions, the registry's whole list, sits last under a rule. Large and Regular put LAYOUT ▾, the aside, the search and Find by key on one row; Regular puts its category drop-down on the left of the pills. Compact's header names the layout and its changes, and the row under the tabs is the category drop-down, the search and Find by key. LAYOUT ▾ is under More at Compact. The toolbar takes Kit 2's sizes, as the Library's does, inside Kit 2's window padding: the tab strip 32, 38 and 44 points high at Compact, Regular and Large, the search and every drop-down Kit 2's text field (30, 36 and 40), the filter pills, the layer pills and the scheme pills Kit 2's filter chip (24, 26 and 28), and Find by key and Show as list the control height (28, 34 and 38). Its rows sit a space apart at Compact and Kit 2's row gap apart above it. The pills stay beside the tabs, and Regular's beside its category drop-down, while they fit there, and take a row of their own under them when they do not. Large keeps a 172-point category nav. The filters are All, Changed, Clashes, Unbound and Latchable; Compact keeps them in the category menu. Rows are Kit 2's control height (28, 34 and 38 points). Each row leads with the mark, then the action's name with a quiet aside, then at Regular and Large Changed for the player's own change or the row's area outside its own category, and up to four chord chips (one chip and "+n" at Compact). An order's 3.1c word (ATTACK) is in the details, not the row. The chosen action's details sit beside the list at Regular and Large, and on a page of their own at Compact, under ‹ and the list's name: its name and what it does, where it works, a draft clash with Settle…, each chord on a well with Change… and its remove mark, + Add a key, then More options (Known as, the kind, the base, what it works with, each chord's explanation, Latch and Hold to confirm when the action allows them, and Find on keyboard), and at the foot where its keys come from ("From {layout}", or "Your change, over {layout}") with Reset. Groups starts with Number keys select groups. The footer is the draft line ("2 unsaved edits", a dot while there are any) and Undo on the left, a note while a clash holds Save back, then More, Cancel and Save changes. More holds Restore defaults, Import…, Share… and Paste…, and at Compact the layouts first.

Commands, Keyboard and Mouse are tabs. At Large and Regular they lead the filter row and the pills sit to their right. At Compact they head the screen. The keyboard is ANSI, ISO or JIS, from the system or the drop-down. Holding is Nothing, Shift, Ctrl and Alt, and Cmd where Primary is the command key. Each key shows its legend, as the player's keyboard prints it, tinted by its category; the legend under the picture names the tints. The layer's modifiers are drawn held, a clash is marked and a key the draft changed is marked. A press on a key only inspects it. The inspector, a column at the right at Regular and Large and under the picture at Compact, shows the key, what it does in each place, Change {action}'s key… for what it does while playing, and Use {key} for {action}… for the action chosen in the list. Show as list puts every key that does something in this layer in a list in the picture's place, and Show keyboard puts the picture back. At Compact the picture scrolls sideways in its well. The mouse shows left, right, the wheel, middle, back and forward, and buttons 6 to 8 once the game has seen them pressed. At Regular and Large the picture is large and on the left, with a callout for each button, and the table on the right groups each place's roles as a chord and its role. At Compact the roles stay in a list. The scheme strip is Right-click and Left-click. It redraws the roles and does not change the layout's bindings.

The controls a journey presses, as `oa.` and the name:

| Name | What it is |
|---|---|
| `key-editor.layout` | LAYOUT ▾ on Large and Regular. At Compact the same items lead More. Items `key-editor.layout.<id>`, `.duplicate` and `.rename` |
| `key-editor.search` | The search field. Typing searches commands; in Common commands it reaches every action, common ones first |
| `key-editor.find` | Find by key, beside the search |
| `key-editor.category` | Regular and Compact's category drop-down. Items `key-editor.category.<id>`, and at Compact the filters |
| `key-editor.nav.<id>` | One Large nav entry. `common` is Common commands, `all` All actions |
| `key-editor.filter.all`, `.changed`, `.clashes`, `.unbound`, `.latchable` | The filter pills. At Compact they are items of the category menu |
| `key-editor.list` | The list. Its scroll bar is `key-editor.list.bar` |
| `key-editor.row.<id>` | A row. Its text is the name and its chords. A `gui.` id longer than 80 bytes, or any id that would make the name unsound or longer than 100 bytes (a long `select.tag-` id among them), is `key-editor.row.n<k>` |
| `key-editor.number-keys` | Number keys select groups, at the head of Groups. Parts `off` and `on` |
| `key-editor.detail.chord.<n>` | A chord's Change…, numbered from 1. Its text is the chord. A fixed Escape chord has none |
| `key-editor.detail.chord.<n>.remove` | A chord's ✕, numbered from 1. A fixed chord has none |
| `key-editor.detail.add` | + Add a key |
| `key-editor.detail.more` | The details' More options |
| `key-editor.card.settle` | Settle…, on a draft clash in the details. It opens the clash question |
| `key-editor.capture` | The capture prompt. Its text is the title, the chord and the status line |
| `key-editor.capture.well` | The well. A click or a wheel turn here finishes the chord, or starts again once one is showing |
| `key-editor.capture.more` | The capture's More options |
| `key-editor.capture.means` | Same place on every keyboard, under More options. Parts `off` (the letter) and `on` (the key's place) |
| `key-editor.capture.starts.press`, `.double`, `.hold` | How it acts, under More options: a press, a double press or a hold |
| `key-editor.capture.status` | The line that says the chord is free, what uses it, or why it is refused |
| `key-editor.capture.cancel`, `.ok` | The capture's Cancel, and Use {chord} for {action}, or Choose what happens… for a chord in use. It is disabled while the chord is refused or still waiting |
| `key-editor.clash` | The clash question. Its text is the question and every outcome |
| `key-editor.clash.replace`, `.swap` | The outcomes Move it to {action} and Swap them. A press marks one; Swap them is absent for + Add a key. Each one's text is what it leaves on both actions |
| `key-editor.card.keep-here`, `.keep-there` | The outcomes Keep it for this action and for the other, while a draft clash is settled |
| `key-editor.clash.confirm` | The confirm button, named after the marked outcome. Nothing changes before it |
| `key-editor.clash.another`, `.cancel` | Choose another key, and Cancel |
| `key-editor.find-key` | Find by key. Its text is the line and every use of the chord |
| `key-editor.find-key.well` | Its well. A press here listens for another chord |
| `key-editor.find-key.row.<n>` | One use, numbered from 1. A press chooses that action in the list |
| `key-editor.find-key.close` | Find by key's Close |
| `key-editor.detail.latch`, `.confirm` | Latch and Hold to confirm, under More options. Parts `off` and `on` |
| `key-editor.detail.reset` | Reset, at the details' foot |
| `key-editor.back` | Compact's details page's ‹ and the list's name |
| `key-editor.draft` | The footer's draft line. Label, no press |
| `key-editor.undo` | Undo |
| `key-editor.more` | More. Its items are `key-editor.restore`, `.import`, `.share` and `.paste`, after the layouts at Compact |
| `key-editor.restore`, `.import`, `.share`, `.paste` | Restore defaults, Import…, Share… and Paste…, items of More |
| `key-editor.cancel`, `.save` | Cancel and Save changes |
| `key-editor.share.name`, `.id`, `.text` | The share name, the id it folds to, and the text form |
| `key-editor.share.copy`, `.save-file`, `.open-folder`, `.close` | COPY AS TEXT, SAVE FILE…, OPEN FOLDER and CLOSE. OPEN FOLDER shows once a file was written |
| `key-editor.paste.summary`, `.list`, `.apply`, `.cancel` | What the paste would change, its rows, APPLY {n} CHANGES and CANCEL |
| `key-editor.import.from.eye`, `.tdraw`, `.ddraw` | The older setup the import reads |
| `key-editor.import.choose`, `.summary`, `.list`, `.apply`, `.cancel` | CHOOSE ANOTHER FILE…, what was found, its rows, IMPORT {n} KEYS and CANCEL |
| `key-editor.close` | The header's × |
| `key-editor.summary`, `key-editor.footer-note` | Labels. No press. The summary's text is the whole summary, of which the row shows what fits |
| `key-editor.view.list`, `.keyboard`, `.mouse` | The Commands, Keyboard and Mouse tabs. Only the open tab takes Tab |
| `key-editor.layer.none`, `.shift`, `.ctrl`, `.alt`, `.cmd` | Holding. Cmd is absent where Primary is Ctrl |
| `key-editor.keyboard-shape` | The shape drop-down. Items `.system`, `.ansi`, `.iso` and `.jis` |
| `key-editor.keyboard.<code word>` | One key. A press inspects it |
| `key-editor.key-card` | The inspector. Its text is the key and every line it shows |
| `key-editor.key-card.change`, `.use` | Change {action}'s key… and Use {key} for {action}… |
| `key-editor.show-as-list` | Show as list, and Show keyboard |
| `key-editor.key-list` | Every key that does something in the layer. Its text is every line |
| `key-editor.mouse.mouse-left`, `.mouse-right`, `.mouse-middle`, `.wheel`, `.mouse-back`, `.mouse-forward` | The mouse's buttons. `.mouse-6` to `.mouse-8` appear once the game has seen them pressed |
| `key-editor.scheme.right-click`, `.left-click` | Clicks give orders with |
| `key-editor.mouse-table.<place>.<n>` | One role in one place, numbered from 1 |
| `key-editor.detail.find` | Find on keyboard, under More options |
| `key-editor.details.bar` | The details' scroll bar |

Tab's order is LAYOUT ▾, the search, Find by key, the categories, the pills, the list, the details' controls, Undo, More, Cancel, Save changes and the ×. At Compact the details page's order is ‹, its controls, Undo and Save changes. A switch's halves are `oa.<name>.off` and `oa.<name>.on`. A click on either half toggles the switch.

## Input

`editor_pointer`, `editor_key`, `editor_text`, `editor_wheel`, `editor_command` and `editor_stick` change only the model and `ScreenState`. They return what the host must do. `cancel` closes the editor. `save` writes the draft when `can_save` is true. `ask_keep_changes` names the layout being chosen. `ask_leave` asks before unsaved edits are closed: the host offers Save, Discard and Keep editing, or Discard and Keep editing while a clash holds Save back. `ask_name` asks a layout's name: its id is `duplicate` or `rename`.

On the list, Up and Down move the chosen row, Page Up and Page Down move by the rows shown, and Home and End go to the ends. Enter on a row opens Compact's details page, or moves the focus to the details' first control at Regular and Large. Delete on a focused chord removes it. Escape closes an open menu, else clears a search that holds words, else leaves Compact's details page, else asks before closing or closes. Cancel and the × do the same last step. Typed text goes to a focused search; a key typed there is only text. The search matches names, descriptions, asides, ids, an order's 3.1c word and category titles, folded as the Library folds them. The wheel and a finger scroll the pane under them. A tap selects the row. A press on a chord's ✕ takes that chord off.

`find` focuses the search. `back` closes a menu, or leaves the details page, or answers `cancel`. `save` answers `save`. `add_key` is + Add a key on the chosen row: the controller's X. `undo` is Undo; the host gives it Primary+Z while the search does not have the focus. The host maps a controller as it does for the Library: Y is `find`, X is `add_key`, B is `back`, Menu is `save`, and L1 and R1 are Shift+Tab and Tab. The right stick is `editor_stick`.

+ Add a key, or Change… on a chord, opens "New key for {action}" under "Add a key" or "Change a key" and where it is read. While that prompt is waiting, keys go to the capture, and a button or a wheel turn in the well finishes the chord. Held modifiers show as keycaps. A lone modifier finishes only a modifier action. Escape cancels; Escape held for a second binds Escape. Once the chord is finished the prompt says what it would do: "{chord} is free while playing" and Use {chord} for {action}, "{chord} is in use while playing" with the actions that use it and Choose what happens…, or why it is refused. A click in the well listens again. More options holds Same place on every keyboard and How it acts. Choose what happens… opens the clash question: "{chord} is already in use", what uses it, and each outcome with what it leaves on both actions, worked out on a copy of the draft: Move it to {action}, and Swap them when a chord is being changed. A press on an outcome only marks it; the confirm button, named after it, writes it, as one step of Undo. Choose another key listens again, and Cancel changes nothing. A click on a button is not itself a chord. The host must feed the dispatcher nothing while a prompt is open, and its tick runs the Escape timer. The platform's remaps run before the prompt sees the key.

Find by key listens the same way, apart from the search, and lists every action on the chord heard, in every context, with Open ›, which chooses that action in the list and shows its category or All actions. It changes nothing. A press in its well listens for another chord, a tap of Escape closes it, and Escape held for a second looks Escape up.

On the keyboard, the arrows and the controller's D-pad move between keys by where they sit. A press or Enter on a key inspects it and changes nothing. In the inspector, Change {action}'s key… chooses the action the key does while playing and opens the capture for that chord, and Use {key} for {action}… fills the capture for the action chosen in the list with the layer's modifiers and that key, and waits for its button. Tab stops on the open view, the chosen layer, the shape drop-down, Show as list and the focused key, each as one stop. A key's focus or the pointer fills the inspector. A finger's first tap is a hover and the second tap acts. Find on keyboard outlines that action's keys.

On the mouse, a click on a button or a role in the table opens the capture for it. The scheme pills change the draft's scheme at once. The host writes Interface Type when the player saves, and Cancel leaves it.

A clash already in the draft shows on the chosen action's details, "{chord} does two things", with Settle…, which opens the same clash question: Keep it for this action and Keep it for the other, each with what it leaves, the confirm button, and Choose another key, which opens the capture for this action's chord. The editor opens on All actions and the Clashes filter when the draft already names a clash and the host did not ask for another list.

Every change to the draft is one step of Undo: a removed chord, a capture with what it moved off another action, a settled clash, Reset, Restore defaults, a paste, an import, a helper, the number-keys switch, the scheme and a layout chosen, made or renamed. The draft line counts the edits not saved yet; the summary's count of the player's own changes is what is saved.

Share… shows the name, the id it folds to, the base, how many keys and helpers change, and the text form in a well that scrolls when the lines do not fit. The share, paste and import prompts are content-sized panels over the dimmed editor. COPY AS TEXT and SAVE FILE… are the host's. Paste…, and Primary+V while the search is not focused, asks the host to read the clipboard. APPLY writes the preview into the draft: a chord that is already in use stays where it is and is listed as a clash, which holds Save changes back until Settle… settles it. An action this version does not know is kept. Import… asks the host for the older keys, lists the ones that differ, and IMPORT writes them into the draft, moving a chord off the action that had it. Nothing of either prompt is in use until the player saves.

`focused_field` returns the search field's rectangle while it has the focus, so the host can place the system's text input.

## Entry points

- `categories` lists the categories in order, with counts, tints and letters: Common commands first, All actions last. Developer Mode adds Developer. Under the played mod's name (or "Tags" when no mod is named) the Tags group lists every Category word the loaded units carry, in the registry's order, named from the `select.tag` template with the data's spelling. A word another action already selects lists that action, with the word as its aside, and All still counts the action once. A layout's `select.tag-` binding the loaded units lack is a greyed row in Tags, with the aside "not in the loaded units", not under "Not in this version". Other actions this version does not know are "Not in this version", read only.
- `rows` is one category, one filter and the search. Common commands lists its commands in their own order with each row's area; with a search it lists every matching action, common ones first. `details` is the chosen action, with `technical`, its id and an order's 3.1c word. `counts` is what the pills say.
- `remove_chord` takes one chord off. Escape's fixed chord on `chat.cancel` and `ui.screen-cancel` is refused, and so is any change to an unknown action. The action can take more chords.
- `reset_action` drops the action from the draft. `restore_defaults` clears the draft's known bindings and its helpers, and puts number keys back. Unknown actions stay, so a save writes them back.
- `set_latch` and `set_confirm` write the draft's helpers. Turning one latch on while the helper was unset writes a list of just that action. Turning one confirm off while it was unset writes every confirmable action except that one.
- `set_number_keys` is SwitchAlt. It is saved with the layout and is not one of the own changes the summary counts.
- `choose_layout` chooses a base. Setting the changes aside clears the draft and leaves the previous own folder for the host to leave untouched. `duplicate` makes a new layout from the current one. An empty name does nothing. `rename` renames the chosen layout. OA default is not renamed.
- `changes` counts the draft's bindings and helpers. `unsaved` counts those and a layout that was chosen, duplicated or renamed, and the number-keys switch. `clashes` is how many clashing pairs the bindings in use have, including a clash that lives only in the mod's data. `draft_clashes` counts the pairs that name an action the draft binds, and a clash a paste is holding. `can_save` is false while `draft_clashes` is not zero. `result` is the manifest to write, SwitchAlt, and any layout just made or renamed. When an import recorded a tool, a file and a date, the manifest names them under `imported-from`.
- `id_from_name` folds a share name to a lower-case id of 1 to 64 bytes. A built-in id, and an id that starts with `own-`, is refused. `share_summary` is the name, that id, the base, the counts and the text form.
- `paste_preview` reads the text form or a whole manifest and lists each change, clash, unknown action and error with its line and column. `apply_paste` writes the changes into the draft and holds each clash. `import_preview` keeps the keys that differ from the chords in use. `apply_import` moves each chord and names the action that loses it.
- `undo` takes back the last step of changes and `can_undo` says whether there is one; `EditStep` makes the changes made while it lives one step. Undo keeps the last 100 steps.
- `keyboard_look` is the keyboard for one shape and one layer. `key_card` is every action on one key. `mouse_look` and `mouse_table` are the mouse for one scheme. `keys_of` is the keys Find on keyboard outlines. `set_scheme` changes the draft's scheme and leaves the bindings.

## The keys overlay

`overlay_groups` lists the keys of the contexts that are active now, from the resolved bindings it is given: an action is listed when one of its contexts is active and every condition it names holds, with its first key chord. Pointer and wheel actions are left out. A numbered family whose keys run in order, with the same modifiers, collapses into one range (Ctrl+1…9); a broken run lists each member. Actions that exist only because a hack is on are grouped under "This mod", just before Game. `overlay_title` is "Keys while placing a building" while a building is placed, "Keys while watching" while only watching, and "Keys while playing" otherwise.

`overlay_layout` places the panel right of the side column and below the battlefield's top: 240 points wide with one column of groups at Compact, 420 at Regular and 520 at Large with two, as tall as its content up to the battlefield less its margins, beyond which the groups scroll under a fixed header and footer. The header holds the title, EDIT KEYS… and the close mark, the layout's name and changes, and the "On:" line, whose chips are the latches and sticky modifiers that are on (filled while latched or locked). Each group is headed by its tinted letter keycap and its name; each line is the action's name, "held" for a hold or a modifier, and its chord chip. `draw_overlay` blends the HUD's panel colour over what lies under the panel, with rounded corners and a hairline edge, then draws the list. The screen is `keys-overlay`; the button is `keys-overlay.edit`, the close mark `keys-overlay.close`, the list `keys-overlay.list` (its text every word the panel shows, one line each) and the scroll bar `keys-overlay.bar`. Tab's order is EDIT KEYS…, the close mark and, while the groups scroll, the list. In a match Tab and Shift+Tab walk them and show the focus; while it shows, Enter and Space press the focused button, Page Up, Page Down, Home and End scroll the groups, Up and Down on the list scroll it a step, and the arrows otherwise move the focus. A press on the match hands those keys back to it. On a pad, L1 and R1 page the groups while they scroll, Y is EDIT KEYS… and View's tap closes the panel.

## Tests

`ui-key-editor-model` checks the categories, Common commands and a search in it, Undo step by step and a capture undone in one step, the filters, the search, removing and resetting a chord, Latch and Hold to confirm, the number-keys switch, choosing, duplicating and renaming a layout, a clash in the mod's data that stays listed while saving stays allowed, a draft chord that blocks saving, an unknown action written back, the Tags group's order, a bound tag, a tag the loaded units lack, Developer Mode, and Escape's fixed chord.

`ui-key-editor-layout` lays the screen out at Compact, Regular and Large, including the keyboard and the mouse, and the ISO and JIS keyboards at Large. It checks that every part lies in the window, that names are sound, that Tab's order is the one above, and that every text fits. A key scrolled past the well is clipped.

`ui-key-editor-overlay` lists the keys of the contexts active while playing, while watching and while placing a building. Assign group collapses into Ctrl+1…9, and with group 5 moved to Q it is Ctrl+1…4, Q and Ctrl+6…9. Hack actions sit under This mod, and a hack key whose condition does not hold is left out. The On line shows the latches the caller names as chips, and no On line shows without one. The panel at each class sits inside the battlefield, to the right of a 128-point column, every part inside it, with one column of groups at Compact and two above it; on a short battlefield the groups scroll. The automation names are sound.

`ui-key-editor-input` drives the keys, the pad's commands, a finger's scroll and tap, and Cancel and the × asking before unsaved edits are closed, with Undo and Primary+Z taking them back.

`ui-key-editor-capture` captures a chord and checks what the capture says and its button; that the clash question lists each outcome on both actions, changes nothing until its confirm button, and that Cancel and Choose another key change nothing; Settle… on a draft clash; and Find by key, which changes nothing, apart from the search.

`ui-key-editor-views` checks the labels, the layers, Escape, Settings, an AZERTY map, a key's click that only inspects, the inspector's buttons, the mouse in both schemes, and Find on keyboard.

`ui-key-editor-share` checks a share id, a paste that keeps a clash and an unknown action, and an import that moves a chord and records `imported-from`.

`ui-key-editor-draw` and `ui-key-editor-draw-data` draw the list, the draft with Undo, the keyboard, the mouse, the capture, clash and card, Find by key, the settling question, Share, Paste and Import, and the keys overlay at each class, the capture's More options and the keyboard as a list at Large, in the block font and again in the installed game's fonts, and check them against `tests/frames.txt`. The ISO and JIS keyboards are drawn at Large. The overlay is drawn at each class on an 800 by 480 room beside a 128-point column, with a latched QUEUE in its On line; its pictures are `keys-overlay-<class>.png`.

## State

`Editor` holds the host's `EditorInputs`: the base layout, the chosen layout, the mod's keys, the hacks that are on, the scheme, the keyboard map, the platform, the own draft and its helpers, SwitchAlt, Developer Mode, Hold to confirm's setting, the played mod's name and the installed layouts. A call changes the draft, and each change is a step Undo takes back. The host fills the inputs and writes `result()`.

`ScreenState` holds the hover, the press and the focus, the list, details and prompt-text scrolls, the search, which menu is open, the category, the filter, the chosen action, whether Compact's details page is showing, whether the editor was opened over a match, the capture, clash, Find by key, share, paste or import prompt, the clash question's marked outcome and whether it settles a draft clash, Find by key's chord, and whether the capture's and the details' More options are open. It also holds which view is showing, the keyboard's shape and whether it follows the system, the modifier layer, how far the compact keyboard is scrolled, the key the inspector shows, the key the arrows move, the action Find on keyboard outlines, whether the keys show as a list and how far it is scrolled, and whether buttons 6 to 8 are shown.

## Invariants

- A row shows the chords resolution gives for that action, from the base, the chosen layout, the mod's data and the draft.
- Changed means the draft names the action. Unbound means it has no chord. Clashes means `find_clashes` reports a pair. Latchable means the registry's flag.
- `can_save` stays false while a clash names an action the draft binds. A clash in the mod's data alone stays in the Clashes filter and leaves Save changes allowed. The module writes no file; the host writes `result()` when the player saves.
- Escape's fixed chord on `chat.cancel` and `ui.screen-cancel` stays. An action this version does not know is listed, refuses every change, and is written back.
- Developer rows are listed while Developer Mode is on.
- Number keys select groups is SwitchAlt. The summary's count of the player's own changes leaves it out.
- No binding changes before the player confirms it: a capture writes on its button, the clash question on its confirm button, and a key pressed on the keyboard or a row in Find by key only inspects or chooses.
- Every word a player reads comes from `text.hpp`.

## Limitations

Nothing here imports, shares or pastes a layout. The host opens the screen, chooses the keyboard's shape, feeds it nothing while a capture is waiting, asks the name questions, writes the layout and Interface Type, and reloads the bindings. A layout's name is asked by the host. The module reads no preference, no clock and does not pause a match. A mouse or wheel chord carries no keyboard modifiers: the screen's wheel and pointer do not say which keys are held. The screen's wheel is the vertical turn. The keyboard is drawn by the kit's `add_keyboard`, and the mouse by `add_mouse`.
