# Enhanced Mode

Enhanced Mode is an opt-in set of versioned engine rules. Choose it in
Settings, under Engine Limits, before starting a game. It starts Off.
The choice applies to the next game; a running match keeps its rules.
The selected mod keeps its units, assets and unrelated rules.

Graphics settings are independent. Enhanced Mode does not select a
renderer, change graphical effects or require hardware acceleration.
Players in the same match may choose different graphics settings.

## Current preset

Rules version 4 remains the current preset. Versions 5, 6, 7, 8, 10, 11 and 13
are explicit experimental choices.
Version 8 extends version 7's static routing
with applied-leg queue checks and planning that ignores temporary unit occupancy.
Version 10 inherits version 8 and selects AI strike-target distribution
among spatial cohorts. Version 11 inherits version 10 and adds bounded
retention of compatible current cohort targets. Version 13 inherits version 10
and adds bounded cardinal queue escape without version 11's target retention.
Versions 9, 12 and 14 are unsupported. Version 6 derives from
version 4 and keeps version 5's advisory corridor lanes
separate. The presets retain a capacity of 10,000 units per participating
player. The engine allocates ranges only for participating players, with at
most 32,767 units across the match. Two players can therefore field 10,000
units each.
Spectators do not consume a unit range. The projectile pool holds 32,767
records, with allocation failures reported by the benchmark.

Scanner and jammer contact queries reuse the spatial projection within a
contact-picture pass. These changes establish capacity and remove repeated
work. Version 2 introduced bounded coarse routes shared between nearby
destinations, ignores moving traffic when building those static routes, and
rechecks every returned edge against current passability and player knowledge.
Narrow corridors outside the coarse graph retain the ordinary path search.
Version 3 groups more destinations into each shared field, returns shorter
route prefixes and retains useful fields when an extracted edge becomes
blocked. Its construction work stays bounded. Earlier presets keep their
original routing policy.

Version 4 checks the unit's footprint along each locally simulated ground
step, including intermediate cells and both sides of an exact diagonal
crossing. A free destination no longer permits a step through an occupied
intermediate cell. Work is bounded to 128 origins and 512 footprint cells;
an unsupported step receives the ordinary blocked response. This corrects
collision handling, but does not establish smooth movement through a crowded
battlefield. Off and versions 1, 2 and 3 keep their original movement rules.

Each preset has a distinct rules identity for saves, recordings and
multiplayer agreement. New experiments preserve the behavior of Off and
earlier presets. Unit and projectile capacities and renderer selection
remain unchanged.

Version 3 can also remove an unnecessary bend from a published guide when
the unit's actual footprint has a clear, fully known line to the following
point. The optional check has its own probe limit within the existing shared
budget. It traces the actual fixed-point position, including both sides of
diagonal grid crossings.

For an installed route with sustained blockage, version 3 periodically checks
the following corner without waiting for another search result. Each unit has
a 120-tick phase, with at most 128 origin queries and 512 footprint cells per
attempt. Successful advancement preserves the original goal, pending search,
retry clock and a compatible temporary passing head. This corrects blocked
waypoint cases; sustained 20,000-unit congestion remains unresolved.

Local steering gives moving units temporary detours around mobile ground
traffic, including idle units, and yields by waiting time measured at the
start of the tick, with unit ID breaking ties. The original mission and route
remain in place, and the final footprint occupancy test still prevents
overlap. A short mobile blockage can wait before requesting another full
path. After 30 consecutive collision-blocked ticks, ordinary replanning
resumes even if boundary jitter
moves the unit slightly. A local passing attempt can also retry once every
30 ticks, staggered by unit ID, without restarting that grace period.
Target searches use nearby spatial buckets and a sightings membership index
when that reduces work, retaining the legacy candidate order. They can miss
a unit at the edge of the range that has moved within its cell out of the
bucket it is filed in, which changes the candidates and their random
draws. The feature `exact_indexed_searches` (0.9.3) reaches one cell further
and finds exactly what the full scan finds; the old versions keep the first
searches.

Version 5 adds advisory lanes for fully known narrow ground corridors. Units
align with their directional lane before entering, preserve their original
orders while crossing, and request an ordinary route after leaving. Shared
work limits bound corridor discovery, new queries and retained guidance.
Retained units receive service in rotating slot order; deferred work keeps
crossing history, and saved games retain the service cursor. Native movement
and the swept footprint guard still decide every actual step. Version 5
remains an explicit benchmark choice after failing its earlier fixed-march
comparison; those results do not diagnose the later aligned-patrol battle.

Version 6 provisionally retains an unchanged patrol goal and its active
search while the unit is still travelling toward it. Arrival, failure,
cancellation and replacement still permit the normal mission transition.
For ground move, patrol and repair-patrol orders, usable formation offsets
remain unchanged. The bounds check uses the original group click when a
generated offset's full footprint leaves the playable area and the click
fits that area. This bounds fallback does not guarantee clear terrain.
After that check, the additional terrain fallback uses the original playable
click only when the resulting formation point has no statically eligible
native arrival footprint and the click has one. Moving units do not
invalidate a destination. These corrections do not establish that traffic
can reach a destination promptly.

Selection orders, including radar and megamap clicks, share this repair
with stage and camera orders. Queued-order cancellation compares the
repaired destination. Game-host move and patrol requests use the same
repair while retaining their existing order acceptance and append behavior.

The built-in computer player can refresh a lost strike target early. Its
strike selector rejects dying targets and targets without positive health.
Each Match permits at most one early AI task dispatch per six simulation
ticks on the AI's owning machine; ordinary strike deadlines remain separate.
The limit counts task dispatches across that Match. Its save-only 424-byte
state retains target bindings, retry deadlines and native strike clocks,
and stays outside shared simulation hashes.

Version 6 also keeps a fixed index of squad membership positions. Transfers
preserve the existing swap-and-pop member order while avoiding repeated
linear searches in fresh matches. If restored or changed membership makes
the index uncertain, that Match permanently uses the existing searches.
The index is derived state and adds no saved fields. Recruitment decisions
and immediate order dispatch keep their existing timing; this does not cap
the work of a whole AI task.

The current conservative queue predicate keeps a unit behind a live,
same-owner leader with the same footprint and directed next two route
points. Their final destinations may differ. The leader must own its active
route and be ahead and nearer the next point. This applies within the existing
traffic grace period of fewer than 30 blocked ticks; mature recovery remains available.
The installed-map results below remain mixed and do not establish smooth flow.

Version 8 checks a leader's current applied passing leg before certifying that
following it still advances a queue toward its guide. Ground-route planning
also treats units with movement objects as temporary occupancy, including
zero-speed definitions. Static building footprints, blocking features, terrain
depth, slope and bounds retain their ordinary passability rules. Every actual
ground step still checks current footprint claims and swept collision.

A unit can retain its forward route while it waits for space in a crowd.
Arrival, replacement and other native failure paths retain their normal mission
transitions. In a separate prototype comparison with 20,000 units and 9,000
simulation ticks on Core Prime Industrial Area, Hold Fire patrols in version 8
recorded no path-failure callbacks and improved mean rear progress over version 7.
Sustained bridge transit and broad rear advance remain unresolved. Version 8
remains experimental and does not replace the default version 4.

These changes do not yet establish smooth frame rates at that population.
Ray tracing remains future work and will be a separate graphics choice.

The desktop minimap draws visible contacts at the scaled HUD's layout
resolution instead of enlarging contacts already merged in the original
126-pixel radar. Compact owner-colored cores retain nearby contacts, with
contrast edges and stable hints for opposing owners sharing a pixel.
The original logos' opaque grey borders can erase a nearby owner's color;
the displayed layer paints every contour before any contact core.
Fog and radar visibility and the original input coordinates remain in use.
Low-resolution and phone displays retain the ordinary radar path. Dialogs
keep the displayed contacts beneath their panels and apply the same panel
shading to the minimap's overlapping pixels.
This presentation change is independent of Enhanced Mode. It does not
guarantee a separate display pixel for every unit at arbitrary densities.

The engine resolves the preset over the selected game's profile before
building the world. Its version enters the simulation hash. With the mode
Off, existing profiles keep their canonical rules and hashes.

## Saved and recorded games

A saved game records Enhanced Mode in an EnhancedMode account, format 2.
The account holds Revision (an integer, at least 1 when the mode is on),
Features (feature ids as comma-separated decimals, written in id order,
never names) and Needs Version (the text of the first engine version that
plays the choice, such as `0.9.3`, omitted when the writer did not know
one). It also keeps the unit-player mask and the physical unit-range
order. A game with the mode off writes no account. A missing account
loads as Off. A format-1 account is unreadable.

A recording stores the same choice in the `OAE1` sector. The sector is 16
bytes, little-endian, after its type word:

| offset | size | field |
| --- | --- | --- |
| 0 | u32 | revision, at least 1 |
| 4 | u64 | feature bits, bit (id − 1), so bit 0 is feature 1 |
| 12 | u32 | needed engine version, `major << 16 \| minor << 8 \| patch`, or 0 when unknown |

Any other length is refused. This build writes `0.9.3` as `0x00000903`.
A recording without the sector plays with the mode off. Bit 63 would be
feature 64, which does not fit the feature set, so a reader refuses it
instead of dropping it.

Loading a save or playing a recording restores that choice and does not
change the preference for new games. A revision or a feature id this build
does not know is refused. The refusal names the stored engine version:
"This game needs Open Annihilation {version} or later." When the file
stored no version, it is "This game needs a later version of Open
Annihilation." The reader does not guess a version it was not told. A set
that is not closed, that conflicts, or that pairs revision 0 with features
is refused as unusable. A malformed account or sector is refused.

Enhanced saves retain both the original participants and their physical
unit-range order. Loading a network save therefore preserves ownership and
unit IDs even when player slots were ordered differently between machines.
The old numbered presets remain playable as revision 1 plus that preset's
feature set. Versions 2, 3 and 4 save five owner-local navigation tables:
shared fields, the current static-map revision, temporary traffic heads,
actual-progress clocks and the shared-route scan clock. The scan clock
prevents a load pass and a regular pass at the same tick from polling the
unit ranges twice. These tables stay outside shared world and trace hashes.

