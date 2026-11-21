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
their original canonical profiles and hashes. `battle_flow_version` is six:
its rules retain patrol goals, bounded early AI retargeting, and short-grace
route queues. `distributed_battle_flow_version` is seven, retaining those
features while distributing repaired group destinations. Versions 6 and 7
remain explicit experimental choices. `following_flow_version` is eight:
it inherits version 7's static `fine_regions` routes, format-3 saved fields,
80-field and 8 Mi-cell limits, and native movement. Its separate identity
reserves an experimental local-following policy; metadata alone does not
implement that policy or establish better large-battle flow. The new-game
default remains 4. `uses_distributed_battle_flow` selects only the known
versions 7, 8, 10, 11 and 13, so an unsupported future version does not inherit them.
`cohort_target_version` is ten: it inherits version 8's movement and adds
AI strike-target distribution among spatial cohorts. Versions 9, 12 and 14
are unsupported. `cohort_retention_version` is eleven: it inherits version 10 and
retains compatible current cohort targets within the bounded weighted-cost
margin. It derives those prior targets from current owned attack heads and
adds no persistent state. `uses_following_battle_flow` selects only versions
8, 10, 11 and 13; `uses_cohort_target_distribution` selects only 10, 11 and 13.
`uses_cohort_target_retention` selects only 11.
`cardinal_queue_escape_version` is thirteen, an explicit experiment
inheriting version 10. `uses_cardinal_queue_escape` selects only 13. It
permits bounded cardinal adjacent-origin cell centres after a mature local
queue's existing chooser returns an unadjusted or in-origin result, only on
an existing retry with fresh moving neighbours. Existing real crossings
and the old result when no legal cardinal exit exists are preserved. It
adds no persistent state and does not inherit version 11's target retention.
A matched 20,000-unit comparison reduced aggregate stationary time and
increased mean progress, with mixed ARM rear progress; see the
[measured limits](../../../docs/enhanced-mode.md#experimental-cardinal-queue-escape-version-13).
`uses_enhanced_unit_rendering` selects versions 10, 11 and 13 for their shared
animated body detail and deferred geometry policies. The new-game default
remains version 4 while the explicit experiment is validated.
Graphics settings are
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

`data-enhanced-mode` checks pinned Off, version-1 through version-8 and
version-10 canonical and simulation hashes, distinct version-11 and
version-13 identities,
preservation of selected mod rules and match choices, and refusals of
unsupported versions or rules that require Enhanced Mode while Off.

## Features, sets and revisions

A feature is one named engine change (`Feature`): local traffic flow, one
of the three shared-route builders, cohort targets and so on, 27 in all.
`feature_table()` lists each one in id order with its snake_case name, its
kind (capacity, simulation or orders), the features it needs directly and
the features it cannot share a match with. Only the three route builders
exclude each other. A feature's id never changes and is never given to
another feature: changing what a shipped feature does means a new feature
with a new id.

Code asks only whether a feature is on: `FeatureSet::has` is one bit test,
cheap enough for every unit on every tick. Once the version checks are
converted, no code outside this module compares a version number.
`close_features` adds everything a set's
features need, transitively. `feature_set_problem` says why a set cannot be
played: an unknown feature, two features that exclude each other, a missing
need, or a set that is not empty yet lacks `capacity`, which sizes the
Enhanced Mode state every other feature uses. `feature_by_name` and
`feature_name` convert between features and their names.

A revision is a frozen, numbered set of features; `highest_revision` is the
highest this build plays, and `revision_features` gives each one's set.
Revision 1 is provisional until its features are decided: until then
it holds version 4's set, today's new-game preset.

During the restructure, `version_features` maps each old numbered preset
(Off, 1 to 8, 10, 11 and 13) to the features it switches on, and
`features(rules)` reads the set resolved rules play. The aliases reproduce
every check each old version made; they go when the version numbers leave
the engine. The test proves
each alias set and each revision complete and conflict-free, and pins the
whole feature table.
