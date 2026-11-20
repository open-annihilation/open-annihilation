# Media

The media group holds what the engine plays or makes as video: the intro
player (`oa-media-intro-player`, and `oa-media-intro-player-null` where SDL3
is missing), the movie surface the game presents a playing movie on
(`oa-media-movie-surface`), and the director, which turns a director script
(`.oascript`) into the camera of every frame of a video of a recorded game.
The media libraries read simulation state and never write it.

## Intro player

Target `oa-media-intro-player`, header `oa/media/intro_player.hpp`. It plays
the game's Smacker movies (`Data/1.zrb` to `Data/5.zrb`) for the frontend and
the campaign: `IntroPlayer::open` reads a movie's header, tables and Huffman
trees through `oa-formats-smacker`, and `play` reads one frame at a time
from the file, decodes it with the engine's Smacker decoder, turns the palette
indices into RGB and shows it through SDL3, with the first audio track's
samples queued on a stream of the sound output (`oa/audio/sound_output.hpp`).
It needs SDL3 and nothing else; where SDL3 is missing,
`oa-media-intro-player-null` checks each movie and skips it with one logged
line.

- **Timing.** Frame n is shown n frame periods after the first frame, the
  period being the header's frame-rate field (a negative value counts
  1/100000 s, a positive one milliseconds), in whole nanoseconds.
- **Sound.** `play` starts the sound output for the movie and stops it
  after, so each start has its stop. When the output cannot start (a
  computer with no sound device) or will not take the movie's stream, one
  line is logged and the movie is shown without sound. Full playback then
  waits for the queued samples to play out, for at most 5 s: a sound device
  that stops taking samples ends the movie with an error instead of holding
  the game.
- **Scaling.** Frames are centred on the 640x480 canvas and the canvas is
  letterboxed to fill the window (`letterbox_dest`). Height modes 2 and 4
  double the shown height; in mode 2 the odd rows repeat the decoded row on
  screen and take palette colour 0 in a snapshot.
- **Skipping.** Escape, a quit event, closing the window, a mouse button, a
  touch, or a gamepad's A, B or Start (Menu on the Steam Deck) ends the
  movie as skipped (`skips_movie`); every gamepad is open while a movie
  plays, since SDL sends only an open gamepad's buttons. Other window
  events go to `PlaybackHooks`.
- **Memory.** One movie keeps its frame tables, its four Huffman tables,
  one frame of palette indices, one RGB frame (and a doubled one in height
  mode 2), the largest frame payload and one audio chunk's samples: at most
  about 2.5 MB for the game's movies. Nothing holds a whole movie.

`intro-player` plays the small movie of
`src/formats/smacker/tests/support/smacker_test_movie.hpp` headless (the
information it reports, three frames, six samples, a snapshot checked pixel
by pixel, a frame limit, the audio and pixel bounds, a damaged frame),
through SDL with the dummy drivers, and on sound outputs of its own: one
that cannot start, one that refuses the stream and one whose device stops
taking samples, where playback ends within its bound. Each movie file is
removed once its players are destroyed, which Windows allows only when they
have closed it. `intro-player-null` checks the player without SDL3.

## Director

Target `oa-media-director`, headers `oa/media/director.hpp`,
`oa/media/director/clock.hpp` and `oa/media/director/transition.hpp`,
namespace `oa::media::director`. It links `oa-formats-oascript` for the
script's types and nothing else: it holds no world and reads no game state,
so every function is a pure computation on its arguments.

### Entry points

- The clock (`clock.hpp`): `rational_of` turns a script's decimal into an
  exact fraction; `frame_tick` gives the tick a frame shows,
  `first_frame_of_tick` the first frame that shows a tick, `frame_samples`
  the 48 kHz audio samples a frame covers, `frames_of_seconds` a duration in
  whole frames, and `chunk_first_frame`, `chunk_of_frame` and `chunk_count`
  where the video's chunks start.
- Views (`director.hpp`): `fit_height` is the view height at which the whole
  map fits the output, `clamp_view` keeps a view on the map,
  `view_of_camera` reads a script's camera, and `engine_view` gives the
  whole-pixel engine camera, zoom, margin and sub-pixel offsets that draw a
  view.
- Shots: `compile_shots` checks a decoded script against a map's bounds and
  places its shots on the frame clock, with every error and warning named by
  its key path; `CameraRig` then steps the compiled shots one frame at a
  time and returns each frame's `FrameCameras`: the tick, the shot, its view
  and, during a transition, the held view of the shot before.
- Transitions (`transition.hpp`): `blend_transition` draws one step of a
  fade, wipe, dissolve or checkerboard between two RGB frames, and
  `checkerboard_cell` gives the checkerboard's square.

### Invariants

