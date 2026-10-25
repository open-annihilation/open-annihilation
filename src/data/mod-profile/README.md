# Mod profiles

A mod profile (`oamod.yaml`) says how a mod differs from 3.1c: identity,
layout, limits, unit-script extensions, data keys, standard hacks with
parameters, and visual strings and media. This module resolves one against
the registry into the records the engine reads, with its canonical form and
hashes. Target `oa-data-mod-profile`, header `oa/data/mod_profile.hpp`,
namespace `oa::data::mod_profile`. It takes bytes and plain values and never
opens a file; `src/app/mod_profile_loader.cpp` does the file work.
`supportedGames` and `requiredExpansions` are package facts: written, they
enter the full hash only, and an omitted list is not filled into the
canonical form. Omitting `supportedGames` means `totala`.

## The registry

`registry/hack-registry.yaml` is the source of truth: every meaning a
profile can name, each parameter's type and bounds, its 3.1c baseline, its
default once the entry is on, who may adjust it (`fixed`, `install`,
`match`), whether it is part of the network-play hash (`sim` or `view`), the
setting it binds to, presets, constraints, and for each hack whether this
engine implements it (`implemented`). The file's header describes every
field.

`tools/gen_mod_registry.py` checks the registry's consistency (a hack's
area is the first word of its id, among others) and writes what the engine
compiles from it:

- `src/registry_table.inc`: the registry as constant tables, read through
  `oa/data/mod_profile/registry.hpp`, each entry with its area and summary,
  each hack with its title (the name players see, such as "Deterministic
  Wind"), each area of the hacks with its title (`area_title`), and each
  parameter with its unit;
- the records: `MatchRules` in
  [src/data/match-rules](../match-rules/README.md), and `Identity`, `Layout`,
  `Strings`, `Media`, `DataKeyBindings`, `NetworkRules`, `RecorderRules` and
  `UiRules` here (`include/oa/data/mod_profile/records.inc`);
- `visit_bindings` (`include/oa/data/mod_profile/bindings.inc`), which names
  every meaning with the one field that holds it. A limit's parameters are
  held by the engine's own `Limits` record ([src/data/limits](../limits/README.md)),
  which `ModProfile::limits` is; the generator's `LIMIT_FIELDS` names the
  field of each.

Change the registry, then run `python3 tools/gen_mod_registry.py`; the
`mod-registry-sync` test fails while a generated file differs from what the
registry gives. A string the registry writes with byte escapes stays escaped
in the generated files.

## Resolving

`resolve_profile` reads a profile, checks it block by block, resolves each
parameter in the registry's order (baseline while the entry is absent or
false, default once on, preset, the profile's value, the player's settings
for parameters the profile binds, the player's overrides, then the host's
match options) and returns a
`Resolution`: the `ModProfile` records, the effective profile as a `Value`
tree, its sim-scope part, its canonical JSON (RFC 8785) and the provenance
of every value. A profile writes a limit or hack as `true` (defaults), a
mapping (an optional `preset` and parameters), a scalar for an entry with a
shorthand parameter, or `false` (off, the same as leaving it out).

`ResolveOptions` carries the player's settings (`Settings`: INI Section/Key
values and registry values, matched without case, the profile's registry
seeds filling the gaps), the player's `overrides`, then the host's
`MatchOption`s, and `accept_unimplemented_hacks`, which turns the refusal of
a hack this engine does not implement yet into a warning, for development.
`match_option_problem` runs the registry-only checks of one match option
without a profile: a known parameter, adjustable per match, and a value of
the right type inside its bounds. It returns the message resolving that
option would report, or nothing when the registry accepts it.


Every refusal is a `Diagnostic` naming the profile, the line and column, the
offending path and what is wrong: a missing `author` or `packaging` block
or one of their required values, a malformed e-mail address or a date that
is no calendar day; an unknown key, hack, parameter, preset,
script extension or data key; a value of the wrong type, out of range, not
a multiple, too long, not matching its pattern, repeated or not ascending;
an index mounted twice, inside 3.1c's own range or holding an extension of
the other direction; one extension mounted twice; a fixed parameter bound
to a setting; a constraint broken; a data key whose hack is off; a hack not
implemented yet. A setting that does not read as its type is ignored with a
warning, and one out of bounds is clamped with a warning, as the original
game treats its INI and registry.

