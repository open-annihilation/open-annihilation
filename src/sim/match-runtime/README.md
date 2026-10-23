# Model, script and spawn runtime

This component composes `unit-spawn`, `model-runtime`, `script-vm` and asset
ownership into a running match. `SpawnBridge` creates owned model pieces and a
COB interpreter for each unit, binds them before running `Create`, and keeps
the script across ticks. Its `SpawnSubsystems`, effects and unit-value
services are mandatory interfaces for engine operations that live elsewhere;
none of them has a default that reports success.

## Unit scripts

`ScriptInstance` looks script names up case-sensitively. A named call with
`immediate` set runs a whole scheduler step with elapsed zero right after the
start. `query` allocates one context with four arguments, runs just that
context and copies local slots 0..3 back; a RETURN operand is not the query
result, and a query that sleeps stays scheduled. A missing script or an
exhausted context pool leaves the query arguments untouched. `UnitInstance`
uses these calls for QueryPrimary/Secondary/Tertiary,
AimFromPrimary/Secondary/Tertiary, the fallback piece choice and the
asynchronous SetMaxReloadTime.

`call_no_arguments` keeps the context's old locals and never calls the
failure callback. `call` writes all four slots, zeroing omitted trailing
values, before it sets the argument count, and its failure callback receives
zero. Create uses the no-argument form.

`UnitHost` maps model-piece operations to the unit's own instance. Unit-value
GET/SET covers selectors 1..20 with their flag changes. Geometry, activation,
yard-map and effect operations remain required services. Two piece operations
do nothing, as in 3.1c. A piece position asked for outside the model is the
unit's position.

`SharedRandom` seeds the match stream and draws bounded values, including the
signed bound rejection and the unsigned division of 3.1c. Every unit script
draws from this one stream; do not give a script a private stream. The SLEEP
clock scale is supplied by the owning application; it is not derived from the
simulation frequency.

Script allocation stages a shared COB asset before the interpreter is built
with its model, and no script runs before the bind and Create order. The
bridge rejects a COB asset that does not match its model at bind. Asset
tables, the slot pool and caller services must outlive the bridge.
`UnitInstance` cannot move, because its interpreter and host keep references
to its model and world slot.

## The match

`Match` owns the slot pool and the unit instances, weapon slots,
squads, spatial occupancy and buckets, sight grids, movement maps, projectile
pool, effect world and map features. Its inputs carry the resolved runtime
metadata and map state explicitly. `MapPlot.metal` comes from the map's metal
setting and feature overlays; it is not TNT padding. Sight dimensions and
masks are explicit loader inputs. Ground movement uses `GroundRuntime`;
direction and distance come from `game-math`. A match built to resume a
savegame (`OfflineInputs::resuming_saved_game`) places only the map's
blocking markers; the save's Features section then places its features
through `place_saved_feature`, `ignite_saved_feature` and
`restart_saved_feature_sequence`, after `adopt_feature_defs` has taken any
type the save loads. Its plots' metal still starts with the metal of the
map's indestructible features, which it does not place, until the save's
Metal section replaces it. A limitation: a save whose Metal section is
missing or not one byte per cell keeps that metal, where the game keeps the
map's metal without it.

Unit creation shares nothing while the run flag (`Game.session_flags` bit 0)
is clear; with it set the match needs its `multiplayer` hooks or it stops.
`OfflineServices` supplies command and activation sounds, attachment and UI
callbacks, effects and map-update operations. A unit whose order speaks with
its own caption, as a transport's "Unit is too heavy to transport" or a
builder's "Starting construction", reaches the application through
`SpeechHooks` (`Match::set_speech_hooks`); without them the speech plays its
category through `command_sound` and the category's own caption.

A unit whose type is a feature, such as dragon's teeth or a fortification
wall, stops being a unit when its build finishes. The machine that simulates
it marks it to die at once, dismissed, and the other players take that death.
The feature named by its corpse is placed on the same footprint, at that
feature's own health and with the builder's colour. It blocks movement and
can be reclaimed for the feature's metal, and it can be destroyed, but it
cannot be selected, ordered or counted. The builder's order still ends with
"Building complete". A wall created already finished is marked the same way
as it is placed.

## Rules