In a battle room the host decides. The host's Enhanced Mode switch, right
of BATTLE ROOM, starts from the host's own setting, and Off in a Boneyards
room; the host may turn it on there. A launch that locks the room's options
keeps it Off and grayed. Every machine from Open Annihilation 0.9.3 on
announces the highest revision it plays (presence field 0x26), whatever its
own setting says. While the switch is on, the room plays the lowest of the
highest revisions of every seated human and watcher; a computer player
counts as the machine that runs it. A machine that announces none, 3.1c or
Open Annihilation 0.9.2 and older, makes the room Off, and the room names
it. A machine whose record has not arrived yet changes nothing and holds
START. The host's record announces the room's revision (field 0x27), and
leaves it out while the room is Off, so an Off room sends what it sent
before. Every machine, the host included, plays the room's revision, never
its own setting's choice, and START waits until every machine's simulation
hash is the host's. Each change of the room's revision posts one line:
"Enhanced Mode: Revision {n}.", "Enhanced Mode: Off.", "Enhanced Mode is
off: {player}'s game doesn't have it." or "Enhanced Mode is Revision {n}:
{player}'s game has no later revision." No battle room loads a saved game
in this build; every load holds the choice its save stored.
Unit movement travels between the machines of an Enhanced match as complete
route batches ([Network transport](#network-transport)): a tick of route
changes from thousands of movers reaches every machine whole.

## Network transport

With Enhanced Mode Off, and in any game with a 3.1c machine, a player's tick
travels as 3.1c's unit state record (0x2c): the units whose route changed
are listed until the record reaches 0x200 bytes, about 33 three-point route
heads, and the rest are put off to later ticks, where only each unit's
newest head goes out; a lost frame is not sent again. None of that changes.

Every machine of an Enhanced match is an Open Annihilation machine playing
the same rules, so there each local player's tick travels as one complete
route batch in place of its 0x2c. The network rules carry the batches
whenever `setup.enhanced-mode` is on. Every build with Enhanced Mode has
them; they are no feature of a revision.

- **Complete.** A batch lists every unit whose route, traffic head or
  blocked bit changed in the tick, and every aircraft whose goal changed,
  with no byte budget, in the layout of the 0x2c list, and ends with the
  round-robin full unit record. A part is closed once it reaches 0x380
  bytes, so no part is longer than one send buffer (0x42a). A tick's
  changes are never merged with another's: each tick sends all of its own.
  Each batch names the tick of the player's batch before it, so a receiver
  knows their order whatever ticks it skips. A batch that cannot be packed
  sends nothing.
- **Applied whole, in order.** A receiver keeps a sender's parts until the
  batch is whole and the batch before it is applied. It then applies every
  change, steps the sender's units once and applies the full record, so
  each copy steers from exactly the heads its owner chose in that tick. The
  step a sender's tick triggers is held until then. Parts may arrive in any
  order; one already held, or of a batch already applied, changes nothing,
  and so does a malformed one. Spread over the receiver's ticks, a batch's
  parts fall due together, as one 0x2c would.
- **Reliable.** Each machine acknowledges, from each of its players, what it
  holds of every other player's batches, at most once per send interval (6
  ticks at the default rate) and every 30 ticks while it holds one it
  cannot apply yet. A sender keeps each batch until every other player has
  acknowledged it, and sends a part again to a player that lacks it after
  30 ticks, or after 6 once that player holds a batch sent later.
- **Bounded.** A movement step is held at most 900 ticks, the lag at which
  the game loop starts slowing for a player that falls behind: a batch a
  receiver has held that long is applied with the parts it holds, and its
  sender stops sending it.
- **Gated.** The pump admits the batch records in the game only. A machine
  without the rule ends its frame split at them, as 3.1c does, and sends
  none. Where batches carry the unit states, a 0x2c only gives the commander
  start sync.
- **Recorded.** A recording keeps each part once, as it was first sent or
  first arrived, and no acknowledgement. Playback of network games and of
  recorded skirmishes puts the parts back into their frames and applies
  each batch whole before its step.

The records, little-endian:

| Record | Offset | Field |
| --- | --- | --- |
| Part (0xe4) | 0 | type |
| | 1 | u16 length, the type byte included |
| | 3 | u32 tick whose changes the batch holds |
| | 7 | u32 tick of the sender's batch before it, 0 for its first |
| | 11 | u16 part index, from 0 |
| | 13 | u8 flags: bit 0 on the batch's last part |
| | 14 | bit stream as the 0x2c list: per change a u16 unit index, the def index and the unit's delta; the u16 terminator 0xffff; in the last part only, a flag bit and the full unit record; padded to a byte |
| Acknowledgement (0xe5) | 0 | type |
| | 1 | u16 length, the type byte included |
| | 3 | u32 player id whose batches it acknowledges |
| | 7 | u32 tick of the last batch applied, 0 for none |
| | 11 | u32 highest tick of which any part is held |
| | 15 | u8 entries, at most 8 |
| | 16 | per entry, oldest first, a batch held but not applied: u32 tick, u16 parts mapped, u8 flags (bit 0: its last part is held), then one bit per part held |

Measured on the Mac Studio over its loopback (`native-net-loopback-route-burst`
and `-loss`): 3,000 movers sent south at the same tick make one batch of
3,000 changes, 35,171 bytes in 39 parts, about 11.7 bytes a change. With no
loss the joiner applies it at the tick it was sent, no step is held and no
part is sent again. With 10% of each machine's frames lost it is whole 7 to
19 ticks after the burst's tick; the longest held step was 18 to 19 ticks
on the joiner and 25 to 42 on the host, for the joiner's own batches, and
about 430 to 520 parts were sent again over 3,030 ticks. Five thousand head
changes in one tick pack into 84 parts of 75,788 bytes (`net-replication`).

The bound: each lost frame costs one more round, which takes at most about
36 ticks (the 30-tick resend and one send interval) and usually 6 to 12. At
10% loss a frame is still missing after four rounds one time in ten
thousand, so the burst's twenty-odd frames are whole within about 150 ticks
in all but two bursts in a thousand, far inside the 900-tick lag rule. No
burst can hold a step past 900 ticks: there the batch is taken with what
has arrived.

### Three-machine test plan

For the suite's network case of Revision 1:

- Machines: the Mac Studio hosts on macOS; its Windows and Linux guests
  join. Every machine plays Enhanced Mode, Revision 1.
- Starting each machine: the suite starts every engine from the command
  line, as its other crossplay cases do, and readies and starts the battle
  room as they do. The host runs `--host --map MAP --start-positions fixed
  --enhanced-mode --net-route-burst 3000`, and each guest `--join ADDRESS
  --side arm --net-route-burst 3000`: a host started with launch settings
  such as `--map` admits only guests whose start is a launch too, which a
  launch setting such as `--side` makes it. Each machine also writes
  `--trace-units FILE` and `--net-record FILE`. A host in Enhanced Mode
  opens its room with a unit limit that holds the movers beside the
  commander.
- Stage: nothing more is clicked. Once the match starts, each machine gives
  its own player 3,000 movers, and at tick 300 sends them all one row south
  in one group move ([`--net-route-burst`](command-line.md#network-play)).
  Every machine lays out the same blocks: one for each player, by the order
  of the players' unit ranges, clear of every start position of the map
  and of the other players' blocks. In a block the movers stand loose, in
  staggered rows, and a mover is made only where it can walk its march, so
  that every mover can reach its goal and stand still there. The match then
  plays on until every mover has stood still and a round of full unit
  records has passed: about 3,200 ticks for 3,000 movers a player, and
  1,200 for 1,000.
- Runs: one with no loss, then one with 10% of the frames lost on one link
  (shaped by the suite; `-e 10` on one guest instead loses that share of
  every frame the guest sends).
- Comparing the machines: on what the network shares, never on world
  digests. Each machine simulates and orders only its own player's units
  and follows the others' from their records a few ticks behind; it numbers
  its own player first, its copies of other players' units hold no orders,
  and a copy's health arrives only with its owner's round-robin full unit
  record, one unit a tick. So two machines' `--trace-digest` streams are
  never equal at any tick, and each is kept as that machine's own record.
- Pass:
  - every 300th tick, each player's live units are the same on every
    machine;
  - at the end, once every mover has stood still on its owner for 300
    ticks and a whole round of full records (the room's unit limit in
    ticks) has passed since a unit last appeared on any machine, every
    unit's owning player (by who plays it, not by slot), type, map cell and
    health are the same on every machine; a mover still moving fails the
    run and is named;
  - no machine holds a step longer than the bound above (150 ticks in the
    lossy run, a few ticks in the lossless one), and each machine's
    `route batches:` line, printed as it leaves the match, is kept with the
    run;
  - each machine's recording plays back with no record error, twice to the
    same stream, and the units it settles to are the live machines'.
- `native-net-route-burst` plays the same plan with two programs on one
  machine, 1,000 movers each on Painted Desert, and
  `native-net-route-burst-loss` with `-e 10` on the guest
  ([testing](development/testing.md#network-play)).
- Plus an Off run of the same stage, whose wire is byte for byte the base's
  (the capture compare of the crossplay cases), and a battle room with a
  3.1c machine, which plays Off with unchanged bytes.

## Measuring battles

The simulation-only benchmark runs repeatable idle, marching and combat
workloads with a fixed seed and writes a JSON report. `group-march` gives both
armies distinct, footprint-checked arrival positions in shared destination
regions. Its report counts unique units making progress, arrivals and stalls,
including the initially near, middle and far thirds of each army. For example:

```sh
./run.sh --game-dir /path/to/TotalAnnihilation --enhanced-mode \
  --battle-benchmark /tmp/oa-combat.json --benchmark-units 128 \
  --benchmark-scenario combat --benchmark-warmup 30 --match-ticks 300
```

A capacity run on a large installed map can request the full population:

```sh
./run.sh --game-dir /path/to/TotalAnnihilation --enhanced-mode \
  --battle-benchmark /tmp/oa-20000.json --benchmark-units 10000 \
  --benchmark-map "Core Prime Industrial Area" --benchmark-scenario combat \
  --benchmark-warmup 30 --match-ticks 300
```

The population is per side and includes its commander. This harness
measures simulation tick cost, not rendered frame rate. It refuses
unsupported populations and maps without enough clear placement sites.
See [the command-line reference](command-line.md) for all options and
[the benchmark module](../src/app/battle-benchmark/README.md) for report
fields and workload limits.
Use `--benchmark-rules-version 1`, `2`, `3`, `4`, `5`, `6`, `7`, `8`, `10`,
`11` or experimental `13` with Enhanced Mode to compare the presets
on the same seed. The report includes simulation phase timings and
cumulative route, local-flow and targeting work; percentiles of individual
phases are not additive.

`--benchmark-features SPEC` is a development flag for the battle benchmark
only. SPEC is a comma-separated base (`v<N>` for an old alias version, or
feature names) followed by `+name` and `-name` items; the engine closes the
set and plays it in that benchmark, including the `stage` scenario. It is
refused everywhere else, and it is refused beside a rules version, an
Enhanced Mode version, Enhanced Mode off, or a network game. While it is
set the rules are held at version 1, so anything that still reads the rules'
set sees only capacity, and the report's `enhanced_version` is 1.
`enhanced_features` names the set the match played, in id order.
`enhanced_features_spec` is the flag's text. `enhanced_features_override`
is true when the flag chose the set. `data_faults` counts distinct data-fault
lines. `staged_battle.extra` holds the named numbers a gate compares:
`swept_misses` counts the ground steps whose path passed through ground or a
unit that blocked it through the whole tick, which swept ground steps refuse
([battle benchmark](../src/app/battle-benchmark/README.md)).
`--benchmark-features` is a development flag: it cannot be combined with
`--enhanced-revision` or `--enhanced-features`, which put a set into the
rules themselves, and it goes with the other development aliases before
release.

`oa-enh-gate` takes `--features SPEC` (it passes `--enhanced-mode
--benchmark-features SPEC` and refuses `--version` beside it), `--noise
PATH` (the noise file, default `baseline/gate/noise.json`), `--noise-out
PATH` (where `--noise-from` writes) and `--keep-trace` (gzip each battle's
per-tick stream into `--out`). Its report adds `.all` totals over both
players, `attackers.pN`, `faults`, `work.NAME` and `extra.NAME`.
`oa-enh-reference --features SPEC` compares that set with `--against
VERSION`, `--against-features SPEC2`, or `--twice`, and plays `ai-patrol`
through the benchmark's `stage` scenario. Without `--features` both tools
keep their previous options and output.

### Staged battles

The benchmark's `stage` scenario measures a battle with computer players
and group orders: it starts the skirmish the preferences set up, plays a
stage file's placements and group orders through the player's order path,
and then measures every tick. Outside the timed step it follows the units
the stage left and reports, per player, progress toward each unit's first
move or patrol destination, the share of the last 300 ticks they stood
still, units crossing named measure lines (`--benchmark-crossing`), deaths
and births, how widely attackers spread over their targets, and an audit
every 150 ticks that each mobile unit's footprint cells name it. Its
per-tick digests are those of the same stage played headless with
`--stage`. See [the command-line reference](command-line.md#staged-battles)
for the fields.

The version 6 aligned-patrol battle is such a stage: 9,999 stock troops a
side in aligned columns on Core Prime Industrial Area, each army with its
commander given one patrol click, Fire at Will and Maneuver, CORE played
from the local seat and ARM by the normal computer player, seed 7. With
version 4 for 9,000 ticks it reproduces that checkpoint's deaths (521 CORE,
223 ARM), births (none and six) and 61 clean audits.

### Reference fixtures and gate battles (0.9.3)

The 0.9.3 restructure keeps two scripts with its plan, outside this
repository; both run a worktree's build on the build machine and build
nothing.

- `oa-enh-reference [--fixture F] [--version N]` replays three short,
  deterministic fixtures for Off and every old version: the `group-march`
  and `combat` benchmarks at 1,000 units a side, and a headless stage of
  the aligned-patrol formation's front eight columns (1,356 troops a side,
  the most Off's unit limit holds) with the normal computer player, each
  for 1,800 ticks at seed 7. It compares each run's `--trace-digest` stream
  with the reference recorded before the restructure and prints `same` or
  the first tick that differs, and fails on any difference.
- `oa-enh-gate --version N [--map M] [--ticks T] [--baseline NAME]
  [--out DIR]` runs the gate battles one at a time: the aligned-patrol
  battle; Expanded Confluence (rivers crossed at fords) and Dire Straits
  (narrow straits between the halves), each 10,000 stock units a side
  placed where the benchmark finds clear ground, with one patrol click per
  army and a computer player; all for 9,000 ticks at seed 7; and the
  synthetic counterflow, crossing and choke fixtures. It writes
  `report.json` and `report.md`, and against a named baseline it prints
  each metric's change and whether the change exceeds the measured
  run-to-run noise.

## First 20,000-unit checkpoint

An Apple M5 Mac with 24 GB of memory completed the combat workload on Core
Prime Industrial Area with 10,000 units per side, seed 7, 30 warmup ticks
and 300 measured ticks. The final population was 19,349, with 2,495 live
projectiles sampled at peak and no projectile allocation failures. Repeated
runs produced the same final state digest, `4f1722b3e338a32d`.

One Check-build run measured 27.3 ms median, 73.7 ms p95 and 206.1 ms p99
simulation ticks. Host load affects wall times. This is a scripted,
simulation-only workload with no AI decisions or rendered frames; it does
not establish sustained 30 Hz simulation or 60 FPS rendering.

## Version 2 checkpoint

These timing runs precede the route-quality corrections described below.

Paired Check-build runs on the same M5, Core Prime Industrial Area and seed 7
compared 30 warmup ticks and 300 measured ticks. Two combat runs per version
retained identical results within that version: v1 `4f1722b3e338a32d`,
v2 `943bf1ddd2329180`. Other host workloads varied, so these wall-time ranges
do not establish a uniform speedup.

| Workload | Rules | Median tick | p95 tick | p99 tick |
| --- | --- | --- | --- | --- |
| 20,000-unit combat, two runs | v1 | 12.9–18.0 ms | 21.9–27.3 ms | 27.8–30.2 ms |
| 20,000-unit combat, two runs | v2 | 10.8–12.1 ms | 15.3–25.2 ms | 18.1–36.6 ms |
| Dense 20,000-unit group march | v1 | 16.9 ms | 26.4 ms | 34.5 ms |
| Dense 20,000-unit group march | v2 | 19.7 ms | 24.9 ms | 36.5 ms |
| Active 20,000-unit march, two runs | v1 | 6.2–8.8 ms | 11.3–17.9 ms | 13.1–25.2 ms |
| Active 20,000-unit march, two runs | v2 | 8.8–11.2 ms | 13.8–18.2 ms | 16.1–20.8 ms |

Combat sighting-resolution attempts fell from 194,599,924 to 7,044,792;
these counts exclude bucket collection, sorting and membership filtering.
The default dense march starts touching-width rows at 32-pixel spacing,
with only 96 pixels between the opposing fronts and hold fire enabled.
It measures congestion after early contact, rather than sustained open-ground
motion. All 20,000 units survived; units advancing at least 16 pixels toward
their goals rose from 12,646 to 13,545. Total goal-distance improvement was
478,187 pixels for v1 and 485,923 for v2. Many remained stalled.

The active march uses the video's separate capture fixture: 48-pixel spacing
and banks offset 1,024 pixels away from the centre, with the same engine
physics and pose recording disabled for timing. This configuration is not
the default CLI layout. Both versions had 19,840 units advance at least 16
pixels. V2 performed 499 shared-route hits and 548 ordinary search starts;
v1 started 217 ordinary searches. V2's extra steering and replanning have
costs; these results do not establish that every formation moves faster.

A separate v2 combat run completed 1,000 measured ticks, ending with 19,292
units and no projectile allocation failures. It measured 11.4 ms median,
15.4 ms p95 and 19.6 ms p99, with a 61.2 ms maximum. Most simulation ticks
fit the 33.3 ms target in these runs, but occasional long ticks remain.
Rendered frame rate, AI-heavy workloads and gaming-desktop results remain
unvalidated.

## Route-quality checkpoint

Enhanced v2 now completes a temporary avoidance leg after the unit passes its
target, rather than requiring a turn back into a five-pixel arrival circle.
A replacement path clears the old avoidance leg even when its destination
has not changed. Shared routes preserve a clear lane to each unit's own
destination, favour cardinal expansion at equal depth, and retain bounded
route-prefix progress when a direct visibility test would exceed the tick's
remaining probe budget. Off and v1 keep their previous movement behavior.

The sustained eight-unit steering fixture, with one-cell footprints, completes
in 466 ticks instead of 549. Its closed-orbit windows fall from 34 to zero,
with no footprint overlaps. This fixture exercises the actual steering and
integration functions; it does not establish stock two-cell traffic capacity.

A separate 900-tick comparison repeats the original 20,000-unit march on Core
Prime Industrial Area with byte-identical initial positions, seed 7, 48-pixel
spacing and 1,024-pixel bank offsets. Units entering a tight orbit fall from
10,792 to 7,142, a 33.8% reduction. Here an orbit means travelling more than
24 pixels in 60 ticks while ending within five pixels of the window's start.
Units travelling fewer than 16 pixels in the final 150 ticks fall from 11,283
to 9,558. Median inward displacement increases from 552 to 630.5 pixels on
the left and from 406.5 to 614.5 on the right. All 20,000 units survive this
hold-fire comparison. Congestion and repeated local detours remain.

An exact static-footprint audit finds 1,419 of that original march's mirrored
goals blocked or on disconnected ground. The separate combat recording uses
connected, mirrored troop placements and real attack orders, with stock
footprints and movement. Its HUD counts the original roster's live targets
with positive health and removes a unit on its actual lethal-hit tick.
The dots are drawn from recorded engine positions; the recording does not
measure the game's rendered frame rate.

A subsequent 300-tick combat benchmark preserves the v1 digest
`4f1722b3e338a32d` and the v2 digest `943bf1ddd2329180`. One v2 run measures
14.6 ms median, 32.3 ms p95 and 71.7 ms p99, with a 561.6 ms maximum wall
time. Host load varies, and long ticks remain; these results do not establish
smooth whole-battle performance.

## Congestion and CPU checkpoint

Shared-route polling now visits at most 1,024 unit slots per tick across the
active players independently of the ordinary search cursor and its credit.
It skips a unit whose full search is already active, leaving that search's
route and completion authoritative. Sustained blockage can still receive a
shared terrain detour when the wall lies beyond the local traffic probe.
Pending fields, unchanged guides and direct goal legs leave ordinary dynamic
replanning available after 30 blocked ticks. A successful delivery observes
the ordinary 60-tick retry interval; pending fields do not consume it.

Static-map changes, including wrecks, retain useful fields and mark them
dirty rather than discarding every frontier. Returned connections are
checked against the current map. Polling rotates the first active player
each tick so one army cannot always consume the shared extraction budget.
Completed dirty fields refresh one at a
time, at 30-tick intervals, at least 60 ticks after completion. A route can
skip its first coarse anchor when a bounded footprint check clears the
connection to the next corner. Local passing also rejects an immediate
sideways return that would undo a leg completed on that or the preceding
tick, while allowing forward rejoining and a mature recovery to reverse.
Completed inactive heads retain their completion tick; new routes clear
that history. A shorter copy of an existing guide also leaves a mature
passing head intact. The saved head remains 27 bytes. Traffic probes follow
the movement controller's projected aim; passing directions retain the
original route intent and bounded lookahead.

A unit stationary for at least 30 ticks may voluntarily yield its traffic
priority during its staggered mature retry. Eligibility uses the same
tick-start motion snapshot as ordinary priority comparisons. The recovery
keeps the existing passing candidates, footprint checks and native movement.

Physical footprint results share a fixed 640 KiB scratch memo within one
planner invocation. Each lookup still checks the requesting player's current
fog first. A new invocation, including another load pass at the same tick,
starts empty. This scratch storage is neither saved nor hashed.

The earlier connected 20,000-unit recordings still show very little passage through
the centre bridges. In the 1,800-tick comparison, recorded bridge crossings
involve four distinct units in combat and eleven in the hold-fire march.
The count requires a completed side change inside the bridge lane; touching
the bridge or crossing elsewhere on the map does not count. Sampling can
hide additional crossings between recorded ticks. These results do not
establish repaired bridge flow, despite the smaller steering fixtures'
progress.

A subsequent recovery-only comparison repeats 1,800 ticks with the same
20,000 starting units, goals and production routing limits. Mean Euclidean
goal-distance improvement rises from 1,008.65 to 1,028.38 pixels. The original
rear cohorts, 1,378 ARM and 1,219 CORE units, improve from 269.39 to 283.03
pixels on ARM and from 230.36 to 242.34 on CORE. All 601 recorded samples
have zero stock-footprint overlaps.
The rear still makes little goalward progress during the final ten seconds:
6.56 pixels on ARM and 8.41 on CORE, with medians near zero. Only five distinct
units cross the centre bridges, including repeated returns. This improves
stationary recovery but does not resolve sustained congestion or bridge flow.

Separate frozen-workload comparisons measure target-query and death-cleanup
changes apart from these route changes. Each uses 20,000 starting units,
Core Prime Industrial Area, seed 7 and 1,800 ticks without warmup. Both sides
of each pair retain digest `d13d0852756db488` and 18,902 final units.
Filtering nearby target slots by the sightings membership index before
sorting changes median tick wall time from 11.219 to 10.339 ms and p95 from
14.906 to 14.521 ms. P99 worsens from
19.120 to 28.634 ms in that pair, so this is not a uniform latency improvement.

Ground-only v2 matches also skip air-target cleanup until a flying driver or
air goal has existed. Thereafter every death keeps the cleanup, including
mirrored, arrived and restored goals. In its separate paired run, total
thread CPU time at the two large death bursts falls from 45.181 to 26.368 ms
at tick 147 and from 40.370 to 23.455 ms at tick 228. These are whole-tick CPU
measurements, not rendered frame times or a whole-battle tail-latency claim.

V2 now indexes owned orders and observer links in preallocated storage.
Removed orders are destroyed immediately; empty ownership slots compact in
stable creation order on a later operation tick. Full-capacity replacement
retains a linear storage-reuse fallback. Certified observer links retain
checks against their actual predecessor and target; unindexed records use
the original checks. Off and v1 keep their original order handling.

Separate native 20,000-unit fixtures on the same M5 compare common-target
command and death bursts. Each pair retains its world digest and every
non-timing activity field:

| Operation | Before thread CPU | After thread CPU |
| --- | --- | --- |
| Death releases 9,674 attack orders | 383.183 ms | 20.635 ms |
| Stop 10,000 units targeting a living enemy | 112.720 ms | 14.104 ms |
| Retarget 10,000 units through the ordinary attack command | 132.131 ms | 23.775 ms |

Death and Stop run for 600 ticks; Retarget runs for 90. These isolate order
cleanup rather than whole-battle performance. External scheduling still
causes occasional longer wall times: one 52.460 ms Stop-fixture tick used
only 11.949 ms of thread CPU. The larger installed-map congestion remains
unresolved in the hold-fire crossing workload.

## Version 3 installed-map checkpoint

Version 3 repeats the connected, native 20,000-unit capture on Core
Prime Industrial Area for 1,800 ticks. Troops retain stock two-cell footprints,
48-pixel initial spacing and 1,024-pixel bank offsets. Combat uses actual
attack orders, with 9,567 ARM and 9,499 CORE units surviving. All 934 recorded
deaths match the native per-tick census and lethal-hit records; the first is
at tick 145. The final world digest is `2c965e3f554a1a34`.

On the same M5, this Check-build capture records simulation thread CPU time:

| Mean tick | Median tick | p95 tick | p99 tick | Maximum tick |
| --- | --- | --- | --- | --- |
| 11.20 ms | 11.44 ms | 14.05 ms | 15.54 ms | 29.10 ms |

None of these 1,800 ticks exceeds the 33.3 ms CPU budget. Other host work
can still extend wall time. This is one scripted battle with no strategic
AI or rendered frames, and does not establish rendered frame rate, desktop
performance or a causal speedup over captures made under different host load.
Pose and census recording occurs outside the timed simulation step.

The matching hold-fire march still shows poor flow. Only 12 distinct units
complete a side change inside the centre bridge lanes. The original rear
cohorts improve their goal distance by means of just 15.74 ARM and 17.93 CORE
pixels during the final ten seconds, with medians of 2.52 and 2.64 pixels.
Every recorded footprint is physically legal and no sample has overlapping
units, but shared-route coverage and low CPU cost have not resolved the
waypoint pile-ups.

## Version 4 ground-step checkpoint

The swept ground guard passes both independent footprint-intersection checks
and Match movement tests. In a native sixteen-unit corridor comparison,
arrivals rise from eight to nine, but the remaining total goal distance rises
from 3,845 to 4,287 pixels. This is a collision-correctness improvement with
mixed movement results. The separate corridor-coordination prototype is not
enabled: it reduced arrivals to four in the same comparison.

Two Check-build combat runs on the M5 use Core Prime Industrial Area, seed 7,
10,000 units per side, 30 warmup ticks and 300 measured ticks. Every non-timing
report field repeats exactly, with world digest `54bc88091f949178`, active
combat and zero projectile allocation failures. Median simulation thread CPU
time is 10.23–10.48 ms, p99 is 13.93–14.20 ms and the largest tick is
15.57 ms. All measured wall and thread CPU times stay below 33.3 ms. Earlier
versions reproduce their pinned world digests in this build.

These short scripted runs contain no strategic AI or rendered frames. Other
host workloads were uncontrolled. They do not prove a speedup, sustained
whole-game frame rate or improved bridge throughput at 20,000 units.

## Version 5 corridor checkpoint

The fixed hold-fire march on the M5 retains 20,000 units over 1,800 ticks.
Version 5 increases completed inward centre-bridge actors from 11 to 12,
but all 601 recorded positions of the original 2,597 rear-cohort units
remain identical to version 4. Only 68 actors differ anywhere, first at
tick 1,716. This is not a large-battle flow improvement, so version 4 remains
the default and version 5 stays an explicit benchmark choice.

All recorded footprints, native-cell snapshots and live censuses pass.
Observed simulation thread CPU time is 11.73 ms mean, 14.84 ms p99 and
29.93 ms maximum; none of the 1,800 CPU or wall samples exceeds 33.33 ms.
These measurements cover this scripted march, not strategic AI, rendered
combat or gaming-desktop performance. Rear units retain their orders and
routes but remain blocked by mobile traffic.

This older fixture uses individual mirrored goals and hold fire, without
the normal ARM computer player; its commander-bank assignment also differs
from the aligned patrol workload below. Its bridge and rear metrics do not
diagnose that later battle. A separate version-4 headless diagnostic now
loads the exact aligned patrol setup and records 1,800 native ticks with
normal ARM AI and combat. Further pathing work must use its evolving orders
and goal epochs; it is a diagnostic baseline, not a replay of presented
frames or a gameplay-performance measurement.

## Version 6 aligned-patrol checkpoint

The same-build comparison at `bffdc12eb` starts exactly 10,000 stock units
per side on Core Prime Industrial Area, seed 7, with aligned commanders,
unchanged positions, original group clicks and stock stances. ARM uses the
normal built-in native computer player, with no seated brain or humanized
controller. This headless diagnostic grants one tick at a time through the
application's native step/finish path. Its window hooks and tick batching
differ from the normal presented game; its timings are not gameplay FPS.

Version 6 repairs 2,088 out-of-playable formation points and another 5,598
points whose complete native arrival regions cannot fit the unit. It keeps
all 12,314 usable original points unchanged. Of the bounds failures, 1,878
also fail the padded native geometry; the other 210 intersect the hidden
right edge of the playable area. An independent audit of the actual first
retained post-tick native goals finds eligible arrival regions for all 20,000 version-6 units,
with no untested goals. Bounds, static eligibility, connectivity and traffic
remain separate questions.

Both versions complete 9,000 ticks, or five simulation minutes, with normal
victory and death rules enabled. Their outcomes remain ongoing at the cap.
Their census, event and crossing records through tick 1,800 reproduce the
earlier 60-second captures exactly, as do both canonical world states at that tick.
The two fixed rear cohorts start with 798 original units each; their IDs are
retained when actors die. Counts include actual births and deaths.

| Observation | Version 4 | Experimental version 6 |
| --- | ---: | ---: |
| Unique CORE centre-bridge inward crossers at 1,800 ticks | 2 | 34 |
| Unique CORE centre-bridge inward crossers at 9,000 ticks | 58 | 130 |
| Unique ARM centre-bridge inward crossers at 9,000 ticks | 0 | 0 |
| All CORE static-grid progress to original click, 9,000 ticks | 1,911.70 px | 2,002.37 px |
| Repaired 674 CORE rear units: same estimated progress | 1,046.77 px | 1,228.70 px |
| Unchanged 124 CORE rear units: progress to their own initial native goals | 3,216.16 px | 2,550.86 px |
| Mean native-grant thread CPU | 10.32 ms | 11.90 ms |
| p99 native-grant thread CPU | 15.49 ms | 16.33 ms |

Static-grid progress is an offline estimate from native footprint origins
and the initial native static masks: eight neighbours, no diagonal corner
cutting and quantized edge lengths. All 20,000 original actors have finite
estimates at the final endpoint; retired actors keep their own last-live
position. The estimate ignores moving traffic, later wrecks, fog, planner
limits and speed. It is not native path length. ARM replaces its original
click through AI orders from tick 300, and patrols can rotate legs, so
initial-target distance is not a complete measure of current-order
execution or AI efficiency.

The longer static-grid estimate shows about 17% more route-distance progress
for the repaired rear subset even though the earlier 60-second Euclidean
comparison looked worse. This supports a detour interpretation without
establishing native path efficiency. The 124 unchanged endpoints
still regress. At tick 9,000, 682 of the 797 surviving CORE rear originals
have the collision-blocked flag set in version 6; that snapshot alone does
not establish an attempted move. ARM's zero centre crossings must be read
alongside its AI-selected targets, rather than treated alone as a route
connectivity failure. These results do not establish smooth battle flow.

Combat is sustained: CORE/ARM shots rise from 6,953/100,408 in the control
to 14,957/140,836 in version 6. Final live counts are 9,479/9,783 and
9,423/9,662 respectively; deaths are 521/223 and 577/339, with 0/6 and
0/1 births. Both captures retain all detail rows without drops. All 61
physical snapshots, spaced five simulation seconds apart, report zero ownership/overlap errors;
the control separately reports one attached unit across 31 snapshots.
This is a sampled ground-ownership check, not swept safety between samples.

ARM rear goal exposure rises from 65.2% to 99.53% across the full capture;
over the final 1,500 ticks both versions reach 99.67%. This separates the
early retarget improvement from later shared strategic deadlines.

The six-tick early AI budget is read from the frozen source and verified
against saved state. Twelve early grants respect that spacing, costing
23.60 ms mean and 25.68 ms maximum whole-grant CPU. Ordinary strike deadlines
are separate. Each mode has one tick above 33.3 ms, its initial strike at
tick 300; maxima are 73.36 and 78.13 ms. Timings include bounded event
callbacks and exclude trace formatting and file writes. System and unrelated
process activity is recorded, so these are desktop observations rather than
isolated causal estimates. Event aggregates and per-tick hashes remain exact;
head/goal hashes describe post-tick observations, not commands issued and
replaced within one tick.

Earlier bounds-only checkpoints (`95884214e`, then `868aaf1bc`) improved CORE
centre crossings from 2 to 39 and 36 but left unusable terrain goals in
place. Their CPU comparisons were also contended by compiler or unattributed
Python work. They do not replace the same-build, usable-goal comparison
above. Version 4 remains current, version 6 remains experimental, and ray
tracing stays deferred while movement and presented frame rates improve.

## Current-view rendering checkpoint

Graphics remain independent of Enhanced Mode. Repeated poses share exact
local bounds and GPU geometry; classic Full shares bounded polygon-order
results and joins coloured faces with their neighbouring texture page.
Exclusive live instances can supply their vertices directly, while shared,
linked and temporary draws retain owned snapshots.

A windowless Metal check loads 20,000 real models and unit-script instances.
The turned view emits 12,481 markers and 2,066 instanced models, preserving
canonical picking. Both classic and turned images match the previous code
exactly, with the same paused world, camera and geometry. A separate native
check compares 478 borrowed/captured frames, bounds and picks, including
interpolation, attachments and water fixtures.

Paused profiles use 30 warmup and 60 measured frames per view on the M5:

| Wall-clock stage | Previous profile | Current profile |
| --- | ---: | ---: |
| Classic model/sprite emission and uploads | 205.77 ms | 100.82 ms |
| Classic first executor call | 54.99 ms | 11.21 ms |
| Turned Scene construction | 26.07 ms | 22.83 ms |
| Turned draw-list planning | 8.34 ms | 4.59 ms |

These separately recorded wall spans contain nested work and are not
additive or measurements of GPU execution time. Whole-frame capture also
includes mandatory readback. They do not establish gameplay frame rate;
Full preparation still exceeds the target budget. Ray tracing remains
future work while congestion and existing-view costs are addressed.

## Live 20,000-unit rendering checkpoint

The ordinary turned renderer now retains one exact terrain tile survey,
bounded to four MiB including its owned source buffers. Heights, tile
indices, black fog, crop, projection depth and floating-point controls are
checked before reuse. Atlas planning and terrain validation still run.
Scene tests compare the cached result with an uncached renderer across
source mutations and camera changes. This cache is independent of Enhanced
Mode. The foreground measurements below precede it and do not measure its
performance benefit.

The native game window is measured on the M5 in Check builds (`-O2`,
assertions enabled). Core Prime Industrial Area, seed 7, starts with exactly
10,000 units per side. Matching ARM and CORE commanders and troops occupy
their own banks, with ordinary patrol orders, stock weapons and health,
and a normal ARM computer player. The clock runs normally throughout.

Both views use Full rendering, anti-aliasing setting 4 and a physical
1920×1018 output. The camera centres on `(5120, 5120)` at zoom 0.25;
classic yaw/pitch are 0/0 and turned yaw/pitch are 20/-53. Each run requests
900 warmup and 900 measured simulation ticks, ending at a frame boundary.

The following native-window timings are provisional: foreground status was
not established. Their cached application-active state did not require SDL
input and keyboard focus, so they do not validate foreground gameplay FPS.

| Source | View | Presented FPS | 1% low FPS | Simulation ticks/s | Frames | Measured ticks |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `7e95c8d92` | Classic 2.5D | 2.910 | 1.98 | 12.853 | 204 | 901 |
| `391f4e5da` | Classic 2.5D | 5.510 | 4.36 | 24.632 | 202 | 903 |
| `7e95c8d92` | Turned 3D | 5.464 | 2.58 | 25.434 | 194 | 903 |
| `391f4e5da` | Turned 3D | 9.878 | 7.56 | 30.000 | 297 | 902 |

Both sides show movement in every reported interval and record firing,
damage and death events during each measured window, with no births.
Other game checks reached sampled per-process CPU peaks of 46.4% and 48.5%
during the earlier classic and turned runs; none was sampled during the
`391f4e5da` runs. Finite sampling does not prove an idle host. These are
observed desktop timings, not an isolated causal speedup estimate or evidence
of smooth foreground play. Paused readback profiles above measure preparation
and capture work, not gameplay FPS.

Source `76fbc3b16` requires SDL input focus and ownership of SDL keyboard
focus on every measured frame, alongside its existing Full-renderer,
normal-clock and presentation checks. A separate operating-system monitor
samples whether the game is active and frontmost approximately every 100 ms.
The completed runs below pass both SDL checks on every measured frame, with
active, frontmost OS samples covering measurement. The largest sampled gap
was 107.4 ms for the first classic run and 181.4 ms across the later four
runs. Samples establish no focus state between observations. Any startup or
warmup focus losses preceded measurement.

| Source | Verified view | Presented FPS | 1% low FPS | Simulation ticks/s | Frames | Measured ticks |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `df903e85f` | Classic 2.5D | 5.343 | 4.50 | 24.942 | 193 | 901 |
| `0a0d20bb2` | Classic 2.5D, models | 5.465 | 4.38 | 25.225 | 195 | 900 |
| `0a0d20bb2` | Turned 3D, models | 8.568 | 5.94 | 30.036 | 257 | 901 |
| `0a0d20bb2` | Classic 2.5D, strategic dots | 70.173 | 36.53 | 30.003 | 2,105 | 900 |
| `4c8e9744a` | Turned 3D, strategic dots | 35.134 | 25.41 | 30.000 | 1,054 | 900 |

The first classic run spans 36.12 seconds. The three `0a0d20bb2` runs use
the same executable and span 35.68, 30.00 and 30.00 seconds respectively.
The later turned-dots run spans 30.00 seconds on its recorded executable.
Requested game speed is 10; classic models adapt between 8 and 9, while
turned models and dots retain 10. Both sides move in every reported interval
and record firing, damage and deaths, with no births. Final live counts are
9,773/9,922 for the model views and 9,774/9,922 for dots. The model views
have one fewer live CORE unit than completed death events explain; pending
death before cleanup is consistent with this difference but was not counted.
No other game, compiler or profiler appears in the retained resource samples;
finite sampling does not establish an idle host or a causal speedup.

Dots uses the existing graphics-menu choices: units as dots, after zoom
1/3. Source `4c8e9744a` makes the turned view respect these choices too,
independently of Enhanced Mode. At zoom 0.25 it omits unit and feature
models and reduces effects and overlays; Full rendering and anti-aliasing
setting 4 remain enabled. These are separate quality results. Marker
visibility and picking need separate correctness checks: zero planned models
in a timing log does not establish either. The display refreshes at 60 Hz, so 70.173
presents/s cannot show more than 60 distinct refreshes/s. The table's 1% low
uses frame spans; classic dots' presented-interval 1% low is 34.01 FPS. Its tail
latency and unit congestion still prevent a whole-battle smoothness claim.

Classic model staging and upload average 91.49 ms per frame; dots reduces
that work to 0.07 ms. Turned model views still spend 30.90 ms building the
Scene and 25.15 ms planning the draw list. These CPU wall spans contain
nested work and are not GPU timings. Turned dots reduces Scene construction
to 5.43 ms and draw-list planning to 1.49 ms, but still spends 12.02 ms on
simulation and 15.58 ms drawing per frame on the shared application thread.
Both turned quality levels remain below 60 FPS.

Earlier turned attempts remain rejected: one lost input and keyboard focus
at tick 1,480 during measurement; another, in a locked session, failed
readiness at tick 934. Their partial data supply no FPS result. The later
completed turned run is verified separately. Version 4 remains current,
experimental version 5 has not passed its older march flow gate, and ray
tracing stays deferred while congestion and existing-view costs are addressed.

## Foreground strategic dots checkpoint

Three completed native-window runs use the same executable from a Check build at
`23e12bf71` on the M5 and the same Core Prime Industrial Area scene: 10,000
stock units per side, seed 7, 900 warmup and 900 measured ticks. Full
presentation uses AA4, the existing strategic dots choice after zoom 1/3,
zoom 0.25 and a 1920-by-1018 output. Rules 4 and 6 are selected explicitly
in the normal benchmark view file; graphics selection remains independent.

| View | Rules | Presented FPS | Ticks/s | Frame 1% low FPS | Final CORE / ARM |
| --- | ---: | ---: | ---: | ---: | --- |
| Classic 2.5D dots | 4 | 74.048 | 30.006 | 38.32 | 9,774 / 9,922 |
| Classic 2.5D dots | 6 | 67.358 | 29.996 | 34.13 | 9,937 / 9,987 |
| Turned 3D dots | 4 | 49.329 | 29.997 | 30.14 | 9,774 / 9,922 |

Every measured frame passed the SDL input and keyboard focus checks and
the guards for normal timing and the requested Full presentation. OS focus
sampling has a terminal qualification for classic v6: one final sample is
unknown. The last sample known to be valid is 0.112 seconds before the
process receipt ends; exact alignment with the final measured frame is
unverified. The 3D v4 run coincided with external ffmpeg load reaching
777.3% CPU, so its timing is externally contended. The 3D v6 attempt failed
readiness at tick 930 when the Mac locked; it supplies no FPS result.

No births occurred. Cumulative deaths were 226 / 78 under v4 and 63 / 13
under v6, exactly reconciling all final counts with the initial armies.
The classic v6 workload had more original units moving between samples:
the minimum ARM X/Z-motion count was 8,920, versus 130 under classic v4.
This is sampled movement, not proof of forward flow or bridge clearance.
Different motion and combat workloads and sampled host activity prevent an
isolated comparison of renderer speed. These runs precede terrain-demand
caching and establish no gain from that change.

The display refreshes at 60 Hz, so more than 60 presents/s cannot show more
than 60 distinct refreshes/s. The frame tails remain above the 16.7 ms
budget, and smooth bridge/rear flow remains unresolved. These observations
do not establish smooth 60 FPS play or full-model rendering at this scale;
version 4 remains current and ray tracing remains deferred.

A later foreground run at `8cfc42dd6` measures experimental version 6 in
the turned view, with the same Full/AA4 strategic dots and initial 20,000
units. It completes 901 measured ticks at a frame boundary in 30.07 seconds:
1,014 presented frames, 33.722 FPS, 29.964 ticks/s and an 8.84 FPS frame
1% low. The final CORE/ARM counts are 9,937/9,987, matching 63/13 deaths
and no births. Every measured frame passes the native focus guards;
587 valid OS samples follow startup, with no invalid terminal sample.

The terrain survey and plan average 0.18 ms. All 25 atlas-reset frames also
change ground geometry, and those frames include the 11 slowest frame
spans. This associates the slow tail with atlas and ground rebuild work;
it does not identify which material exhausted the atlas. Sprite work is
also substantial. Sampled host activity and the different version-6
movement and combat workload prevent a causal comparison with the earlier
version-4 timing. A previous attempt lost focus at tick 1,205 and is
excluded. These measurements precede the later minimap and recording changes.

## Experimental cardinal queue escape (version 13)

Version 13 is an explicit experiment based on version 10. It keeps
version 10's distributed routing, local following and cohort target assignment.
Animated body detail and deferred unit geometry are the Graphics choice
Lighter far units, not part of the version. It does not select version
11's target retention. Off and the earlier supported presets retain their
existing rules. Versions 9, 12 and 14 remain unsupported.

The existing mature retry phase may test the centres of the four cells
cardinally adjacent to the origin when a fresh route probe finds moving
traffic and the ordinary fan and reverse choices return no adjustment or a
target within the origin cell. Every ordinary choice that crosses the origin-cell boundary
is preserved. The additional candidates must pass the existing clearance
checks. If none is legal, the ordinary result is returned unchanged.
The experiment adds no saved state, random draws or retry clocks, and does
not relax swept movement or final footprint occupancy checks.

Select it explicitly for a comparison:

```sh
./run.sh --skip-intro --enhanced-mode-version 13 --hardware-acceleration=full
```

Saves, recordings and multiplayer agreement retain its separate rules
identity. Graphics choices remain independent. The supplied preset remains
version 4.

A matched 9,000-tick comparison on Core Prime Industrial Area started
10,000 units per side with the normal ARM computer player. Mean progress
toward the original Patrol destinations increased from 1,397.26 to 1,426.87
world units, and stationary exposure over the final 300 ticks fell from
24.78% to 22.23%. Original retired units retain their last alive endpoint
in that progress measure; paired survivors also improved on average on
both sides. Retired time does not count as stationary.

The result remains mixed. Paired ARM rear survivors lost 7.81 world units
of mean progress, and central ARM median progress also fell. Travel and
detouring increased. No passage through the specifically measured outer
mouth zones was recorded; this does not describe all bridge traffic.
The old version-10 controls repeated exactly across all 31 exports.
These headless results establish neither smooth bridge flow nor gameplay
FPS; tick timings were affected by other work. The default stays version 4.

## Experimental cohort target retention (version 11)

Version 11 inherits version 10's movement and cohort distribution.
Unit rendering is a Graphics choice, Lighter far units, not a version. It may retain a target named by a strict majority of
the cohort's current strike members when their compatible owned attack
heads still name an eligible enemy. Members without a compatible head
remain in the majority denominator and cast no vote. This uses the current
heads; it does not establish who originally issued an order.

The retained target's weighted squared-distance cost may be at most
12.5% above the fresh best target's cost. Earlier cohorts' final choices
contribute to those costs, including retained choices. Existing per-member
distance fallback, six-tick target inspection, 300-tick natural strike
cadence and recruitment limits remain unchanged. The policy derives prior
targets from canonical match state and adds no saved state or random draws.
Version 10 retains its original targeting behavior and rules identity.

For ordinary play, opt in explicitly:

```sh
./run.sh --skip-intro --enhanced-mode-version 11 --hardware-acceleration=full
```

The normal frontend opens and the rules choice lasts for this launch.
Loaded games and recordings keep their own version; multiplayer peers
must agree on the version and simulation rules. Graphics choices remain
independent. Animated body detail and deferred unit geometry follow
Lighter far units while Enhanced Mode is on. The supplied new-game preset remains version 4.
This separate identity does not establish better battle flow or 60 FPS;
those require fresh moving-and-fighting measurements.

## Experimental cohort targets (version 10)

Version 10 retains version 8's routing, following and swept ground collision.
Its separate rules identity selects deterministic spatial cohorts for AI
strike-target assignment. It does not enable the withdrawn version-9 passing
history policy. Ordinary saves, replays and multiplayer peers still name the
version selected for their match; versions 0 through 8 keep their rules.

Select version 10 for ordinary play without changing the supplied preset:

```sh
./run.sh --skip-intro --enhanced-mode-version 10 --hardware-acceleration=full
```

Start a new Skirmish or campaign in the normal frontend. The rules choice
lasts for this launch; saves and recordings retain their recorded versions.
Graphics stay independent: a match starts in classic 2.5D, Option+drag on
a Mac (Alt+drag elsewhere) turns it to 3D, and Option+R (Alt+R) returns to
classic. Full hardware acceleration and Graphics → Allow Full 3D Mode
are required to turn the camera. See [the command-line reference](command-line.md#display-and-sound).

For measurement, use `rules_version 10` in a gameplay benchmark
or `--benchmark-rules-version 10` in the native simulation benchmark. The
new-game switch without an explicit version still selects version 4.
A separate identity establishes
neither better army flow nor a frame-rate result; those require the actual
moving and fighting comparison.

## Experimental local following (version 8)

Version 8 has a separate save, recording and multiplayer identity. It starts
with version 7's native movement and static shared routing, including fine
format-3 fields, their unfinished construction prefixes and the same bounded
work and storage limits. It does not select a global crowd-cost field.
Its applied-leg following and static-only cached planning retain the actual
movement checks described above. Sustained 20,000-unit flow still requires
validation; the prototype comparison does not establish smooth bridge transit.
Version 4 remains the new-game default, and renderer choices stay independent.
Select version 8 explicitly with `rules_version 8` in a gameplay benchmark
or `--benchmark-rules-version 8` in the native simulation benchmark.

## Experimental distributed battle flow (version 7)

Version 7 retains version 6's patrol, retargeting and swept-movement rules.
The supplied default remains version 4. Off and versions 1–6 keep their
previous behavior; an existing save's resolved commands are not redistributed.
Select version 7 explicitly with `rules_version 7` in a gameplay benchmark
or `--benchmark-rules-version 7` in the native simulation benchmark.

Group commands resolve every member before issuing any order. Usable raw
formation points stay exact, including Y. Their complete native arrival
footprints are reserved first; only repaired destinations receive other
arrival spaces in a bounded, click-connected static component. Mobile
occupants do not enter this static placement check. Its canonical static
knowledge matches version 6 and is distinct from unknown-is-clear routing.
Connectivity to the click does not establish a route from each actor.
Unsupported classes, exhausted work or insufficient space retain the previous
fallback, with explicit assignment and exhaustion counts.

Reservation priority follows stable actor IDs. After all sites are admitted,
repaired actors and sites are matched spatially within each complete movement
and arrival class. Integer projections across and along the command's heading
retain lateral and forward order, with explicit ID and site tie-breaks. This
changes neither the reserved site set nor usable original points. Sorting
reuses the finished frontier storage and reports its comparison work separately.

The stock 20,000-unit scene preserves 12,314 usable points and assigns all
7,686 repairs without exhaustion, using one movement class and 312,736 bytes
of scratch per command. Placement legality alone does not establish flow:
the first pilot still reduced rear progress. Movement measurements must keep
the original rear cohorts, report changing goal episodes, and distinguish
geometric goal gaps from actual route distance.

Version 7 uses a two-cell shared graph. Wrecks can leave a passable gap that
the earlier four-cell graph misses, sending whole cohorts back to the slower
individual search. The finer graph permits up to 80 fields within an
8,388,608-node storage limit allocated before ticks. Clearing, expansion and
extraction budgets remain unchanged. Its 32-edge prefixes cover the same
distance as the earlier 16-edge guides. Saved fields
have a distinct format; earlier presets retain their original grids and
formats. This corrects the sampled static connectivity failure, but a full
battle must still validate construction latency and actual unit movement.

Newly completed fine fields stay available for at least 60 ticks before
admission can evict them. This gives the two-army workload time to poll a
finished route; it does not guarantee every possible participant layout
finishes its poll in that interval. The cache-state bound is 41,952,640
bytes. At 642 by 648 map cells it uses 41,611,200 bytes, plus 104,004 bytes
of restore-validation scratch. Unreleased version-7 pilot saves with the
earlier cache bounds are refused rather than silently losing navigation
state. Off and versions 1–6 keep their previous state formats and bounds.

When the first destination in a region cannot connect directly to a later
unit's goal, version 7 can follow that goal's branch of the existing tree.
It reads at most 512 ancestor nodes, turns at the shared ancestor and keeps
the ordinary 32-edge prefix limit. Published edges are rechecked against
current passability and player knowledge. Ancestor links share the existing
65,536 extraction credits with cell queries. Unsupported branches retain
ordinary prefix delivery; an optional check larger than the configured
budget cannot suppress that prefix forever. This adds no saved fields.

A nearby shared-route anchor can also point backward and repeatedly pull a
moving unit into a loop. Version 7 checks these reverse hooks on a staggered
31-tick cadence, removing one corner only when the unit's actual footprint
has a fully known clear path to the following point. The extra check is
limited to 64 terrain plots, preserves the goal and active search, and keeps
the final occupancy veto. Ordinary forward corners keep their existing
handling. In the eight-unit native queue regression, all units cross by tick
492 under the original 900-tick deadline; version 6 still crosses by 712.

Version 7 limits the synchronous wall-follow seed of an ordinary search to
256 steps. A truncated seed that made strict heuristic progress can guide
the existing credited search to a partial route while retaining the actual
goal. Consuming that guide requests another search; only actual goal
containment signals arrival. A seed without progress keeps a zero threshold.
Truncation alone reports neither contact nor failure. Map preparation and
clearing remain separate costs. Earlier presets keep their original seed behavior.

The bounded shared-route poll also identifies old, motionless, non-mobile
fallbacks for ordinary search. One of every four existing credited cursor
picks may serve one such unit per player per scan, ordered by search age and
unit ID. An out-of-order pick preserves the ordinary cursor. Live identity,
generation and owned goal geometry are rechecked before dispatch, which uses
the same search budget and lifecycle. A separate runtime pick phase survives
the heuristic's 150-scan usage reset, so sparse picks still reach their fourth
slot. No new saved scheduler state is added; ordinary searches already
rebuild from saved orders on load.

Built-in rally moves retain an existing, owned, nonterminal ground march,
including a patrol's waiting phase and complete queued return leg. Idle,
arrived, interrupted and unsupported orders still receive normal rally
commands. Built-in strikes also retain an unchanged live-target chase.
Manual commands and custom controllers keep their existing command semantics.
New strike recruitment is limited to 256 members per dispatch, including a
new group's seed. Existing members are not truncated. This spreads admission
cost at the expense of slower assimilation; it does not cap every future
large-group retarget. These policies add no persistent scheduler or save table.

In the measured 1,800-tick scene, the original tick-300 rally issues 9,979
moves to one centroid and consumes 68.69 ms inside an 87.48 ms tick. Retaining
the actual waiting patrols reduces that rally work to 0.65 ms and the tick to
17.39 ms. Later recruitment bursts still require separate measurement.

### Independent rendering work

The normal renderer's performance changes apply with Enhanced Mode Off or On.
Frame-local effect and light-mask atlas rows keep transient image variants
from repeatedly evicting retained ground. In the matched rules-6 Metal
diagnostic, atlas resets and ground revisions fall from 21 to zero; observed
FPS rises from 27.263 to 28.948 and frame p99 falls from 138.75 to 83.20 ms.
These runs use the same camera and graphics choices, without recording or
readback in the timed span. They do not establish 60 FPS or smooth movement.

Retained ground can also retain its spatial indices when geometry revision,
cuts, axes, triangle count and floating-point control agree. Pose capture
reads the three turn rates together and compares pieces while copying them,
without delaying capture or changing interpolation history. CPU reference
tests and windowless Metal comparisons check image equivalence; performance
must still be measured in the full battle. Ray tracing remains deferred.

Warmed unit-pose histories can be captured independently on the existing
drawing pool, with a synchronous join before rendering or script queries.
Slot validation, faults and cold-history handling stay on the calling thread.
Workers use preallocated rate storage and the caller's floating-point control.
A paired 20,000-instance capture check measured 3.816 ms for the original
serial capture and 1.785 ms with four total threads, including the caller.
Complete histories and rates matched. This excludes validation, simulation
and drawing, so it does not establish the full-frame gain.

Strategic markers can also be listed in ordered bands on the drawing pool
once their ordinary white material is already persistent and adaptive model
budgeting is inactive. Full models, reflections and card handling keep their
existing pass. The joined vertices, picking ranges and material order are
checked against the original renderer, including windowless GPU pixels.
Compact markers are drawn in every Enhanced Mode game, not from a version.
A same-input 20,000-marker Scene check measured 1.059 to 0.511 ms with four
total drawing threads; a one-thread control added about 0.026 ms. This is
Scene construction timing, not gameplay FPS. The model-stage timer sums the
parallel bands' durations and must not be read as their critical path.

Eligible prepacked surface effects can prepare ground indices before the
sprite worker pass, allowing independent triangle bounds to use the free
drawing pool. Query-free and uncertain material cases retain lazy handling.
The first real query checks its actual axes and floating-point control before
claiming preparation; a mismatch takes the original path.

A changed geometry revision can retain the old bucket layouts only after
recomputing and comparing every ordered world/card bound bit, the vertex
count, axes, card mode, floating-point control and exact reach. Queries then
read the current vertices. Any mismatch rebuilds the buckets normally. This
adds neither a geometry hash nor an assumption that revisions are redundant.

### Combined battle checkpoint

Source `ad3cb4085` was compared with the unchanged `e4d7b3d6` rules-6
baseline on the M5 Mac. Each foreground run started with 10,000 units per
side, used 900 warmup ticks and 900 measured ticks, and kept the normal
gameplay clock. Both views used Full Metal, AA 4, strategic dots after
one-third zoom, a 0.25 zoom and a 1,920 by 1,018 window on the 60 Hz external
display. The turned view used yaw 20 and pitch -53. No recording or readback
ran during measurement. Accepted runs passed native focus and operating-system
foreground checks; an interrupted turned-view attempt was excluded.

| View | Baseline FPS | Combined rules-7 FPS | Baseline / combined ticks per second | Baseline / combined frame p99 |
| --- | --- | --- | --- | --- |
| Classic 2.5D | 60.154 | 56.478 | 29.994 / 30.006 | 30.52 / 30.74 ms |
| Turned 3D | 27.425 | 42.339 | 29.843 / 30.004 | 134.16 / 35.69 ms |

The turned-view maximum frame fell from 233.22 to 62.55 ms, and atlas resets
fell from 22 to zero. The combined run retained its ground indices on all
1,257 frames that needed them; 13 frames made no index query. Battle paths
and casualties differ between presets, so this comparison does not isolate
the cost of any one renderer change. The classic view was slightly slower,
and the turned view still missed 60 FPS.

Separate native runs continued each preset through 9,000 ticks. Whole-tick
thread CPU p99 changed from 18.704 to 18.238 ms and the maximum from 83.547
to 35.214 ms. Ticks exceeding 33.333 ms fell from three to one. Mean CPU
cost rose from 13.691 to 14.287 ms; these changed battle trajectories do not
establish an overall simulation throughput gain. All 61 complete physical
audit epochs passed in each run, and live counts reconciled with births
and deaths throughout.

The longer movement comparison failed its flow gate. Unique units entering
the strict centre-bridge corridor fell from 130 to 23. Over the final 300
ticks, mean rear-cohort displacement changed from 42.70 to 20.68 pixels for
CORE and from 23.00 to 18.16 for ARM. ARM rear units stationary throughout
that interval increased from 12 to 133. These were predominantly chases
whose last native reach check rejected the target; they must not be described
as a successful combat standoff. Version 7 remains experimental while
static-edge route delivery and sustained rear movement are corrected.

A diagnostic replay reproduced that run's exact world and 27 deterministic
observation streams. All 133 stationary ARM rear units had fine-grid paths
through the final terrain, but none connected through the four-cell graph.
Their 2,120 recorded shared requests returned 133 pending and 1,987 fallback
results, with no usable publication. The saved field exactly matched a
freshly rebuilt graph; neither a stale field nor an exhausted extraction
budget caused this failure. A two-cell graph reconnected every sampled
origin on that same map. These are connectivity checks, not a replacement
for measuring the subsequent battle.

### Finer-grid and parallel-capture checkpoint

Source `349fdeef6` combined the two-cell graph, reverse-anchor recovery and
parallel pose capture. Both presets completed a fresh 9,000-tick comparison.
Version 6 retained the exact earlier world digest, random streams and
recorded trajectories. Version 7 passed all 61 complete physical audit
epochs and retained the same legal command destinations, but still failed
its flow gate: centre-corridor crossings fell from 130 unique units to eight,
and final-300-tick mean rear displacement was 23.32 pixels for CORE and
9.72 for ARM, versus 42.70 and 23.00 in the reference. All 191 stationary
rear units had statically reachable arrival areas.

At the final tick, all 20 fine fields were still building. The common key
needed by all 798 sampled ARM rear units was absent; 604 CORE rear units
also lacked their field. These observations motivated the larger bounded
cache and completed-field protection above. Their full-battle effect still
requires a new measurement. Periodic refresh work also competes for the
unchanged construction budget.

Whole-grant process CPU p99 changed from 19.647 to 22.153 ms and the maximum
from 93.971 to 32.563 ms; ticks exceeding 33.333 ms fell from two to zero.
Mean process CPU rose from 12.202 to 13.769 ms. Wall-time ticks exceeding
33.333 ms increased from 28 to 54. The slowest wall tick took 219.369 ms
while using 22.105 ms of process CPU. Process CPU includes worker and
background threads; these figures do not establish uninterrupted 30 Hz play.

Fresh uncaptured foreground runs used the same cameras, Full Metal AA 4,
strategic-dot overview, display and 900-plus-900-tick contract as above.
Both accepted runs passed native and operating-system focus checks. Two
earlier classic attempts failed focus checks and contribute no FPS result.

| View | Reference FPS / ticks per second | `349fdeef6` FPS / ticks per second | Reference / candidate interval p99 |
| --- | --- | --- | --- |
| Classic 2.5D | 60.154 / 29.994 | 63.481 / 29.991 | 30.52 / 30.34 ms |
| Turned 3D | 27.425 / 29.843 | 50.895 / 30.017 | 134.16 / 31.77 ms |

The turned-view maximum interval fell from 233.22 to 42.75 ms, with zero
atlas resets. The classic maximum rose from 39.73 to 48.67 ms. These are
combined engine and changed-battle observations, not an isolated renderer
comparison. The later cache-capacity changes are not measured by these
foreground results. Smooth movement and sustained 60 FPS in 3D remain unmet;
version 4 stays the supplied default and ray tracing remains deferred.

### Route-capacity checkpoint

Source `89a336682` retained 80 fine fields and protected newly completed
fields. Its fresh version-6 1,800-tick prefix matched the previous roster,
orders, world state, census, random streams and common observation epochs.
The version-7 battle completed 9,000 ticks with no physical audit errors,
the same legal destination assignments and no protective stop.

All 133 units in the originally stationary ARM cohort survived and moved
during the final 300 ticks; all acquired a nonzero search timestamp. None of
the 796 surviving original rear ARM units was stationary throughout that
interval. Seven of 798 CORE rear units remained stationary, versus four in
the earlier version-6 reference. Mean rear displacement was 33.41 pixels for
CORE and 31.64 for ARM, versus 23.32 and 9.72 in the smaller-cache pilot and
42.70 and 23.00 in the version-6 reference. Considerable circulation remains;
movement alone is not proof of an efficient route or a successful attack.

Copied-cache diagnosis found three stationary CORE units whose reachable
goals lay on a different branch from the regional seed. The requested-goal
connection above returns valid goalward paths for all three captured cases,
including fractional-footprint and continuation checks. The other four had
no free adjacent full-footprint origin and held the same guide proposed by
the cache. These checks do not establish the outcome of a new full battle.

Whole-grant process CPU averaged 12.905 ms, with 16.623 ms p99 and 26.942 ms
maximum; no process-CPU tick exceeded 33.333 ms. Wall p99 was 17.213 ms and
the maximum 59.617 ms, with seven ticks over that budget. The final 300 ticks
had a 22.962 ms wall maximum. These are native-grant measurements, not a new
foreground FPS result. The marker and requested-goal changes after this
checkpoint still need their combined battle measurement.

The crossing metric used in these comparisons covers the north and south
bridge bands at the map's central X plane, not its literal middle point.
On the current Core Prime Industrial Area fixture, native terrain at world
position 5,120 by 5,120 is too deep for either stock ground class, including
a diagnostic one-cell footprint. No unit footprint or map was changed.
That coordinate test does not identify a bridge in an earlier Metal Heck
picture; matching the map and camera is necessary before diagnosing it.

### Requested-goal and marker checkpoint

Source `0db3fb8dc` combined the requested-goal branch connection and parallel
marker listing with the larger route cache. Its fresh version-6 1,800-tick
prefix again retained the earlier world state, census, random streams and
common observation epochs. Version 7 completed 9,000 ticks and all 61
physical audit epochs without overlap, footprint or projection errors. The
same 12,314 valid destinations stayed exact and all 7,686 repairs retained
their admitted actors and legal site sets.

All seven CORE units tracked as stationary at the route-capacity checkpoint
survived and moved during the final 300 ticks. The three regional-goal alias
cases traveled 210.8, 226.6 and 116.2 pixels and made positive geometric
progress toward their current goals. Their goal episodes changed during the
battle; this does not show completion of the previously stalled orders.

Overall flow remains mixed. Mean rear displacement fell from 33.41 to 29.97
pixels for CORE and from 31.64 to 27.28 for ARM. The original ARM cohort now
has 798 survivors rather than 796. Four CORE and three ARM rear units were
stationary throughout the final 300 ticks, compared with seven and zero at
the route-capacity checkpoint and four and twelve in the version-6 reference.
All 133 members of the originally stalled ARM cohort survived and moved.
North/south bridge-band inward crossings
were ten unique CORE units and one ARM unit, compared with eleven and one at
the route-capacity checkpoint. These are neither a literal middle-bridge
measurement nor a successful whole-battle flow gate. At tick 9,000, 19,436
units remained alive: 9,646 CORE and 9,790 ARM, accounting for 565 deaths and
one ARM birth without pending cleanup.

Whole-grant process CPU averaged 12.956 ms, with 16.513 ms p99 and a 34.467 ms
maximum. One process-CPU tick exceeded 33.333 ms. Wall p99 was 17.148 ms,
with an 81.222 ms maximum and five ticks over that budget. The largest CPU
tick issued no AI commands and had negligible measured AI work; its remaining
cost is not attributed to routing by these observations.

Both fresh foreground views passed native and operating-system focus checks,
using the same Full Metal AA 4 strategic-dot overview, display, camera and
900-plus-900-tick contract as the earlier checkpoints. No recording or
readback ran during measurement.

| View | Reference FPS / ticks per second | `0db3fb8dc` FPS / ticks per second | Candidate interval p99 / maximum |
| --- | --- | --- | --- |
| Classic 2.5D | 60.154 / 29.994 | 61.904 / 30.002 | 29.93 / 33.17 ms |
| Turned 3D | 27.425 / 29.843 | 56.083 / 30.009 | 29.71 / 70.53 ms |

The turned view still contains one interval over 50 ms and does not sustain
60 FPS. These are combined engine and changed-battle measurements, not an
isolated marker-rendering gain or a close-up model benchmark. Version 7
remains experimental, version 4 remains the supplied default, and ray
tracing remains deferred.

### Rejected demand-refresh checkpoint

Source `bf1d09c08` tested preferential refresh of fields whose stored edges
had failed live validation, with every fourth refresh slot reserved for
background changes. A copied-cache comparison delivered clear guides for
three captured ARM cases 740 ticks earlier without increasing work limits.
That check advanced only the cache over frozen terrain, not the battle.

The subsequent 9,000-tick battle passed all physical and destination audits
but failed the flow comparison. Stationary rear survivors increased from
four CORE and three ARM to sixteen and eight. Fourteen were waiting for
unvisited anchors in building fields and three hit stale stored edges.
Four more had no statically clear arrival origin for their current goals;
the remaining three had usable guides. Rear displacement changed from
29.97 to 20.07 pixels for CORE and from 27.28 to 39.05 for ARM. North/south
bridge-band unique crossings remained ten and one.

Both runs consumed exactly 18,190,336 expansion and seeding credits.
The candidate performed fewer periodic refreshes, while nonempty admission
replacements increased from 88 to 307. Its building fields were recently
requested; cold fields alone do not explain these waits. The demand-refresh
policy and its new saved dirty state were reverted, restoring the
requested-goal checkpoint above. Version 7 remains experimental.

Build-only terrain-index diagnostics remain available in benchmark logs.
They record prior validity, exact cache keys, cache-instance build attempts
and elapsed bounds/world/card construction spans. Records are bounded,
collected after drawing jobs join and printed after measurement. They change
neither rendering decisions nor the existing frame-log schema. The last
accepted foreground FPS figures above predate these diagnostics; no FPS
result is inferred for the rejected candidate.

### Ground-index preparation checkpoint

A windowless, sparse real-engine comparison of the later renderer used the
same 20,000-unit rules-7 battle, 1,800 native ticks and 60 Full Metal draws.
All 60 complete RGB images and sampled world, census, RNG and geometry
records matched. A draw containing three revision-only index builds changed
from 33.747 to 8.363 ms of index work: exact bounds verification retained
the buckets and removed both constructor spans. Its first cold build changed
from 11.676 to 11.507 ms, which does not establish a cold-build improvement.

These director draws use rendered units, sparse effect history and GPU
readback; they are neither the normal strategic-dot overview nor gameplay
FPS. Separate GPU tests compare complete RGBA frames. A procedural-terrain
component comparison measured about 0.7–1.0 ms less Scene work, with identical
outputs, but did not reproduce the earlier 15 ms cold bounds phase. No
uninitialized-storage optimization was added. The combined pathing and
foreground battle measurements remain a separate gate.

### Partial-guide and search-service checkpoint

The retained search changes keep capped partial guides and a preferred-pick
phase that survives heuristic resets. Two later experiments were removed
after full-battle comparisons. Allowing collision recovery from footprint age
improved a small jitter fixture but reduced ARM rear displacement from 28.63
to 8.31 pixels in the matched battle. The ordinary motion-age recovery gate
remains in use.

A second experiment allowed two representative fields for disconnected
destinations in one region. Over copied terrain, it delivered safe guides to
all 150 captured ARM cases within 130 ticks. That did not predict the full
battle: removing only this implementation increased CORE rear displacement
from 21.11 to 47.98 pixels and north/south bridge-band unique crossers from
12 CORE and 1 ARM to 18 and 9. ARM rear displacement changed from 29.25 to
22.70 pixels, so whole-battle flow remains mixed. These counts describe the
current Core Prime Industrial Area fixture, not a verification of the earlier
Metal Heck image or the literal water-covered centre.

In the isolated overlay run, the retained configuration completed 9,000 ticks with 9,658 CORE and 9,741
ARM units alive, 604 actual deaths, three ARM births and no pending deaths.
All 61 physical audit epochs passed, and command destinations retained the
same 12,314 original points and 7,686 repaired sites. Process CPU time was
12.507 ms mean, 15.934 ms p99 and 30.573 ms maximum, with no ticks over
33.333 ms. The representative-field run measured 13.516, 28.810 and 38.905
ms respectively, with seven slow ticks. Background host workloads differed;
this comparison does not isolate a particular helper or allocation cost.
Both consumed the same 18,190,336 expansion and seeding credits. The field
experiment and its new fixtures were removed; its copied-cache proof remains
an experiment, not a whole-battle success.

The discarded field candidate's normal foreground measurements were 64.138
FPS in the classic view and 53.670 FPS in the angled view, both at 30.002
simulation ticks per second. These were uncaptured strategic-dot overviews
on the external 1920-by-1018 display, Full Metal with AA4, seed 7 and 10,000
units per side, with 900 warmup and 900 measured ticks. They are not FPS
measurements of the restored configuration.

The cold terrain-index cost remains variable. Earlier normal foreground
runs measured 55–70 ms builds, while the later field candidate measured
13.343 ms. An exact 613,332-triangle snapshot replay used the same 72-byte
vertex stride, axes, floating-point controls and byte-identical hot callback
but measured about 1.9 ms for parallel bounds. No subnormal or nonfinite
coordinates were present. This does not establish a math or allocation fix;
GPU load, residency and scheduling were not reproduced offline. No new
floating-point, allocation or culling policy was adopted from that result.

### Retained production repeat

A fresh production build at source `0a40ac526` repeated the same 9,000-tick
battle. Its final world digest was `5e61a009313942ed`; all 38 deterministic
output files matched the accepted no-field run byte for byte. The earlier
preset's 1,800-tick compatibility prefix also matched. The generated-source
inventory was extended for this freeze; the earlier freeze's missing generated
server-source bytes cannot be reconstructed from its pinned object files.

The fresh repeat measured 16.213 ms mean, 30.391 ms p99 and 42.388 ms maximum
process CPU time, with 22 ticks above 33.333 ms and an 82.456 ms wall maximum.
Identical recorded behavior did not produce identical timings. The host was
not isolated, and residency, scheduling and thermal causes remain unproved.
The earlier overlay's zero slow ticks therefore do not establish sustained
headroom on this laptop.

Fresh normal foreground runs used that production build, with no observer,
capture or readback, and the same AA4, resolution, zoom and strategic-dot
settings described above. Both passed native and OS focus guards through
all 900 measured ticks after 900 warmup ticks.

| View | FPS | Simulation ticks per second | Interval p99 / maximum |
| --- | --- | --- | --- |
| Classic 2.5D overview | 64.707 | 30.003 | 27.69 / 39.66 ms |
| Angled 3D overview | 56.025 | 30.013 | 29.12 / 38.58 ms |

Neither run had an interval over 50 ms. The angled view's cold index build
measured 13.266 ms, including 4.592 ms of bounds work, without changing the
renderer implementation between these runs. The angled view remains below
the 60 FPS target, and rear-army flow remains mixed. Ray tracing is deferred.

### Root capture and script dispatch checkpoint

Every Enhanced Mode game captures only root movement while the graphics
policy draws every unit as a strategic dot. Unit scripts, motion, queries
and combat continue to run. Returning to models immediately captures the
current rig, including while paused; piece interpolation resumes after the
next complete tick endpoint. That first interval uses the current piece
poses, without inventing earlier animation. Those frames match a full
capture, on the processor, in Full and on the card, so root capture is on
for every Enhanced Mode game. Enhanced Mode Off, director rendering and
debug order lines retain complete capture. Lighter far units changes how
far units' bodies are drawn; it does not choose this capture.
Renderer selection stays independent.

A component check used 20,000 distinct procedural VM/model instances with
310,000 pieces and four total threads. Six paired blocks of eight captures
measured 1.791 ms mean wall time for complete capture and 0.264 ms for root
capture. First model priming measured a 2.342 ms median and 4.269 ms maximum;
priming with retained storage measured a 1.501 ms median. Root endpoints,
current rigs, sampled VM state and immutable inputs passed comparison. These
measure capture work, not a stock-map frame or gameplay FPS. Benchmark parsing
also accepts a same-tick rig capture with zero new root captures, while keeping
its frame, aggregate and triplet bounds.

The VM avoids dispatching already-stopped context slots. Active contexts,
sleeping and waiting contexts, budgets, immediate callbacks and piece motion
keep their existing order and behavior. A 528-tick comparison reduced context
entries from 4,224 to 152 with identical state, results and callback traces.
This removes empty work without skipping scripts.

A fresh retained build at source `c6b50e170` completed the same 9,000-tick
battle and reproduced world `5e61a009313942ed` and all 38 deterministic files
byte for byte. Earlier-preset compatibility and all physical checks passed.
Process CPU time measured 11.925 ms mean, 15.934 ms p99 and 25.064 ms maximum,
with zero ticks above 33.333 ms. Wall time reached 36.156 ms, with one tick
above that budget. The host was not isolated and the diagnostic observer now
also records resources and bounded head traces; these timings do not isolate
the VM change's contribution. Rear displacement and crossing indicators are
unchanged from the retained production repeat.

Three additional local-steering trials passed focused and physical checks
but failed the whole-battle flow criteria. All three were reverted:

| Trial | CORE rear NET | ARM rear NET | Inward crossing indicator, CORE / ARM |
| --- | --- | --- | --- |
| Retained physics | 47.98 | 22.70 | 18 / 9 |
| Retain progressing unfinished passing legs | 29.57 | 16.32 | 14 / 3 |
| Extend completed passing history | 34.73 | 36.09 | 15 / 4 |
| Reject passing targets in the current cell | 16.89 | 15.30 | 14 / 8 |

NET is mean world displacement during the final 300 ticks for the fixed
original rear cohorts. The crossing indicator checks inward X=5120 side
changes every tick in the north/south bridge bands, across the original
20,000-slot domain with generation changes tracked. It excludes births
outside that domain and does not establish complete bridge traversal or a
crossing of the literal map centre. The scene remains Core Prime Industrial
Area, not Metal Heck.

One sampled ARM unit repeated a moving loop inside a single footprint cell
every 60 ticks. Rejecting its in-cell targets removed the loop but left it
stationary; the whole army also moved less. That result requires better
physical-blocker evidence, rather than treating fewer local turns as better
flow. The current retained build includes none of those three steering
policies.

The Mac remained locked for the new foreground runs. The earlier FPS table
belongs to source `0a40ac526`; it is not a measurement of `c6b50e170`. Actual
FPS in both views and the 60 FPS target remain unverified for the new build.
Ray tracing remains deferred.

## Remaining work

1. Measure simulation phases and AI work separately. Reduce the long ticks
   before making a whole-battle smoothness claim. Compare the same seeded
   workloads on the Mac and a gaming desktop.
2. Validate the current preset on sustained installed-map battles and extend it where
   measured stalls remain. The synthetic counterflow, crossing and choke
   fixtures check arrival, determinism and zero overlap; those results alone
   do not establish that every installed-map group finishes its trip. Preserve
   Off and earlier presets, and run the three-machine route burst plan
   ([Network transport](#network-transport)) in the suite.
3. Separate simulation from presentation. The current application ticks and
   renders on the same thread: a 27 ms tick can miss a 16.7 ms frame deadline.
   Keep mutable World ownership on the simulation thread, queue commands for
   deterministic ticks, and publish bounded immutable presentation snapshots.
   Render poses, effects, visibility and HUD data from those snapshots.
4. Continue scaling the existing Full renderer. Reduce off-viewport model
   emission and remaining Scene preparation, and validate moving views with
   bounded detail and shadow work. Measure frame preparation, upload bytes
   and GPU time separately.
5. Add Ultra as an independent Graphics option. The current SDL GPU path has
   no acceleration-structure or ray-dispatch interface. Introduce native
   Metal and Vulkan ray-tracing backends around shared scene/material data.
   Start with hybrid sun shadows and ambient occlusion, then reflections,
   with temporal reconstruction and denoising. Probe support and fall back
   to Full on unsupported hardware. Preserve fog secrecy and simulation
   results with Enhanced Mode both Off and On.

The working targets are 33.3 ms per simulation tick at 30 Hz and 16.7 ms per
rendered frame at 60 FPS. Test their tail latency and sustained behavior
together; meeting either budget alone is insufficient.
