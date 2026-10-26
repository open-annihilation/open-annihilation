# Computer player

The built-in computer player. It sorts a player's units into squads, runs
one task per squad, and chooses what a builder makes from the AI profile
and the match's strategic strengths. A seat that chose a brain does not
run this player. A seat that did not plays exactly as before.

## Entry points

`oa/sim/ai.hpp` (namespace `oa::sim::ai`):

- `ComputerPlayers` is one match's computer players: the profile text, the
  build-list runs, the plan flag and each type's list.
- `computer_players_configure` stores the profile text and the build-list
  runs. `computer_players_initialize` loads the types and the lists.
- `computer_profile_apply` applies `plan`, `weight` and `limit` lines.
  `computer_apply_weight` and `computer_apply_limit` apply one line.
- `computer_profile_lines` walks a profile with that same tokenizer and
  reports each line that has a word. It does not apply the line. The words
  are valid only while the visit runs. See the grammar below.
- `computer_player_tick_orders` runs one computer player's orders.
- `match_computer_players` returns the match's computer players, creating
  them on first use. `configure_match_computer_players` stores the profile,
  the runs and whether a campaign may build downloadable types.
  `prepare_match_computer_players` loads them before the first orders.
- `computer_write_report` writes the weights report.

## State

`ComputerPlayers` is owned by the match. The profile text and the build-list
runs are copies. The plan flag starts matched and is kept from one profile
pass to the next.

## Invariants

- The tokenizer is the one `computer_profile_apply` uses. A line ends at
  `\n`. Words are separated by space, tab, `\r`, `\n`, `\v` or `\f`. `#`
  ends the line, even inside a word. A line keeps at most 20 words, and
  those words share 126 bytes.
- A `plan` matches when a word after `plan` is present and either the first
  of those words is `any` or one of them is the difficulty keyword. A later
  `any` counts for nothing. Lines before the first `plan` apply.
- A weight's factor is `static_cast<float>(std::strtod)` of the third word,
  or 0 when that word is missing. A limit's count is
  `static_cast<int32_t>(std::strtol)` base 10. `-1` is unlimited.
- A builder's list comes from the configured runs: a count, then that many
  ids, from type 0. A type gets a list only when it is not type 0 and it is
  a builder. An id that is 0 or not below the type count is skipped. The
  list keeps `build_list_kept` plus one entries.
- Downloadable types are skipped when the campaign flag is set.
- A pick whose unit name holds `MAKENUKE` or `MAKEANTI` is a round for the
  builder's first weapon. The structures task queues it with `order_factory`,
  and so does the construction task, which places no site for it: the match
  stockpiles the round. In 3.1c the construction task places such a pick as a
  unit of the round's type, which has no MaxDamage to read its health by.

## Tests

`ai-computer-player` builds and scores with the fixture profiles.
`computer_profile_lines` is checked by `sim-brains-profile`, including one
pass over every installed `ai/*.txt` file.

## Limitations

The line walk does not apply weights or limits, and it does not repeat a
downloadable type's own lines. The built-in player does both. The strategic
strengths belong to the match: this module reads them and does not refresh
them.

