# Enhanced Mode presets

`oa-data-enhanced-mode` resolves a built-in, versioned match preset over the
selected game's mod profile. Its header is `oa/data/enhanced_mode.hpp` and
its namespace is `oa::data::enhanced_mode`.

`off_version` is zero. Off calls the existing profile resolver without
adding or changing options, so an existing profile keeps its canonical
form and hashes. A profile that requires Enhanced Mode cannot run with
Off: resolution returns an error rather than enabling it silently.

`capacity_version` is one. Version 1 records the simulation-scoped
`setup.enhanced-mode` rule and raises the unit ceiling to 10,000 per
participating player. The compact pool permits at most 32,767 units across
all participants, and the projectile pool holds 32,767 shots. Scanner and
jammer queries share a prepared spatial index within each contact-picture
pass. `flow_version` is two. Version 2 retains those
capacities and adds shared routing and local unit flow. Its version has a
different simulation hash. Version 1 remains supported with its canonical
profile and hashes unchanged. `regional_flow_version` is three. Version 3
selects regional shared-route construction; version 2 retains its original
routing policy and identity. `swept_ground_version` is four. Version 4
selects bounded obstacle checks along each ground step
when its matching simulation implementation is integrated. It retains the
earlier capacities and version-3 routing policy. `corridor_flow_version`
is five; `current_version` remains four pending large-battle validation.
Version 5 reserves a separate identity for
advisory corridor lanes. That version requires
the matching flow behavior to be integrated and validated before adoption;
version metadata alone does not provide it. Versions 1, 2, 3 and 4 keep
their original canonical profiles and hashes. Graphics settings are
independent and never enter this module.

A profile that writes `setup.enhanced-mode: true` still resolves to version
1 before match choices apply. The application's Enhanced Mode switch asks
for `current_version` explicitly for a new game; a save or recording asks
for the version it holds. Older profiles retain their canonical meaning,
and recorded matches retain their original movement rules.

`resolve_profile` keeps existing settings, overrides and match choices,
then applies the required version as a strict match choice. The resolved
rules must contain that version. An unsupported version or an incompatible
profile returns diagnostics and no resolution. `rules_version` reads the
effective version from a `MatchRules` record.

Resolve before allocating a match and retain its rules for the match's
lifetime. Save, replay and multiplayer callers must supply the version
recorded for that match rather than the player's default for a new game.

`data-enhanced-mode` checks pinned Off, version-1, version-2, version-3 and
version-4 canonical and simulation hashes, distinct version-5 identity,
preservation of selected mod rules and match choices, and refusals of
unsupported versions or rules that require Enhanced Mode while Off.