A mod's profile reaches the match through `OfflineInputs`: `limits` (the
capacities), `rules` (`MatchRules`, one record per hack), `unit_type_rules`
and `weapon_rules` (what the data keys give single types and weapons) and
`profile_sim_hash`. The defaults are 3.1c's and no profile. The match keeps
copies of all of them; `Match::rules()` returns the match-wide rules and
`Match::rules_view()` a `MatchRulesView` over its copies, which it hands to
the systems that apply them: its tick host (as the simulation-state and
ground-order host) and the adapters, which hold the match itself; the
weapon, health and projectile damage hosts (`Host::rules`); the retaliation
hooks; the unit-value handlers (`UnitValueHost::value_rules`,
`value_limits`); and the computer players (`ComputerPlayers::rules`). Where
each rule is read is listed in
[src/data/match-rules](../../data/match-rules/README.md#where-each-rule-is-read).

A profile's display rules reach the match as `OfflineInputs::display`
(`DisplayRules`, `Match::display_rules()`), which `Match::set_display_rules`
replaces while the match runs as the player's Developer Mode changes them,
from the next tick: what a feature reclaim, an
aircraft reclaiming a unit and a failed landing say, and whether an
endsmoke weapon's burst also shows its explosion and an explosion raises a
smoke column (`EffectWorld::explosion_smoke_column`). They change only what
the player hears and sees, with the order stages and effect emitters that
go with it: a feature reclaim that speaks once moves to its silent stage
with the first step, and so reaches its end one step sooner; an aircraft
that waits to speak until it arrives reclaims in a stage 3 of its own.
`match-missions-ground`, `match-missions-vtol-build`, `match-missions-vtol`,
`match-effect-hooks` and `effect-explosions` check each.

State a rule keeps that 3.1c's records have no room for (the tick a slot
may be reused at, a building's facing) lives in a table of the rule's own
module, allocated while the match is built, never in `Unit`, `Game` or
`Player`. The rule describes it as a `RuleStateTable`
(`oa/sim/match_runtime/rule_state.hpp`): a name, its bytes (fixed-width
integers, no padding) and how to restore them, and adds it with
`add_rule_state(match.rule_state(), table)` while the match is built, and
only when its parameters need the state: at its baseline a rule adds none.
`Match::fold_rule_state` folds the profile's sim hash and every table into
the match-state digest (`src/sim/state-hash`), and `match_tick_digest` and
the trace recorder fold the same into the trace's total; a save keeps each
table as a blob of its ModProfile account. A mutator digest the match holds
is folded too, even when no table was added. A match that holds one also
steers its ground units faster than any stock unit held to their routes
(`TickHost::fast_unit_steering`, `ground_orders::FastUnitSteering`); any
other match steers every unit as 3.1c does. With no table and no mutator
digest nothing more is digested or saved, so a match without a profile, or
with every rule at its baseline, digests as before: `match-determinism` plays
its skirmish under the profile that turns every limit and hack on at its baseline preset and
holds it to the same pins. `match-rules` checks the copies, the view and the
rule-state digests. Under `repair.rate: exact-remainder` the match keeps
each unit slot's heal remainders (`Match::repair_remainders`) as the table
`repair-remainders`; `match-veterancy-repair` plays the veterancy and repair
rules in a match.

The `orders.*` and `air.*` rules act in the mission handlers: which weapon
slots Attack_Chase and Suppress occupy and which busy states free a
builder's weapons (`GroundMissions::occupy_weapons_while_busy`), the repair
pad's activity test in SelfRepair, the blocked-site retries of MobileBuild
and VTOL_MobileBuild, the builder patrol and guard choices
(`ground::patrol_choice`, `ground::guard_choice`), the aircraft that never
break off for a repair pad (`never_retreats_to_repair`) and the air guard
that keeps Hold Position. A blocked builder under orders.build-site-kickout
sends the local player's own mobile units off its footprint
(`GroundMissions::clear_build_site`, `tick_missions_kickout.cpp`); the spot
it last sent each unit to is the rule's state table, `build-site-kickout`,
eight bytes per unit slot, added only when the rule moves units.
`Match::send_ahead_of_orders` sends one of the local player's units the
same way to a point the player drags it to (ui.build-tools), and the build
cursor's site test reports a site it lets through over the player's own
units (`BuildSiteOptions::over_own_units`).
`Match::relocate_queued_build` moves a queued MobileBuild or
VTOL_MobileBuild that has not spawned its building onto another site; when
that order is the one the builder is walking or flying to, the approach
starts again at the new site.
`match-order-rules` checks each rule and parameter against 3.1c's behaviour.

The unit rules (`unit_rules.cpp`) keep such a table for the slot reuse delay
(units.id-reuse-delay), handing its ticks to unit creation. A building's
facing (units.build-rotation) needs none: it lives in the unit's heading, as
it does on the wire and in saves, and `Match::unit_build_facing` reads it
back. The match turns each rotatable type's yard once, and its occupancy,
building sites (`BuildSiteOptions`), construction orders
(`Match::issue_mobile_build`, `Match::set_build_facing`), resurrection and transfers place the building
facing that way. `match-unit-rules` plays each unit rule.

The structure gift rate limit (`sharing.structure-gift-rate-limit`) is one
such rule: the share panel's gifts go through `Match::begin_share_gift`,
`share_gift_unit` and `end_share_gift`, which hold back a batch of structures
when too many went across within the window, and the tick hands over the
batches that are due last (`give_due_structure_gifts`). Its window and
waiting batches are the table `structure-gifts`
(`oa/sim/match_runtime/structure_gifts.hpp`); `match-structure-gifts` checks
them.

## Seat hooks

`Match::seat_hooks` (`SeatHooks`, `seat_hooks.hpp`) is one computer seat's
turn when something other than the built-in computer player plays it.
`play` is called at that seat's controller point and returns true when it
played the tick. A null `play`, or one that returns false, leaves the
built-in order tick to run. The weapon sweep runs either way. Hooks set on
a seat that is not a computer are never called. The hooks and the brains'
state (`Match::seat_state`) are not saved, hashed or shared, and the state
ends with the match.

## Event hooks

`Match::event_hooks` (`EventHooks`, `event_hooks.hpp`) reports what happens
in a match to a reader such as a director, whoever simulates the unit: this
machine, another player's, or a recording the match replays. Each entry is
null by default and is called only at the points every such event passes
through:

- `unit_created` at the end of `create` and `start_player`; a building
  (bmcode 0) created finished is then reported finished by itself.
- `unit_created_by` right after `unit_created` for that same unit, and before
  `unit_finished` when a building created finished is reported finished. The
  builder is the unit whose work started it: the mobile builder of a
  nanoframe, the factory of a factory's frame, the resurrecting unit of a
  raised one, or the aircraft of a building built from the air. It is 0 when
  no unit made it: a commander, a mission's unit, a hand-over's copy, or a
  copy of another machine's unit.
- `unit_finished` from the builder link (`link_built_unit`), which build
  progress reaching the end, a factory's cancelled frame and `finish_unit`
  all take; from the resurrect order, by the unit that raised it; and for a
  building created finished. Each unit is reported finished once from its
  creation on, whichever comes first, so a copy another machine's state
  record already marked finished is still reported when the builder link
  arrives, and a building created finished is not reported again.
- `shot_placed` from `place_shot` (weapon), the burst copy in
  `update_projectiles` (burst) and `launch_meteor` (meteor, no aim). A shot
  `apply_shot` launches reports the aim and target unit it was given, which
  the record of a ballistic or dropped shot does not keep; a burst copy of
  such a shot reports no aim.
- `shot_detonated` at the top of every `detonate`, the stand-in blast of an
  exploding unit included.
- `unit_damaged` in `apply_damage_event` once the unit is known live and not
  dying, before the event changes it.
- `unit_given` once from `transfer_unit` for a capture, a share-panel gift or
  a structure gift, while the old unit is still live. When this machine makes
  the copy, the copy is reported created and then created by builder 0, and
  `unit_given` follows with the old unit and the copy, before this machine
  kills the old unit. A unit another machine handed over is left alive here
  for that machine to kill. When the receiver's machine makes the copy, the
  report names no copy and this machine reports no creation, and the old
  unit's `unit_died` follows once the sweep tears it down. A transfer that
  changes nothing (the unit's own owner, a unit already dying, or a player
  who is not present) is not reported.
- `unit_died` as `teardown_dead_unit` starts on a live unit, before its
  type, owner and position are cleared: each death once, settled here or
  elsewhere.
- `unit_idle` when a finished unit simulated here runs out of orders and is
  given its default mission (`TickHost::queue_default_mission`). A unit
  without a movement object, and a default mission the match does not run,
  are not reported, so a building is never idle. A frame waiting for its
  builder is not idle. The report fires once each time the unit runs out of
  orders, not on every tick it stands by.
- `order_ended` just before the scheduler removes an order its handler
  ended, with the order's mission kind and whether it was done or failed.
  Done is a handler result of 5; failed is 8, a 9 that gives way to the next
  order, or the head order of a result that clears the queue. Orders removed
  with that head by the clear, orders a new command replaces, the secondary
  queue's orders and orders of a dying unit are not reported.
- `unit_carried` at the end of `link_carried_unit`, after the link is in
  place, whether this machine made the link or `apply_carry_link` took it
  from another machine's record. The unit is reported unloaded from the
  carrier it left, then loaded onto the carrier it joined. A link that only
  changes the piece, and a link the carry checks refuse, are not reported.
- `wreck_placed` from `spawn_corpse` when a death places a corpse feature,
  after that unit's `unit_died` and while the dead unit's record still holds
  its position. The feature is the placed feature's type and the plot is
  its index into `World::plots`. A death that leaves no corpse is not
  reported.
- `seen_changed` at the end of a player's knowledge refresh, every 30 ticks,
  when the enemies on its seen list changed. The gained list is the enemies
  that entered and the lost list is the enemies that left, each with the
  unit's slot and the generation that tells that unit from the next one to
  take the slot. The comparison is `detection::diff_sighted`: a slot a new
  unit has taken is one lost entry and one gained entry, and the lost entry
  comes first. A lost unit may have died, or its slot may hold another unit
  now. The lists are valid only for the call. The player's own units and its
  allies are not on the seen list, so they are not reported, and nothing is
  reported when the list is unchanged. A player with no controller, a
  mirrored player among them, is not refreshed. After a load the previous
  lists are empty, so the first refresh reports every seen enemy as gained:
  a brain that starts again from a save hears the enemies its seat can see.
  The previous lists are kept up even when no one is listening, so a
  listener added during a match hears only real changes at its first
  refresh.
- `path_failed` during the tick's path search, when a route search for a
  unit simulated here finds no route and the unit is not already at the
  goal. The report names the unit and the order's mission kind. An order
  is reported once, so a move that tries again is not reported on every
  try. The mark is kept on the match's order record and is not saved or
  hashed; after a load it starts clear, so an order still trying may be
  reported once more. An order the match does not own is reported each
  time the search fails. A failed search for a unit already at its goal
  is not reported. Aircraft do not search routes, so they are not reported.

The hooks receive the world as const and must not change match state, draw
from its random streams or call back into it; the match computes nothing
differently with them set (`match-event-hooks` checks the synthetic
skirmish's digests against `match-determinism`'s pins with and without
them). A limitation: a unit only a state record from elsewhere finishes,
with no builder link following, is not reported finished.

A match also keeps listeners, each a whole `EventHooks` table added with
`add_event_listener` and removed with `remove_event_listener`. Each event
is reported to the primary table first, then to the listeners in the order
they were added. A match holds at most 8. An id is never reused, and 0 is
not an id. Listeners follow the same read-only rule, and a hook does not
add or remove a listener. Replacing or clearing `event_hooks` leaves the
listeners in place. `match-event-hooks` checks that they change nothing the
match computes.

## Sharing with the other players

A multiplayer match shares what it settles through `MultiplayerHooks`, and
applies what the other players' machines settled through entries of its own.
Every hook is null in a game played on one machine; an entry whose comment
says what null does may stay null in a multiplayer game too. Besides new
units, state flags, health events, scripts, shots, deaths and features:

- `shot_intercepted` shares each shot an interceptor's blast sets off here,
  once it has gone off, with the interceptor's shot. An interceptor
  simulated elsewhere sets nothing off here.
- `carry_link_changed` shares every carry link `set_carry_link` accepts,
  whichever machine simulates the units, just before it applies: transports,
  factory pads, the AttachUnit and DropUnit scripts, the builder link and a
  dying carrier's cargo. `apply_carry_link` applies a link another machine
  shared without sharing it again.
- `unit_finished` shares a unit simulated here that its builder's work
  finished, or a building created finished, which names itself as its
  builder. `finish_unit` runs the builder link on another machine's copy:
  it is marked built and activated when its type activates when built, and
  stays on its pad until that machine's carry link sets it down.
- `script_started` carries StartBuilding from the build, repair, reclaim,
  capture and resurrect orders, with the heading to the work as its one
  argument, zero-extended from 16 bits.
- `feature_changed` with `FeatureChange::resurrected` names the origin plot
  of a wreck a unit simulated here raised, so the wreck goes everywhere.
- `unit_transferred` hands a unit simulated here to a player another machine
  simulates, just before the unit dies here as captured. `transfer_unit`
  hands a unit to another player, as a capture or a gift of units does, and
  creates the unit another machine handed over from its `TransferredUnit`.
- `end_local_game` ends the local player's game at once as a defeat, as
  another player's machine reporting this one gone does.

`tick_scripts` advances COB contexts only. `tick()` runs a whole simulation
tick: the unit sweep with the match's own host, projectiles and explosions,
the path search, each player's controller, knowledge, sight and economy, the
features, the wind, the meteor storm, particles and the viewer's remembered
sight, in the game loop's order. `tick(Host&)` runs the unit sweep with an
external host instead. A gameplay branch the engine does not support, or a
broken invariant, is noted in the match's fault record (`Match::fault`) and
stops the operation that met it, leaving what it had done; moving collision and occupancy removal need the resolved type metadata and
collision plots and refuse movement without them. The match copies the
inputs' collision plots while it is constructed and keeps no reference to
them. Each tick starts by copying the units' ground and air words into the
`MapPlot` table: every plot on the first tick, then the plots the spatial
state lists as written since (`written_occupants`), or every plot again when
they come to an eighth of the map or the list lost some.

## Outcomes

Ordinary matches call `configure_outcomes` with the local player index, its
alliance row and the defeat permission; full ticks require it. The local
slot's victory and defeat checks run under its `Player.next_economy_tick`
deadline, which advances by 30 when due, before the slot's economy settles;
there is no separate outcome deadline. The checks read each player's live unit
counters. A condition that becomes false again does not reset the outcome
countdown, and the final outcome is kept for presentation.

Under the deathmatch commander rule an expired defeat countdown places a new
commander instead (`respawn_local_commander`) when the side table is bound;
otherwise the defeat stands. With no host seated the respawn reads the
eleventh player record's setup block. A defeated multiplayer player may go on
as a watcher.

## Sight

Moving units update their sight stamps with the centre and band written back.
Altitude sight needs derived height cells and the LOS.TDF ray patterns
(`OfflineInputs::altitude_sight`); it does not fall back to plain fog.
Coverage is kept for each of the ten players, while the mapped bits are
shared. `unit_visible` samples four corners of the unit's footprint and model
bounds, with the owner, cloak and submerged gates. `point_visible` uses the
asking player's coverage under line of sight, and the viewpoint player's
mapped bit under the mapping rule alone. Both read the coverage as the
simulation does, 3.1c's byte, so a cell 256 stamps see is out of sight to
everything that decides the simulation's results. `unit_shown`,
`point_shown` and `cell_or_footprint_corner_shown` read the whole count
instead, for what the player is shown alone: the fog, the radar, the units,
shots and features drawn, and the pointer and panels over them.

`scan_contacts` builds the viewpoint player's radar picture every 30 ticks:
it first marks its own and radar-sharing players' units as contacts, then
runs the scan's passes (`scan_contacts_for`) for the viewer: its active
radar and sonar units stamp contacts, other players' active jammers clear
them, cloakers near a seen enemy lose the cloak, and units in its line of
sight are radar contacts.

Allied vision (`intel.allied-los-sharing`, `intel.cpp`) grants a player the
view of every player whose alliance row names it, one way, with no
shared-radar role needed:

- the sight stamps count in the coverage of each player the owner allies
  (`SightContext::allied_vision`, in `src/sim/visibility-state`), so the fog,
  the line-of-sight contacts and `point_visible` see through the allies'
  units;
- `unit_visible` passes every unit whose owner allies the asking player
  before the cloak and sight tests, which the computer players' sightings,
  scripts' effects and, through `unit_shown`, the drawn units all go through;
- the contact scan marks every unit of such an owner, then runs its passes
  once for each player allying the viewer, in player order and each as the
  viewing player, so jamming in a later ally's pass clears what an earlier
  one found; a viewer without a unit range runs them once, for itself;
- at each player's deadline, before the viewpoint's scan, a changed
  alliance row or viewpoint player rebuilds every stamp, keeping the mapped
  cells (`follow_alliances_in_sight`); the rows it followed are its rule
  state.

Allied jammers (`intel.allied-jammers-ignored`, `jammer_jams`) stop jamming
the viewing player when it allies their owner; a watching local player
sees no jamming at all, unless the view-switch branch applies: then, while
the viewing player is not the local one in a game with mapping or line of
sight, only the alliance counts.

## Each player's radar picture

`keep_player_contacts` starts or stops one player's picture. The picture is
a byte per unit slot: radar, sonar and jammed, the same three facts the
viewpoint's flags carry, so a seat can be shown the contacts it would have.
The first keep allocates the table. Keeping that player again resets the
bytes to the bits a new unit starts with and sets the scan tick back to 0,
without allocating. Stopping zeroes that player's bytes. A player outside
the ten is noted as a fault and changes nothing.

The picture is scanned at that player's own deadline, every 30 ticks, in
`update_player_slots` just after the viewpoint's scan. `scan_player_contacts`
runs one scan immediately; tests call it. With one participant, or while the
picture is not kept, the scan does nothing and leaves the tick alone.

One rescan costs what the viewpoint's scan costs: two passes over the unit
slots (the first pass and line of sight), one bucket walk per active radar
or sonar unit of that player (and of each player who allies it, under allied
vision), and one pass over the slots plus a bucket walk per active jammer.
Nothing is allocated after the first keep. Nothing runs for a player nobody
keeps, so a match that keeps no picture does no new work.

Sightings, target searches and `unit_in_sight` keep reading the viewpoint's
flag bits, as they do in 3.1c. A picture is not saved, recorded or mixed
into a state digest. After a load it is rebuilt from the bits a new unit
starts with. `player_contact_bits` is what a later brain reads for that seat.

## Economy and repair

Natural repair runs on the eight-tick cadence of 3.1c. It uses the unit's
build cost, build time, maximum health, armour and state flags, and the
`Unit.economy` accumulator words; an accepted repair changes health and a
positive resource gate changes the requested debit without accepting it.

`update_player_economy` is the per-player economy tick: the activity gates,
EnergyUse and make credits with the easy and medium computer scales, cloak
upkeep with the cloak dropped when it goes unpaid, the merge of the unit
accumulators with the player block, the supply and stock ratios, the storage
clamps with waste totals, and the scaling of every block for the next tick.
`economy_test.cpp` asserts every player output field for human, easy, medium
and hard computer players across inactive, storage-full, storage-free,
negative-income, wind and tidal branches.

A mod's rules change the tick in three places. Its income multipliers
(`ai.income-multipliers`) scale a computer player's credit at every production
site by its difficulty's entry, hard included, and what its units reclaim from
features by the reclaim entries; refunds, unit deaths, shares and start
resources keep 3.1c's scaling. Under `economy.stats-exclude-shared-income`
what the player's staging block received from allies that tick leaves the
produced totals, at double precision, just before the block settles; income
and storage keep it. Under `units.init-cloaked-after-build` a unit whose build
fraction left has any bit set neither cloaks nor pays upkeep. Under
`economy.deterministic-wind` with the shared generator, the wind draws every
change from an MT19937 generator (`src/sim/world-environment`) seeded at the
first change with the host's network id, or the match's random seed without a
host record; the generator is the rule-state table `wind-generator`.
`economy_rules_test.cpp` covers each rule and its baseline.

## Orders

`issue_ground_move` is the ordinary command installation once the command
resolver picks Move_Ground: an unqueued command clears the unit's orders and
drops idle orders at the head, then the order follows the queue-tail mark. It
is not a generic command resolver or screen-to-world picker. `issue_order`
inserts an order of any mission kind (descriptor-kept queues, overlay drop,
head, secondary and queue-tail placement); the campaign's InitialMission
scripts queue through it (`mission_unit_binding.hpp`, ctest
`match-mission-units`). The 68-entry mission descriptor table is
`mission_descriptor_table`.

`issue_attack_command` is the attack command a player's order or a computer
player's squad order gives: the attack mission the command resolver picks for
the target (Attack_Chase for a ground unit, Suppress for a unit of the
unit's own side), issued through `issue_order`, which keeps the target only
when the mission takes one and the point the command was given at (the
ground under the pointer; none for a computer player). Unqueued, it replaces
the orders under way, so a move the unit was making no longer steers it: a
unit commanded on the move with its target in reach brakes and fires from
where it stops. `issue_attack` is the attack a unit starts on its own, from
its standby, patrol or guard mission or a hit's reaction: it goes ahead of
the orders the unit has and, unforced, on the manoeuvre order, carries the
unit's leash and a move back to where it stood (ctests `match-attack-command`
and `match-attack-command-data`; the pointer and area attacks in
`native-navigation-outcomes`). `issue_attack_ground` is the attack command on a ground
point: the command resolver picks its mission with no target unit, Suppress
for a unit that does not fly, AirToGround for an armed aircraft, gunships
among them, and AirStrike for a bomber, and a unit whose weapon only reaches
aircraft takes none (ctest `match-missions-air`).

`command.cpp` resolves the move and attack commands with the water-weapon,
aircraft, alliance, availability and capability gates; targeted moves go
through `can_repair_target` and `can_load_target` and the capture, reclaim,
repair, load and guard choices, with the packed capability bytes from
`pack_command_capabilities`. The ground mission family (`ground_missions.hpp`
with the command, attack and construction handlers) and the aircraft
families step every order kind with its phase results, wait events, timers,
leashes and goal retries. The aircraft missions fly their unit through its
air driver's goals (a point, a unit followed or faced, or a point sliding
along a step), never along ground paths, so an aircraft keeps its cruise
height over water and slopes between the legs of an attack or a search
(ctests `match-missions-air` and `match-attack-command-data`). Orders link
into their target's list of observing orders and lose the target when it
dies.

