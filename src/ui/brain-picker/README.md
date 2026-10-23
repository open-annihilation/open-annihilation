# Brain seat picker

The brain seat picker: which brain a computer seat plays, at which level, with which options, and the words each row, the details, the footer, the seat's label and its hover card show. Library `oa-ui-brain-picker`, headers `include/oa/ui/brain_picker/`, namespace `oa::ui::brain_picker`.

It reads no file, clock, preference or network. The host fills `Inputs` and lays the screen out for its frame. Every word a player reads comes from `text.hpp` through the Library's `TextHooks`, except a brain's own name, summary, description, level names, unfit reason and works-with line, which the host has already written. Option values use the mutator picker's `OptionSpec` and `number_values`. The screen draws the picker and takes its input. It does not host it, and it does not map a platform's keys or a pad.

## Entry points

- `open` starts from the seat's current choice. The choice stands when that brain is listed and can play. Its level is kept when the brain has it, and is otherwise `default_level` for the setup's difficulty. An option is kept when the brain has it and allows the value, and is otherwise the default; an option the choice does not name is the default too. A current brain that is not listed, or that cannot play, gives the built-in player and is named by id in `left_out`. The selection is the chosen row. The built-in player's level is the setup's difficulty while `built_in_level_fixed` (a skirmish or a campaign, where the Difficulty button owns every built-in seat). Otherwise it is the current level when that is `easy`, `medium` or `hard`, and the setup's difficulty when it is not.
- `select` moves the selection to any listed row. A row that can play becomes the choice: its level and options stay when it was already the choice, and otherwise the level is `default_level` for the setup's difficulty and the options are the defaults. The built-in player's fresh level is the setup's difficulty. A locked row is selected and shown, and the choice stays.
- `set_level` keeps a level the chosen brain has. It refuses every level of the built-in player while the Difficulty button owns that level. The built-in player's levels are `easy`, `medium` and `hard`.
- `set_option` keeps a choice, a toggle or a number when the value is one the option allows. A choice or a toggle is compared as written. A number must be one of `number_values`, the canonical text. The built-in player has no options.
- `set_every_seat` sets the offer to use this choice for every computer seat, when the picker offers it.
- `rows`, `details`, `summary` and `hint` are what the list draws. `result` is the choice, with every option of the chosen brain. `seat_label` and `seat_card` are what a screen shows for a seat that is not this picker: "Built-in · Hard", "Steady Commander · Firm", and "Steady Commander 1.2 · Firm" in the battle room. The card names the brain, its version (a brain only) and its level, and flags Full awareness.

The built-in player is always the first row, named "Built-in computer player". Its line is the chosen level, or "Built into Open Annihilation". The other brains follow by name folded to lower case. A row's line is the first that holds: why it cannot play, the chosen level's name, or the summary.

The details' facts are Works with, Full awareness and Network. Each is left out when there is nothing to say. Full awareness uses the Library's sentence, in the lock tone. A locked row's reason is the first fact, with no label. While the Difficulty button owns the built-in player's level, the details say "Set by the Difficulty button: {level}" and the levels are locked. A locked brain's levels are locked too. The screen's "Use for every computer seat" reads `words::use_every_seat`. The header's "Computer player" and "{seat} · {mod}" are `words::title_player` and `words::context_seat`; the host fills them from `seat_name` and `mod_name`.

## State

`Picker` holds the host's `Inputs`, the choice (brain, level and options), the selected row, whether the choice applies to every computer seat, and the id left out of the seat's current brain. An empty brain id is the built-in player. The host replaces the inputs by opening again.

`Inputs::built_in_level_names` is Easy, Medium and Hard as the game names them. The model does not look those three up. `Inputs::offer_every_seat` is false where a screen has one computer seat.

## Invariants

- The choice is the built-in player, or a listed brain whose `unfit` is empty. A locked row can be selected. It is not the choice.
- The choice's level is one the brain has. For the built-in player it is `easy`, `medium` or `hard`, and it is the setup's difficulty while `built_in_level_fixed`.
- The choice's options are the chosen brain's, in its order. Each value is one that option allows: a choice's value, a toggle's two, or a number from `number_values`. A number is that canonical text.
- A fresh brain, and a brain selected when it was not already the choice, takes `default_level` for the setup's difficulty and the default of each option.
- `every_seat` is set only when the picker offers it.
- No word is looked up except through `TextHooks`. A brain's own words are the host's.

## Screen

`window_size` is the size class's dialog: 480×324 at Compact, 720×486 at Regular and 960×600 at Large, and no larger than the frame. `picker_layout` lays that window out from the model. `draw_picker` paints the kit's window face, the display list and the kit's window edge. `details_page_layout` is Compact's details page; `picker_layout` returns it while `details_page` is set at Compact.

From the top: the kit header, titled "Computer player", with `{seat} · {mod}` (the mod's name, or "No Mod") and the close mark. The hint, when `hint` is not empty, sits under the header in `colour::lock`, wrapped to two lines at Regular and Large and cut to one at Compact. It is also drawn on Compact's details page. The list is 300 points wide at Regular, 360 at Large, and the window's width less the padding at Compact. Rows are 32, 34 and 36 points. Each row has a 20-point badge (the built-in player's letters when it has no picture), the name, one line in the line's tone colour, the "Full awareness" chip at Regular and Large, and a circled-check chip on the chosen row. That chip is not a control. A locked row is under the kit's locked fade. The selected row uses the kit's selected look. A scroll bar shows while the list holds more than it shows. Under the built-in row, when it is the only one, the list says "No brains here yet. The Library on the main menu gets them."

