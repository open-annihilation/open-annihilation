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
versions 7, 8, 10 and 11, so an unsupported future version does not inherit them.
`cohort_target_version` is ten: it inherits version 8's movement and adds
AI strike-target distribution among spatial cohorts. Version 9 is not
supported. `cohort_retention_version` is eleven: it inherits version 10 and
retains compatible current cohort targets within the bounded weighted-cost
margin. It derives those prior targets from current owned attack heads and
adds no persistent state. `uses_following_battle_flow` selects only versions
8, 10 and 11; `uses_cohort_target_distribution` selects only 10 and 11.
`uses_cohort_target_retention` selects only 11.
`uses_enhanced_unit_rendering` selects versions 10 and 11 for their shared
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

`data-enhanced-mode` checks pinned Off and version-1 through version-7
canonical and simulation hashes, distinct version-8, version-10 and version-11 identities,
preservation of selected mod rules and match choices, and refusals of
unsupported versions or rules that require Enhanced Mode while Off.