## Combat

The weapon tick resolves a slot's target (the SweetSpot piece centre, or the
terrain height under a ground point), starts the turret or vertical-launch Aim
script, applies the range and sea gates, runs the Fire scripts, keeps reload
and events, and places a projectile record in the 300-slot pool. The reload
countdown lives only in the unit's canonical `UnitWeapon.reload`, so saves,
traces and the state digest see the live value. Turret and
vertical-launch shots fire only after the Aim script the slot started has
returned nonzero, and each shot spends that aim. `UnitDef.model_height`, the
model top a shot must pass under, is taken from each type's loaded model when
the match is built.

A shot striking a unit applies its DAMAGE entry through the damage path: a
unit simulated here that drops below 1 health is marked dying and is torn down
by the kill handler, which clears its orders, drops its occupancy, removes the
instance and lowers the owner's unit count. `DeathKind` names the kinds; a
reclaim death credits the reclaimer `(1 - build fraction) * metal cost`. The
damaged unit's reaction (observers woken with event `0x10`, the computer
capturer alert, a chase or re-aim, the under-attack notice) runs through
`weapon_execution::retaliate`, and kind-2 damage and the Paralyze mission run
through the `unit_health` paralysis code.

The machine that simulates a unit settles its death: the kill handler works
out the Killed percentage and the wreck level (`KillOutcome`), drops a last
attacker whose player's slot has gone free, and, for a unit simulated here,
shares them through `MultiplayerHooks::unit_killed` before it clears the
unit's orders and tears it down, so the record still names the unit's owner
and last attacker. `Match::kill_unit` runs the kill handler on one unit;
death kind 0 kills a unit whose slot another player's new unit takes, by its
own health and with no statistics. `Match::apply_kill` applies a death
another player's machine settled: the unit takes that machine's last
attacker, its Killed script starts with the shared percentage for its flying
pieces only, a finished unit explodes when that percentage is above zero, and
the wreck is left at the shared level; statistics count by the shared kind.
`feature_host` passes weapon hits on features, and the fires, destructions
and reclaims settled here (a reclaim with the reclaiming unit), to the
multiplayer hooks' feature entries, and the resurrect order passes the wrecks
it raises; with those entries null every hit is applied here and nothing is
shared.

