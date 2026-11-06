# Enhanced Mode presets

`oa-data-enhanced-mode` resolves a built-in, versioned match preset over the
selected game's mod profile. Its header is `oa/data/enhanced_mode.hpp` and
its namespace is `oa::data::enhanced_mode`.

`off_version` is zero. Off calls the existing profile resolver without
adding or changing options, so an existing profile keeps its canonical
form and hashes. A profile that requires Enhanced Mode cannot run with
Off: resolution returns an error rather than enabling it silently.

`current_version` is one. Version 1 establishes the preset's identity as
the simulation-scoped `setup.enhanced-mode` rule. It leaves the released
movement, capacities and simulation behavior in place. Graphics settings
are independent and never enter this module. Later preset versions can
introduce enhanced behavior without changing what a recording of an
earlier version means.

`resolve_profile` keeps existing settings, overrides and match choices,
then applies the required version as a strict match choice. The resolved
rules must contain that version. An unsupported version or an incompatible
profile returns diagnostics and no resolution. `rules_version` reads the
effective version from a `MatchRules` record.

Resolve before allocating a match and retain its rules for the match's
lifetime. Save, replay and multiplayer callers must supply the version
recorded for that match rather than the player's default for a new game.

`data-enhanced-mode` checks Off identity, enabled identity and hashing,
preservation of selected mod rules and match choices, and refusals of
unsupported versions or rules that require Enhanced Mode while Off.