The details sit beside the list at Regular and Large, and on their own page at Compact. They show a 40-point badge, the name, `version_author`, the description (at most six lines, three at Compact), "Level" over a level strip or the Difficulty note in its place, each option as a kit control, the facts, and "Use for every computer seat" when the picker offers it. A toggle is a switch, a choice of at most four values is a level strip, a longer choice is a drop-down, and a number is a slider with its stops. Those controls are locked unless the row is the choice. A locked row's level strip is locked too. A fact in the lock tone is drawn in `colour::lock`. A scroll bar shows while the details hold more than they show.

The footer shows the summary in `colour::hint`, CANCEL and USE THIS. Compact's list adds DETAILS. Compact's details page has BACK and USE THIS. A button is as wide as its words.

## Names

Journeys press `oa.` and the name. `brains.list` is the list. `brains.row.<id>` is a row, and the built-in player's is `brains.row.built-in`; its text is the name. `brains.level` is the level strip; its parts are the level ids. `brains.option.<option-id>` is an option. `brains.every-seat` is the switch, parts `off` and `on`. `brains.details`, `brains.back`, `brains.cancel`, `brains.use` and `brains.close` are the buttons and the close mark. `brains.hint`, `brains.summary` and `brains.name` are labels: they take no press and no focus, and their text is the words shown. `brains.name` is the details' name, so a text-only pane still has a clip the scroll can read. `brains.list.bar` and `brains.details.bar` are the scroll bars. They take no focus.

## Keys

Tab's order at Regular and Large is the list, the level strip, the option controls, the every-seat switch, CANCEL, USE THIS and the close mark. At Compact it is the list, DETAILS, CANCEL, USE THIS and the close mark. On the details page it is the level strip, the option controls, the switch, BACK, USE THIS and the close mark. The close mark stays on that page so a pointer press on it is a key press too. Labels and scroll bars are not in the order. A details control that is clipped out of view is scrolled into view when it takes the focus.

Up and Down move the selection and keep the focus on the list, and they stay there at its ends. Page Up and Page Down move by the rows the list shows. Home and End go to its ends. On the details, those four keys scroll the details. Left and Right step a level strip, a choice strip or a slider, and set the every-seat switch. Space flips the switch or a toggle, and opens a drop-down. Enter opens Compact's details page, moves the focus to the details' first control at Regular and Large, and on USE THIS answers `use`. Escape closes an open drop-down, else leaves the details page, else answers `cancel`. An open drop-down takes the arrows, Home, End, Page Up, Page Down, Enter, Space and Escape first.

`picker_pointer`, `picker_key`, `picker_wheel`, `picker_command` and `picker_stick` are the input. A press or a tap on a row calls `select`. A level, an option or the switch changes through `set_level`, `set_option` and `set_every_seat`. The screen decides no rule of its own. A slider's knob is held and dragged. A drop-down is opened and an item chosen; a press outside it only closes it. A finger that moves past its reach in the list or the details scrolls that pane and presses nothing on lift. The wheel scrolls the pane under the pointer, or the open drop-down. `picker_stick` scrolls the pane that holds the focus. `back` does what Escape does. `use` answers `use`. The host maps the pad: the D-pad and the left stick are the arrows, A is Space, B is `back`, Menu is `use`, L1 and R1 are Shift+Tab and Tab, and the right stick is `picker_stick`.

## Tests

`ui-brain-picker-model` opens on the built-in player, on Steady Commander at `firm` with `aggression` high, on a level it lacks and on an option it lacks or refuses, and on Ridge Raider, Tide Keeper and a brain that is not listed. It selects each row, including a locked row that is shown and not chosen, and checks `default_level` for each setup difficulty. It sets a level the brain has and one it lacks, and the built-in player's level with and without the Difficulty button. It sets a choice, a toggle and a number, valid and refused, and "Use for every computer seat" offered and not. It checks the rows' order, lines, tones and locks, the details of each kind of row, facts left out when there is nothing to say, the summary with and without every seat, `seat_label` in both forms, `seat_card` with and without Full awareness, and a lookup that translates.

`ui-brain-picker-layout` lays the screen out at Compact, Regular and Large in the windows 640×480 (1×), 1920×1080 (2×) and 2560×1440 (2×). Every part lies inside the window. No two controls meet, apart from a row and the list. Every row is a list item, the chosen row carries the check mark, and locked rows are locked. The level strip's parts are the level ids. Each option is a control of its form. Compact has no details pane, and DETAILS opens the page, which has BACK. Tab follows the order above. The names are sound and unique. Every text fits, and no button is cut, in a French lookup and a long German one.

`ui-brain-picker-draw-data` draws Compact, Regular, Large, Compact's details page, the built-in player with the Difficulty note at Regular, and Compact in French. Each picture is drawn twice, byte for byte the same, with nothing outside the window. `oa-ui-brain-picker-draw-test --data --frames DIR` writes `brain-picker-compact.png`, `brain-picker-regular.png`, `brain-picker-large.png`, `brain-picker-details-compact.png`, `brain-picker-built-in-regular.png` and `brain-picker-fr-compact.png`.

`ui-brain-picker-input` clicks each row, a locked row, each level, each option and the switch, at each class. A finger's tap selects. A finger's drag on the list scrolls and selects nothing. It drives every key above, each command and the wheel. After every input the choice still fits the model.

## Limitations

The screen does not store the seat, and it does not map SDL keys or a pad. The host writes each brain's `unfit` words and its works-with line, and the three names of the built-in player's levels. Tags stay on the brain; the screen does not list them. A number's value has to be the canonical text `number_values` returns. The model sees one version of each brain, the one the host gives. Two brains with the same id: the first is the one a choice finds, and both rows are shown. A number with fewer than two stops is shown locked. The hint is shown on Compact's details page as well as on the list.
