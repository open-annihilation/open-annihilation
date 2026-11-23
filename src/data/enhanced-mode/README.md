# Enhanced Mode

`oa-data-enhanced-mode` resolves Enhanced Mode over the selected game's mod
profile: a frozen, numbered revision of named engine features, or Off. Its
header is `oa/data/enhanced_mode.hpp` and its namespace is
`oa::data::enhanced_mode`.

Off is revision 0 and the empty set. Off calls the existing profile resolver
without adding or changing options, so an existing profile keeps its
canonical form and hashes. A profile that requires Enhanced Mode cannot run
with Off: resolution returns an error rather than enabling it silently.

An enabled match records the simulation-scoped `setup.enhanced-mode` rule:
its revision and its features. `capacity`, the feature every other one
builds on, raises the unit ceiling to 10,000 per participating player; the
compact pool permits at most 32,767 units across all participants, and the
projectile pool holds 32,767 shots. Scanner and jammer queries share a
prepared spatial index within each contact-picture pass. Graphics settings
are independent and never enter this module.

A profile that writes `setup.enhanced-mode: true` resolves to revision 1
with `capacity` alone before match choices apply. The application's Enhanced
Mode switch asks for Revision 1 explicitly for a new game; a save or
recording asks for the revision and features it holds.

`resolve_profile` keeps existing settings, overrides and match choices,
then applies the required `Choice` as strict match choices. The resolved
rules must hold that revision and those features. An unknown revision, a
set that cannot be played or an incompatible profile returns diagnostics
and no resolution. `features` reads the set resolved rules play, and
`store_choice` writes a choice into a rules record the way resolution does,
for code that builds rules by hand.

Resolve before allocating a match and retain its rules for the match's
lifetime. Save, replay and multiplayer callers must supply the choice
recorded for that match rather than the player's default for a new game.

`data-enhanced-mode` pins the simulation and canonical hashes of Off, of
`capacity` alone and of Revision 1, and Revision 1's canonical object. It
also checks preservation of selected mod rules and match choices, refusals
of unknown revisions, unplayable sets and rules that require Enhanced Mode
while Off, and the whole feature table.

## Features, sets and revisions

A feature is one named engine change (`Feature`): local traffic flow, the
fine regional route builder, paced cohort targets and so on. `feature_table()`
lists each one in id order with its snake_case name, its kind (capacity,
simulation or orders), the features it needs directly and the features it
cannot share a match with. A feature's id never changes and is never given
to another feature: changing what a shipped feature does means a new
feature with a new id.

Code asks only whether a feature is on: `FeatureSet::has` is one bit test,
cheap enough for every unit on every tick. No code outside this module
compares a revision number. `close_features` adds everything a set's
features need, transitively. `feature_set_problem` says why a set cannot be
played: an unknown feature, two features that exclude each other, a missing
need, or a set that is not empty yet lacks `capacity`, which sizes the
Enhanced Mode state every other feature uses. `feature_by_name` and
`feature_name` convert between features and their names.

A revision is a frozen, numbered set of features; `highest_revision` is the
highest this build plays, and `revision_features` gives each one's set.
Revision 1 holds seventeen features: `capacity`, `local_flow`,
`routes_fine_regions`, `route_corner_advance`, `ai_strike_retarget`,
`credited_fallback_search`, `ai_attack_retention`, `ai_rally_retention`,
`occupancy_free_planning`, `steering_projection_reuse`,
`exact_indexed_searches`, `wide_strike_recruits`, `paced_cohort_targets`,
`dispatch_cohort_retention`, `counterflow_queue_escape`, `idle_goal_skip`
and `static_interior_refresh_skip`. A new game with Enhanced Mode on plays
it. The match stores a `Choice`: the revision and the feature set.
`ai_strike_retarget` needs only `capacity`; the retarget table keeps its own
count, so it does not need local flow.

The test proves each revision complete and conflict-free, and pins the
whole feature table.