- The clock is integer arithmetic on exact fractions. Each result is one
  product of whole numbers divided by another, formed at 128 bits, so it is
  exact for any rates; within the script's limits (rates of at most three
  decimal places, videos of at most `max_frame_count` frames) every product
  also fits in 63 bits. Frame sample counts add up to the whole with no
  drift. Results too large for their type saturate.
- Views are doubles computed with addition, subtraction, multiplication,
  division, conversions and floor only, in the order the headers write
  them; no maths library function is used, and pi is a constant. The engine
  builds every target with contraction and value-changing optimisations off
  (`cmake/OaOptions.cmake`), so each operation is rounded to double as
  written and every platform computes the same views. A build for 32-bit
  x86 must keep doubles in SSE2 registers for the same reason.
- Linear motion is `start + (end - start) * (k / n)` on the centre's x and
  z and on 1 / height; a spring is stepped once a frame, first its speed and
  then its value, with dt the frame's duration. A shot's state at k = n is
  its boundary state: a shot without a cameraStart continues from it, speed
  included, and a transition holds it as the outgoing view. Clamping to the
  map applies to the returned views only, never to the moving state.
- `compile_shots` refuses a spring whose step a = 2 pi frequency /
  framerate, or whose damping ratio times a, is above `max_spring_step`, so
  that the stepping settles and, at a damping ratio of 1 or more, never
  swings from one frame to the next.
- Transitions are integer blends of bytes, so every platform draws the same
  frames from the same inputs.

### Tests

- `media-director-clock`: frame ticks at 30, 60, 24 and 29.97 frames a
  second and a tick rate of 45, the first frame of every tick against a
  search, sample ranges that join up and add to exactly 48000 samples a
  second, durations rounded half up, chunk boundaries in both modes against
  a search, and exact results at the largest rates, frames and ticks.
- `media-director-transition`: the first and last step of each kind, the
  fade through black, the wipe's edge, the checkerboard at a quarter, half
  and three quarters on small frames (with partial squares), and the
  refusal of bad sizes and steps with nothing written.
- `media-director`: `fit_height` and `clamp_view` on the bounds of Coast To
  Coast (3328 by 1888, fit 1872 at 16:9), Show Down, Painted Desert and
  Caldera's Rim (14816 by 14720, fit 8334); `engine_view`'s corner, size,
  margin and offsets at zooms of 0.5 to 4; every compile error and warning
  with its key path; linear endpoints exact at the start and at the
  boundary; a spring that settles without overshoot at a damping ratio of 1;
  a continuation that carries value and speed across a cut; a transition's
  held outgoing view; and a pinned SHA-256 of 600 frames of cameras and
  their engine views written as text with each double's bits, which must be
  the same on every platform.

### Limitations

- In `seconds` chunking a chunk shorter than one frame is empty: its first
  frame is the next chunk's, and `chunk_of_frame` gives the later one.
- On a map less than `min_view_height` map pixels high, or narrower than
  that view is wide, the view is centred on the map and the engine camera
  starts left of or above it (a negative `left` or `top`), which the
  renderer must draw as empty.

## Director planner

Target `oa-media-director-planner`, headers
`oa/media/director/timeline.hpp`, `oa/media/director/timeline_recorder.hpp`
and `oa/media/director/planner.hpp`, namespace `oa::media::director`. It
links `oa-media-director`, `oa-formats-oascript`, `oa-sim-match-runtime` and
`oa-core-types`. The generator (`--generate-script`) records a replayed
game's timeline with it, then plans a director script from that timeline,
and the index pass (`--replay-index`) records the same timeline and writes
the replay's index from it (`oa-app-replay-index`).

### Entry points

- `TimelineRecorder` records a timeline while a replay runs. `begin` reads
  the map's bounds, its water (the part of each square of
  `water_square_pixels` on the ground plane whose cells lie at or below the
  sea level), the players (name, side, colour, watcher, allies) and the
  tick, and records every unit already in the world as created on that
  tick, commanders first. `event_hooks` returns the match's event hooks
  (`oa/sim/match_runtime/event_hooks.hpp`), which record units created and
  finished, shots with their muzzle and aim, detonations, health events and
  deaths, each on the tick being run. `after_tick` samples every live
  mobile unit every `sample_period_ticks` ticks (place, heading, health,
  and whether the viewer's view draws it, it is cloaked, carried or
  unfinished) and every active player every `stats_period_ticks` ticks,
  both counted from the first tick. `finish` sets the last tick, the
  verdict and the usable end; `take` hands the timeline over.
  `read_player_stats` gives the players' entries for a world as it stands,
  the same ones `after_tick` records on a stats tick; the index pass
  (`--replay-index`) reads the totals on the last tick with it.
