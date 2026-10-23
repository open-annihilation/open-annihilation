# data/brain-facts

`oa-data-brain-facts` holds the facts of one game as plain owned data: the
base game, the expansions that are on, the engine's version, the mode, the
difficulty, the mod and the mutators in apply order. A brain reads a copy.
The simulation does not own the text.

## Entry points

`oa/data/brain_facts.hpp` (namespace `oa::data::brain_facts`):

- `GameFacts` is one game. A default value is the base game `totala`, mod
  `ta-3.1c` named Total Annihilation 3.1c at version `3.1c`, medium
  difficulty, skirmish, and no expansion, engine text or mutator.
- `ModFacts` is the mod, or the base game. `profile` is a copy of the
  effective profile, or empty when the game plays 3.1c's rules with no
  profile in hand.
- `mod_facts_of` names that mod. A null profile and a profile whose id is
  `base-game` stay the base game's id, name and version. Any other profile
  keeps its own. A non-null profile is copied either way.

`SideFacts`, `MutatorFacts` and `OptionFacts` are the sides, the mutators
and one option. A toggle stores a boolean, a number stores a number, and a
choice stores its text.

## State

None. Callers pass a profile and get facts back. Nothing here is kept for
the rest of the process.

## Invariants

- Every text and list is owned. A copy handed to another thread does not
  point back at the caller.
- The base game's id, name and version do not follow a baseline profile
  that calls itself `base-game`.
- The facts are a snapshot. Nothing here watches a match or a setting.

## Tests

`data-brain-facts` checks `mod_facts_of` for a null profile, a profile whose
id is `base-game` with a different name and version, and a `ridge` profile
at version `28`. It also checks a default `GameFacts`.

## Limitations

No AI profile, no map and no units. The app fills a `GameFacts` when a
match is about to seat brains; this module does not decide the mode, the
difficulty or which expansions are on. Loading a brain and asking `fits`
lives with the simulation.