`update_projectiles` flies the pool through its flight modes, tests every
moved shot against the plot under it (intercept burst, occupants, feature,
ground bounce, ground, water) and compacts the pool; a record still live
after its move puffs its smoke trail, and a shot dropping into the sea
splashes. A detonation logs its effects in the match's effect world: the water
or lava art from the plot under the burst when no unit was hit, else the land
art or an endsmoke puff; a nosealeveltrigger sea swallows such a shot whole.
It adds the weapon's shake to the running screen shake in `Game`, which the
app's camera does not apply yet, and plays `soundhit` or `soundwater` where
the viewpoint player sees the burst. An interceptor's blast detonates the
shots near it and shares each one (`shot_intercepted`). A dying unit's
explodeas or selfdestructas weapon detonates through a stand-in shot with no
source after the unit has left its plot.

The commander rule's sweep runs only in the simulation that runs the
commander, after the `order_panel` hook closes the order panel. A commander is
the type its owner's side names, not the FBI flag. Carried units dying with
their transport, captured units, cancelled nanoframes and the hotkey
self-destruct take their 30000 through the veteran scaling of the damage
path.

## The load's players' pass

A match ends its load (`finish_load`, called by each way a match starts: a
new skirmish or campaign mission, a saved game, a network game and a
recorded one) with one players' pass at the load's tick, before its first
tick: the computer players are set up, the path search spends a tick's
credit and every player slot is updated as a tick updates it, so a slot
whose 30-tick deadline is due settles its economy and runs its outcome
checks there. A new game's energy and metal then return to the stores its
start set; a saved game keeps what the pass did, and so plays on from the
pass as the game's own reloads do. The pass waits until the match first
ticks (`run_load_pass`, which the tick drivers call before advancing the
clock and `tick` calls first), so a loaded match stands as it loaded until
then.