The full hash is the SHA-256 of the canonical form; the sim hash, the one
every machine of a network game must share, that of the sim-scope part,
which leaves out the id, name, version, author, packaging, settings and
every `view` value. `ModProfile::author` and `ModProfile::packaging` hold
who made the mod and who packaged it, when and in which revision.
`describe_resolution` writes the effective profile and both hashes, as
`--print-profile` prints them. `limits.weapon-types` at its baseline of 256
weapon types and `limits.sides` at its baseline of five sides are left out
of the canonical form, so a profile that writes either at that baseline
keeps both hashes of a profile that never wrote it. `--print-profile` still
prints the effective profile, which holds both limits.


`baseline_profile_text` writes the profile that turns on every limit and
hack able to play as 3.1c while on, each at its `baseline` preset. Played,
it must give exactly what no profile gives; `match-determinism` and the
saved-game check hold every rule to that.

## Overrides

`overrides` are the player's own changes to the profile's standard hacks
(Developer Mode in the settings dialog), `HackOverride`s of
`oa/data/mod_profile/overrides.hpp`: each turns one hack on or off and sets
the parameters it names while on. The resolver lays them over the profile
after the profile and the player's settings, and before the host's match
options: on gives the values the profile resolved for the hack, or
its defaults when the profile has it off, with each parameter the override
names in its place; off leaves the hack out, as `false` in a profile does.
Each is checked as a profile's hack is: the hack and its parameters known,
every value fitting its parameter (`check_value`), the hack's constraints
kept, the hack implemented to be turned on, and off only while no data key
the profile maps needs it. One that fails is left out whole, with a
warning ending "the override is left out", and the profile resolves
without it. The overrides go into the effective profile, so they change
its canonical form and both hashes exactly as the same values written in
the profile would; a save made with an override of a rule (sim-scope)
hack therefore loads only under the same effective profile.

`overrides.hpp` also gives the states of the standard hacks
(`standard_hacks`, `hack_states` of an effective profile,
`base_hack_states`, `overridden_state`, which lays an override over a state
as the resolver lays one that fits), the text the settings keep the
overrides as (`overrides_text`, one JSON object of each hack's id to
`false`, `true` or its parameters, and `read_overrides`), and
`base_game_profile_text`, the profile that changes nothing, which a game
without a mod resolves to lay its overrides over 3.1c. The settings keep
that game's overrides under `base_game_id`, `ta-3.1c`.

## Tests

- `data-mod-profile`: every way of writing an entry, settings with seeds,
  clamping and the unit-type step, match options, mounts, data keys, the
  records filled, a profile that changes nothing giving 3.1c's records, every
  refusal with its path and position, the canonical form, and hashes pinned
  to what the reference resolver gives for the same profiles; and the
  overrides: a hack turned on over a profile and over the plain baseline,
  turned off, its parameters set, each kind left out with its warning
  while the rest apply, the hashes they change and the ones they keep,
  their text read back, and the hacks' states;
- `data-mod-profile-bindings`: every registry meaning bound to exactly one
  field, every field able to hold what its parameter allows and starting at
  its baseline, and every baseline, default and preset passing the
  resolver's own checks;
- `data-mod-profile-references`: every profile in the folders under the
  folder `OA_MOD_PROFILES_DIR` names resolves to its pinned hashes (accepting
  hacks not implemented yet); skipped without the variable.

## Limitations

The game reads the profile's `Identity`, `Layout` and limits (the archives
discovery mounts, the directories and files every loader reads, the display
version, the side names, the unit build version) and fills `Settings` from
the mod's INI and the preferences that stand in for its registry
([src/app](../../app/README.md#mod-profile-and-mod-folders)). Every match
plays by its rules (`MatchRules`, src/data/match-rules): its unit-script
extensions (`MatchRules::script_get`, read by src/sim/unit-script) and the
records its data keys give unit types and weapons (read from their files by
`oa/data/defs/rule_keys.hpp`). A hack this build does not implement makes a
profile that turns it on refused unless unimplemented hacks are accepted.
