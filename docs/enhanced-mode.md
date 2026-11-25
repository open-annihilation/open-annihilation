# Enhanced Mode

Enhanced Mode is an optional set of engine rules. Choose it in Settings,
under Engine Limits, before a game starts. It starts Off. The choice
applies to the next game; a match that is already running keeps the rules
it started with. The selected mod keeps its units, its files and its other
rules.

Off plays as Total Annihilation 3.1c. The same profiles, the same
simulation hash, the same saves and the same bytes on the wire. On plays
Enhanced Mode, Revision 1, the set this release ships. A later release
plays that revision as this one does.

Graphics settings are separate. Enhanced Mode does not pick a renderer,
and players in one match may choose different graphics settings. Lighter
far units, under Graphics, is off unless you turn it on. While it is off,
an Enhanced game is drawn as a game with Enhanced Mode off is drawn. While
it is on, and the match is Enhanced, units far from the view are drawn
with simpler animated bodies on the graphics card, and some of their
geometry is prepared later. The option changes nothing while Enhanced Mode
is off. Ray tracing is not part of Enhanced Mode, and this release does
not ship it.

## Revision 1

Revision 1 is seventeen features, played together. A click gives the same
order it gives with Enhanced Mode off. What changes is how large an army
may be, how ground units carry a move out, and how the built-in computer
player keeps and spreads its strikes. The machines of a match agree on the
set, and that agreement is part of the simulation hash. A ground step is
accepted or refused where the step ends.

**Larger armies.** A match may field 10,000 units for each player who
takes part, and at most 32,767 units and 32,767 shots in the whole match.
A spectator does not take a unit range. Ranges are allocated only for the
players who take part, so two players can field 10,000 units each. Scanner
and jammer contacts reuse one prepared picture for that player's pass,
instead of building it again for every query.

**Local traffic.** Ground units steer around other moving units for a short
while, and a unit that has been blocked only briefly waits before it asks
for another full route. The order you gave stays in place. After a run of
blocked ticks the unit plans again.

**Shared routes.** Nearby ground destinations share a coarse route, built
on a fine grid. Each edge is checked again against the ground and against
what that player can see before a unit follows it. A corridor too narrow
for that grid uses the ordinary route search.

**Passing a corner.** A blocked ground unit may pass the next corner of its
route when its footprint has a clear line beyond that corner and the ground
there is known.

**Replacing a lost strike.** The built-in computer player replaces a strike
target that is lost or dying, instead of holding the order until the usual
refresh.

**Who searches first.** When a shared route cannot be used, the unit in
that player's army which has waited longest searches first, and an ordinary
search stays bounded.

**Keeping a chase.** When a computer strike refreshes, a unit that is
already chasing keeps that chase instead of starting again.

**Keeping a march.** When a computer rally refreshes, a ground unit that is
already marching or patrolling keeps that order, including a patrol's
return leg.

**Routes through a crowd.** A route is planned through the cells where
moving units stand, so a crowd is not treated as closed ground.

**Steering reused.** While a unit's steering inputs are unchanged, movement
reuses the projection it already made, and the unit does the same thing.

**Exact nearby searches.** A search for a nearby target, a sighting or an
order finds the units a full scan of the same area would find. An air
search waits until an aircraft is a goal.

**Strike recruits in batches.** A computer strike takes on new attackers in
batches, so one refresh does not start every chase in the same tick.

**Spread strike targets.** The attackers are split into groups by where
they stand, and each group picks a nearby target, so they do not all chase
one unit. New chases in a group start a limited batch at a time, and the
rest a few ticks later.

**Keeping a group's target.** At the strike's regular dispatch, a group
keeps a target it has already checked while that target still costs close
to the best one. A later refresh that finds a chase finished picks a new
target.

**Stepping out of opposing traffic.** A unit blocked for a long time by
traffic that crosses its path, or comes against it, may step into a clear
neighbouring cell. Behind traffic going the same way, it waits its turn.

**A stuck unit waits.** A ground unit that is stuck waiting for its next
try does not read its goal again until that try. What it does is the same.

**Quieter route maps.** Route maps that ignore moving units are left as
they are when a moving unit clear of the map's right and bottom edges
moves, settles or dies, because those cells would come out the same.