## Computer players and knowledge

The match sets the computer players up in the load's players' pass, or on
its first tick when a match did not end a load (`finish_load`). Each tick every
player whose record holds a controller (`Player.controller`) runs it: a
computer player's controller calls the seat's play hook when one is set, and
runs its order-callback tick (the squad sort and due tasks in `src/sim/ai`)
only when the hook is absent or returns false. The weapon sweep then retargets the slots of
units that fire at will; command-fire weapons take part only for computer
players. Interceptors aim at an enemy missile; other weapons take a random
enemy in range. The knowledge refresh follows for the same players: every 30
ticks it rebuilds the player's sightings (`src/sim/detection`), its pad list
and, for a computer player, its own-unit counts and base position, then a
one-in-30 draw runs the strategic refresh. The pad list holds the player's
own finished, switched-on air bases that are builders, in unit slot order. A
damaged aircraft that breaks off for a repair pad (attack, strike, seek,
patrol and repair patrol missions) draws one of the listed pads still
switched on within 0xf00 world units (`Match::repair_pads_in_reach` and
`repair_pad_in_reach`), so a pad built or switched on since the last rebuild
is not chosen until the next one. Offline matches run the wind from the OTA
`MinWindSpeed` and `MaxWindSpeed`.

The computer players reach the match through `ComputerHost`
(`src/computer_host.cpp`): squads, orders, placement queries and the random
stream, and for the siege task a player's sight of a point, its sightings, a
unit's first-weapon reach and the order the Attack command gives over open
ground.

