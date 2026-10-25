# data/input-actions

`oa-data-input-actions` is the catalogue of every key, button and wheel turn
the game answers. Each action has one stable id. A layout keeps that id
when a letter moves, and a later change reads the table. Nothing in the
game reads it yet.

## Entry points

`oa/data/input_actions/actions.hpp` (namespace `oa::data::input_actions`):

- `actions` is every action, sorted by id.
- `find` returns the action of an id, or of an old id kept as an alias, or
  null.
- `contexts`, `conditions`, `families` and `hack_ids` are the tables an
  action's bits and names point into. A context's parent is an index into
  `contexts`, and `no_parent` is the root.
- `exclusive` is true when two condition masks require opposite sides of an
  exclusive pair.

An action's defaults are the text of its chords. The table does not parse
them.

## State

None. The tables are constant, generated from the registry.

## Invariants

- An id is never reused for a different action. A rename keeps the old id
  as an alias, and `find` answers to it.
- `src/actions.inc` is generated. Edit
  `registry/actions.yaml` and run `python3 tools/gen_input_actions.py`.
  `input-actions-sync` fails while the table differs from the registry, or
  the registry breaks a rule.
- A default stays the text the registry wrote. `gui.` and
  `select.category-` actions are made when a mod loads, from `families`,
  and are not rows of `actions`.

## Tests

`data-input-actions` reads the table back: the catalogue's actions, in
order, the rows a layout has to tell apart, the one fixed chord and the
exclusive conditions. `input-actions-sync` checks the generated table, and
`input-actions-sync-selftest` plants one break of each rule.

## Limitations

The C++ grammar that parses a chord is K04's. Nothing in the game reads
the table yet, and the words are not translated here.