- `plan_script` plans a script from a timeline: shots from the first tick
  to the usable end (or the ending), `director.endTick` always written, the
  settings' size and rates, one-minute chunks and no interface. Its notes
  say, one line per switch in tick order, `tick N: <subject> score S`.

### What the recorder reads and writes

It reads the world it is given (`Game`, `Unit`, `UnitDef`, `WeaponDef`,
`Player`, `PlayerSetupInfo`) and calls `side_commander`; it never writes
match state, calls the match or draws from its random streams, so a replay
recorded plays out as one that is not. Positions are whole map pixels (the
16.16 position shifted right by 16 bits), build progress 65536ths, costs
and stored resources truncated; every value is an integer. A unit type or
weapon enters the header the first time an event or a sample names it, a
player the first time the world shows it active or one of its units is
created, and a player's start is where its first unit was seen. The event
hooks run inside the match's tick, so they let no exception out: an
allocation that fails stops the recording, and `finish` and `take` throw it.

### How the planner places shots

- The opening shot starts on the whole map and settles, with a 0.25 Hz
  spring, on the first combatant's commander (combatants are players with
  units, not watching and not the viewer).
- A decision every `decision_period_ticks` ticks first cuts to a big
  moment due then: a commander death, a detonation of at least
  `big_blast_area` or one that shakes the view and scores enough, a
  missile's launch and impact when one view cannot show both, or a gun
  firing and its shell landing when one view cannot show both (its first
  shot landing ashore, then one at most every 30 seconds). A moment cuts
  `moment_lead_ticks` before it, at least `min_interrupt_ticks` after the
  last cut, never when the view already holds it, and holds for a shot's
  length unless a commander dies or the fire launched lands. A launch and
  a shell's landing cut hard. A moment is framed on its place whatever the
  water, closer where the map's edge keeps it from the middle.