## Revisions stay frozen

A revision is a fixed set of features. This build plays Off and Revision 1,
and Revision 1 is the highest it plays. A shipped revision does not change:
a later build plays Revision 1 exactly as 0.9.3 does. Changing what one of
its features does means a new feature, with a new id, that only a later
revision includes. An id is never reused. The code asks only whether a
feature is on.

A skirmish, a campaign or a benchmark may play Revision 1 with a feature
list: names, or `+name` and `-name` against the revision. The match records
the set it actually plays. A name this build will not play is refused. A
battle room plays a revision, the whole set, and refuses a feature list.

## Battle rooms

In a battle room the host decides. The switch sits in the strip right of
BATTLE ROOM, labelled Enhanced Mode, and reads Revision 1 or Off. It starts
from the host's own setting. Every machine from Open Annihilation 0.9.3 on
announces the highest revision it can play, whatever its own setting says.
While the switch is on, the room plays the lowest of those revisions across
every seated human and watcher. A computer player counts as the machine
that runs it. A machine that announces none, 3.1c or Open Annihilation
0.9.2 and older, makes the room Off, and the room names that player. A
machine whose record has not arrived yet changes nothing and holds START.

The host's record announces the room's revision, and leaves it out while
the room is Off, so an Off room sends what it sent before. Every machine,
the host included, plays the room's revision, never its own setting, and
START waits until every machine's simulation hash matches the host's. Each
change of the room's revision posts one line: `*** Enhanced Mode: Revision
{n}.`, `*** Enhanced Mode: Off.`, `*** Enhanced Mode is off: {player}'s game
doesn't have it.` or `*** Enhanced Mode is Revision {n}: {player}'s game
has no later revision.` While the room is waiting on a machine, the line
beside START reads `Waiting for {player}: Revision {n}`.

A guest sees the switch grayed. Resting on it shows why: the launch keeps
Enhanced Mode off, or only the host can change it. A launch that locks the
room's options keeps the switch Off and grayed.

A Boneyards room starts Off. The player who created the battle may turn it
on, a matchmade battle and a QuickPlay battle included. A launch that locks
the room's options keeps it Off. No battle room loads a saved game in this
build. A save loads at the revision the save holds, and every machine has
to have that revision.

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
A match with local traffic flow over shared routes, such as Revision 1,
saves five owner-local navigation tables: shared fields, the current
static-map revision, temporary traffic heads, actual-progress clocks and
the shared-route scan clock. The scan clock prevents a load pass and a
regular pass at the same tick from polling the unit ranges twice. These
tables stay outside shared world and trace hashes.

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

- Machines: the Linux arm64 package hosts, and the Windows x64 package
  (under Wine) and the Linux x86_64 package join, each in its own container
  on one network on the Mac Studio. The macOS build cannot reach that
  network, so every machine's recording is played back on macOS instead.
  Every machine plays Enhanced Mode, Revision 1.
- Starting each machine: the suite starts every engine from the command
  line, as its other crossplay cases do, and readies and starts the battle
  room as they do. The host runs `--host --map "King of the Hill"
  --start-positions fixed --enhanced-mode --net-route-burst 1500`, and each
  guest `--join ADDRESS --side arm --net-route-burst 1500`: a host started with launch settings
  such as `--map` admits only guests whose start is a launch too, which a
  launch setting such as `--side` makes it. Each machine also writes
  `--trace-units FILE` and `--net-record FILE`. A host in Enhanced Mode
  opens its room with a unit limit that holds the movers beside the
  commander.
- Stage: nothing more is clicked. Once the match starts, each machine gives
  its own player 1,500 movers, and at tick 300 sends them all one row south
  in one group move ([`--net-route-burst`](command-line.md#network-play)).
  Every machine lays out the same blocks: one for each player, by the order
  of the players' unit ranges, clear of every start position of the map
  and of the other players' blocks. In a block the movers stand loose, in
  staggered rows, and a mover is made only where it can walk its march, so
  that every mover can reach its goal and stand still there. The match then
  plays on until every mover has stood still and a round of full unit
  records has passed: about 2,700 to 3,000 ticks for 1,500 movers a player,
  and 1,200 for 1,000. King of the Hill is the one installed map with room
  for three blocks at that size; no map has room for three blocks of
  3,000.
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
  - at the end, on a tick common to every machine. The machines drift
    apart (one guest ran about 700 ticks behind its host), and a copy's
    health arrives with its owner's full record, sent on the owner's clock,
    so each machine first settles on its own clock: every one of its own
    movers has stood still for 300 ticks, a whole round of full records
    (the room's unit limit in ticks) has passed since a unit last appeared
    on it, and it has reached tick 2,100, 1,800 ticks after the move. Once
    every machine has settled, all play on to the first multiple of 300
    after the latest machine's settled tick, plus 300, and there every
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
  ([testing](development/testing.md#network-play)). Its two programs keep
  step, so it compares player, type and cell at the last tick both have read
  once both have settled, and health once a full record of every copy has
  also arrived on both: a commander's first full record rides on its
  owner's tick 1,020, and a lost part carrying it is sent again, so it can
  come later than the settle.
- Plus an Off run of the same stage, whose wire is byte for byte the base's
  (the capture compare of the crossplay cases), and a battle room with a
  3.1c machine, which plays Off with unchanged bytes.

## Measuring battles

The simulation-only benchmark runs repeatable idle, marching and combat
workloads with a fixed seed and writes a JSON report. `group-march` gives
both armies distinct, footprint-checked arrival positions in shared
destination regions. Its report counts unique units making progress,
arrivals and stalls, including the initially near, middle and far thirds of
each army. For example:

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
Use `--enhanced-revision 1` or `--enhanced-features LIST` with Enhanced Mode
to compare Revision 1 with a feature set on the same seed; LIST is feature
names, or every item `+name` or `-name` applied to the revision, and the
match records the set it plays. The report includes simulation phase timings
and cumulative route, local-flow and targeting work; percentiles of individual
phases are not additive.

`enhanced_revision` is the revision the match played, 0 for Off.
`enhanced_features` names the set the match played, in id order.
`enhanced_features_spec` is the text of `--enhanced-features`, empty without
it. `data_faults` counts distinct data-fault lines. `staged_battle.extra`
holds the named numbers a gate compares: `swept_misses` counts the ground
steps whose path passed through ground or a unit that blocked it through the
whole tick ([battle benchmark](../src/app/battle-benchmark/README.md)).

`oa-enh-gate` takes `--revision N` (it passes `--enhanced-mode
--enhanced-revision N`), `--features LIST` (it passes `--enhanced-features
LIST`), `--noise PATH` (the noise file), `--noise-out PATH` (where
`--noise-from` writes) and `--keep-trace` (gzip each battle's per-tick
stream into `--out`). Its report adds `.all` totals over both players,
`attackers.pN`, `faults`, `work.NAME` and `extra.NAME`.
`oa-enh-reference` plays Off and Revision 1. `--features LIST` compares that
set with `--against off` or `rN`, `--against-features LIST2`, or `--twice`,
and plays `ai-patrol` through the benchmark's `stage` scenario.

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

The aligned patrol is such a stage: 10,000 stock units a side, the
commander included, in aligned columns on Core Prime Industrial Area. Each
army is given one patrol, Fire at Will and Maneuver. CORE is the local seat
and ARM is the normal computer player. The seed is 7 and the run is 9,000
ticks. Expanded Confluence and Dire Straits use the same population, seed
and length. The armies fill either half and patrol toward the other army.
ARM is the local seat and CORE is the computer player. Expanded Confluence
is crossed at fords. Dire Straits meets through narrow straits.

### Reference fixtures and gate battles

`oa-enh-reference` replays three short fixtures for Off and for Revision 1:
the `group-march` and `combat` benchmarks at 1,000 units a side, and a
headless stage of the aligned patrol's front columns (1,356 troops a side,
the most Off's unit limit holds) with the normal computer player. Each runs
1,800 ticks at seed 7. It compares each run's `--trace-digest` stream with
the recorded reference and prints `same` or the first tick that differs.

`oa-enh-gate` runs the gate battles one at a time, on the Mac Studio: the
aligned patrol, Expanded Confluence and Dire Straits, each as above, and
the synthetic counterflow, crossing and choke fixtures, which check arrival,
overlap and that two runs agree. It writes `report.json` and `report.md`.
Against a stored run it prints each metric's change and whether the change
exceeds the recorded run-to-run noise.

## Large battles

Measured on a Mac Studio (Apple M2 Ultra), at Revision 1:

- In the battle benchmark (combat on Core Prime Industrial Area, seed 7,
  no computer player, nothing drawn), the slowest 1% of ticks take 3.4 ms
  at 2,000 units, 5.5 ms at 5,000, 9.3 ms at 10,000 and 17.2 ms at
  20,000, inside the 33.3 ms a tick has at 30 ticks a second. With
  Enhanced Mode off they take 2.4, 5.5 and 10.4 ms for the first three,
  and the benchmark cannot field 20,000.
- In the three 20,000-unit gate battles (10,000 units a side, one side the
  built-in computer player, seed 7, 9,000 ticks), the slowest 1% of ticks
  take 21 to 27 ms, and 1 to 9 ticks in 9,000 take longer than 33.3 ms.
- Drawn on the same machine in Full at 1920x1080, those battles keep 30
  ticks a second but draw 9 to 14 frames a second. The game ticks and
  draws on one thread (see Remaining work), so a battle this large plays
  at full speed but moves less smoothly on screen than a small one.

## Tried and left out

These were tried and left out of the revision this release ships. A match
cannot turn one on.

- Coarse routes shared by nearby destinations, built on a wide grid.
- Coarse routes over large regions, with a bend taken out of the guide.
- Nearby searches that can miss a unit which has moved inside its cell.
- A ground step checked along its whole length, so a unit cannot slip
  through a corner or another unit.
- Advisory one-way lanes through a narrow corridor.
- A patrol that keeps its goal and its search while the unit is still
  travelling.
- A faster squad list, which changes nothing a player sees.
- A briefly blocked unit queueing behind a leader on the same route instead
  of passing.
- A group order sending a unit whose formation point cannot be used to the
  clicked point.
- A separate arrival point around that click for each unit of such a group.
- A moving unit skipping a nearby route corner when the next leg is clear.
- Small ground units taking the right-hand side of a known throat.
- A computer strike taking on only a small batch of new attackers at a time.
- A queue following its leader only while the leader's current step still
  advances it.
- Computer strikes split into groups by where the attackers stand, with
  every new chase started together.
- A strike group keeping its target for the whole strike while that target
  stays close to the best.
- A long-blocked unit stepping into a clear neighbouring cell, whichever
  way the traffic is heading.
- Keeping a unit clear of other units' bodies along the whole of a step.
- Lanes chosen before a unit enters a passage.
- A shared route shifted to one side through a passage with room for two
  units abreast.
- A wide turn spread across parallel lanes.
- A route search refusing a diagonal step when a side of it is blocked.
- That diagonal charged a higher cost instead of refused.
- A waiting unit script updated without a second pass, which changes
  nothing a player sees.
- A local movement check reusing one view of the unit, which changes
  nothing a player sees.
- A refused diagonal step sliding along a clear axis.
- A patrol keeping its goal only while its route search is still running.
- A strike member keeping a chase on another enemy close to its group's
  target.
- A strike group keeping its target when the strike refreshes, not only at
  the regular dispatch.
- A long-blocked unit stepping only into a cell that is no farther from
  where its route leads.

## Remaining work

The application still ticks and draws on one thread. A slow tick can miss
a frame. The world stays on the simulation thread; drawing should be taken
from a picture published for that frame, including poses, effects,
visibility and the HUD.

The Full renderer still prepares models that are off the view. What remains
is less of that work, a moving view with bounded detail and shadows, and
separate measurements of frame preparation, upload size and GPU time.

Ray tracing, as its own Graphics option, is not in this release. It would
need its own drawing backends beside the current one, starting with sun
shadows and ambient occlusion and then reflections, and it would fall back
to Full on hardware that cannot do it. Fog stays fog. Turning Enhanced Mode
on or off does not change what a battle computes.

The working targets are 33.3 ms for a simulation tick at 30 Hz and 16.7 ms
for a rendered frame at 60 frames a second. Both have to hold in the slow
frames, not only on average. Meeting one budget alone is not the target.
