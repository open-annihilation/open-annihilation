# Brain seat picker

The brain seat picker: which brain a computer seat plays, at which level, with which options, and the words each row, the details, the footer, the seat's label and its hover card show. Library `oa-ui-brain-picker`, headers `include/oa/ui/brain_picker/`, namespace `oa::ui::brain_picker`.

It reads no file, clock, preference or network. The host fills `Inputs`. Every word a player reads comes from `text.hpp` through the Library's `TextHooks`, except a brain's own name, summary, description, level names, unfit reason and works-with line, which the host has already written. Option values use the mutator picker's `OptionSpec` and `number_values`. Nothing here draws the picker or hosts it.

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

## Tests

`ui-brain-picker-model` opens on the built-in player, on Steady Commander at `firm` with `aggression` high, on a level it lacks and on an option it lacks or refuses, and on Ridge Raider, Tide Keeper and a brain that is not listed. It selects each row, including a locked row that is shown and not chosen, and checks `default_level` for each setup difficulty. It sets a level the brain has and one it lacks, and the built-in player's level with and without the Difficulty button. It sets a choice, a toggle and a number, valid and refused, and "Use for every computer seat" offered and not. It checks the rows' order, lines, tones and locks, the details of each kind of row, facts left out when there is nothing to say, the summary with and without every seat, `seat_label` in both forms, `seat_card` with and without Full awareness, and a lookup that translates.

## Limitations

Nothing here draws, takes input, or stores the seat. The host writes each brain's `unfit` words and its works-with line, and the three names of the built-in player's levels. Tags stay on the brain for the screen, which uses the Library's `brain_tag_label`; this model does not list them. A number's value has to be the canonical text `number_values` returns. The model sees one version of each brain, the one the host gives. Two brains with the same id: the first is the one a choice finds, and both rows are shown.