- Then action takes the camera when it scores enough after the memory's
  penalty (below): the hot spot where the most damage, death and blast is
  about to land (`hot_window_first_ticks` to `hot_window_last_ticks` ahead,
  weighted by the cost of what is hit, and less the more water its view
  shows past a part), cut to `arrive_early_ticks` before its first event;
  each combatant's army, a group of armed mobile units each moving at
  least `army_min_advance_pixels` toward the nearest enemy start over the
  next `army_look_ahead_ticks`, weighed by its value and framed with lead
  room the way it moves; and long-range fire landing soon, framed wide
  from its launches to its impacts for a few seconds, now and then (a hot
  spot's first view shows the launches of fire landing there as often).
  Action takes over from a quiet subject after `min_shot_ticks`; from
  other action when it scores `switch_ratio_numerator` /
  `switch_ratio_denominator` of the current one, after `max_shot_ticks`,
  or once wide long-range fire has had its few seconds; and an army
  arriving where a fight breaks out hands the camera to the fight.
- When the current shot has run its course (a quiet turn its length,
  action `max_shot_ticks`, or `min_shot_ticks` once nothing happens there
  or its damage has moved out of the view) the next quiet turn goes to a
  side: `turn_side_shots` turns a side, then the next combatant's. In a
  battle (enough damage in the last 15 seconds or the hot spots' window,
  after the first action) action and moments are held instead, for up to
  25 seconds, and a commander takes a turn only while it builds. A turn
  takes the side's best subject of another kind than its turn before: its
  commander with what it builds (at heights cycling from close to wide;
  after six minutes an idle one scores less, and one under the sea takes
  no turn), a factory about to finish a unit (the first ones score most),
  a construction unit starting buildings, or its base where it is
  building, framed around its most valued buildings on land; and,
  crossing to the next side, a wide drift from one side to the other may
  take the turn, at most every three minutes and never over the sea. A
  turn lasts 6 to 10 seconds when its subject starts nothing and 10 to 14
  when it does, the length varying from turn to turn. Quiet shots drift
  across their subject the way that shows the least water, push in or pull
  back slowly and linearly over the turn; a factory is pushed in on with
  a spring. No quiet turn starts so late that the cut to the first fight,
  or a moment, must wait or cut it short.
- The memory: every subject's score is lowered by the views left in the
  last `memory_ticks`, by as much as they overlap its view (the area they
  share over the area either covers, and at least a fixed part for the same
  subject), the more the more recently they were left; action bears half
  the penalty. Cuts are `min_cut_ticks` apart, a shot always has room for
  its shortest length before the ending, and neither the current subject
  nor the one last switched to is taken again at once. A fight whose
  neighbourhood moves by up to two cells from where it was taken stays the
  same subject, until its damage moves out of the view the shot settled
  on.
- A switch pans with a spring when the new view is near the current one
  (`pan_reach_percent` of its width, heights within a ratio of 2; a quiet
  turn only when the camera shows the new view's middle already, and then
  it follows rather than drifts). Otherwise it cuts: without a transition
  when one view shows the other's middle;
  with a wipe of `side_wipe_ms` to the other side of the map (the side
  whose start is nearest) when no wipe came in the last
  `wipe_spacing_ticks`; else with a dissolve of `far_cut_dissolve_ms`. A
  big moment comes in with a checkerboard of `moment_checkerboard_ms`, and
  the first action of the game and the ending fade through black over
  `phase_fade_ms`. Between switches, follow shots at most every two
  seconds spring toward where the subject will be, but only once a point
  of it leaves the safe area, its middle leaves the middle 40% of the view
  (the middle of a fight's damage: the safe area) or the view is more than
  twice what it needs; each moves the view at most a quarter of its width
  and height and changes the height at most a fifth. A drift runs on
  while its subject stays in the middle 80% of the view it passes, and one
  cut short stops where it has taken the camera, at its own pace. A place
  (a base, wide long-range fire, the whole map) is never followed: the
  camera drifts across it from one framing, or holds it.
- Subjects are framed in the middle `safe_area_percent` of the view (a
  lone commander, factory or construction unit in the middle 40%), and
  the view is placed, as far as that allows, to show the least water; a
  view that still shows much water comes closer while it holds its
  subject. Units the viewer's view does not draw, or that are cloaked, are
  never framed. A hot spot is framed around where its heaviest damage
  lands, as much as a close view holds, then with its attackers that fit.
  Long-range fire landing in a hot spot is framed from its launch too when
  the whole map's view can show both.
- The ending: the deciding commander death (the first after which at most
  one team has a live commander, else the last) is held from
  `ending_lead_ticks` before it, then pulled back from with a 0.2 Hz spring
  after `ending_hold_ticks`; the video ends `ending_pull_back_ticks` later.
  Nothing after it is considered, so the loser's units dying in a cascade
  make no shot.

Every number that shapes this is a named constant: those `planner.hpp`
publishes, and the rest grouped in `src/media/src/planner_tuning.hpp`, so the shots
can be tuned after watching renders.

### Invariants

- The planner uses integer arithmetic alone and breaks every tie by a
  stated order, so the same timeline gives the same script, byte for byte,
  on every platform.
- Views are kept on the map by `clamp_view`'s rules worked out exactly in
  whole numbers (centres in half pixels, heights between `min_view_height`
  and the whole map's height rounded down), so a planned script compiles
  with no clamping warning. Centres are whole pixels except where a view as
  large as the map centres on a half pixel.
- The script fits the video's frames: a shot no frame shows gives way to
  the next, a dissolve longer than its shot is left out and a spring too
  fast for the frame rate moves linearly, so the plan compiles at any size
  and rates.

### Tests

- `media-director-planner-recorder`: a small world built in the test; the
  header `begin` reads, the units already there, every event hook's fields
  (positions rounded toward minus infinity, build progress, aim, burst and
  meteor shots, attackers and death kinds), samples every 15 ticks of live
  mobile units with each flag, stats every 30 ticks, the usable end with
  and without an error, and `take` leaving the recorder empty.
- `media-director-planner`: timelines built in the test. No unit gives one
  whole-map shot; two commanders take turns of 6 to 10 seconds, of lengths
  that vary, from the opening; a fight is cut to 90 ticks before its first damage and framed in
  the safe area; big blasts interrupt 60 ticks after a cut and 60 ticks
  before they happen; the deciding commander death is held and pulled back
  from with the cascade ignored, also when the video ends first or the
  death comes in the first seconds; near subjects pan; cloaked and undrawn
  commanders and watchers' units are never framed; one player's commander
  and army take turns; a fight scoring 1.3 times the current one waits for
  the longest shot while one scoring twice takes over at once; a gun's
  shells are framed from gun to impact and a missile across a large map
  gives a launch and an impact; an advancing army is framed with lead
  room; the quiet turns' sides, kinds, lengths, factories and memory;
  armies that must advance; framing around the heaviest damage and away
  from water; a battle in bursts held without idle commanders; a gun far
  from its target shown firing and its shell landing, then again only 30
  seconds later; a commander under the sea and a fight at sea never taken,
  and a blast at the map's edge framed close. Every plan, and 300 random
  timelines at random sizes and rates, must decode back to itself in both
  forms, compile for its map with no error or warning, plan the same
  twice, never take a subject twice in a row, space its cuts, keep its
  zoom limit and move each follow shot at most a quarter of the view. A pinned SHA-256 of the YAML
  of the turns-and-fight timeline must be the same on every platform.

### Limitations

- The constants are first guesses, to be tuned on renders of recorded
  games.
- A timeline of a long game is large: every mobile unit every half second.
  Planning 45000 ticks of 1000 units takes about half a second.
- The timeline holds what the recording replays; a unit the viewer's view
  never draws is never framed, even when it fights.