## Path search

`advance_path_search` runs after the projectiles each tick: it starts a job
for the next player with a waiting unit, expands it and publishes a finished
route through `accept_path`. Each movement class keeps a persistent map that
footprint, occupancy and feature changes refresh.

## Verification

The `match-runtime` tests cover case sensitivity, immediate call return
callbacks, synchronous local writeback with an unrelated active context, unit
values and flag updates. The offline integration test builds a one-piece model
and a real COB program, then runs the full spawn order, masked footprint
occupancy, the weapon query fallback, sight changes, local ground controller
allocation and later hide, sleep and show ticks; a script-driven activation
runs the deferred Activate script and checks the sound and UI callback order.
Only the test's external callbacks are recorded fixtures. It also runs 35
stationary ground ticks (idle mission scheduling, health percentage refresh,
completion of the Create sleep) and 60 ground-motion ticks (cell changes,
terrain collision, occupancy removal and insertion, moving sight). The other
tests cover each order family, combat, the deaths shared with and applied
from the other players (`match-shared-deaths`), the interceptions, carry
links, completions, StartBuilding starts, resurrections, unit transfers and
game endings shared with and applied from them (`match-shared-events`),
projectiles, economy, features, transports, outcomes, saved orders, saved
features and the trace stream. `match-intel` plays the intel hacks against
3.1c's rules.

The tests reach the match through its public header alone. A test that
runs one piece of a tick by itself (an order's mission step, a unit's
death, a player's self-destruct, the end of a build, the air goals' world
queries, the move-rate bits) does so through `MatchTickAccess`
(`tests/support/match_tick_access.hpp`, target
`oa-sim-match-runtime-tick-access`), which hands the call to the match's
tick host as the tick does; no test includes the match's private headers.
The attachment links of a unit as slot indices are public
(`oa/sim/match_runtime/attachment_links.hpp`).
