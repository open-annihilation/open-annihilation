# Replays

A `.oademo` is a replay: a zip of one recorded game. The recording inside is
a `.tad` whose packets are plain. It is stored as one zstd frame, or, when
asked, deflated without zstd. Beside it are `meta.json` and `manifest.json`.
The file holds no map picture cut from the game's data. Its identity is the
SHA-256 of the uncompressed recording.

- [What a .oademo is](#what-a-oademo-is)
- [What is inside](#what-is-inside)
- [Recording a game](#recording-a-game)
- [Playing one](#playing-one)
- [In the Library](#in-the-library)
- [The replay index](#the-replay-index)
- [oa-tool](#oa-tool)
- [Developer notes](#developer-notes)

## What a .oademo is

The game writes a `.oademo` when a recorded game ends
([Recording a game](#recording-a-game)), and a tool writes one from a `.tad`.
Two writes of the same replay are the same bytes. A member this version does
not know is kept, so a later reader still sees it.

## What is inside

The entries, in this order:

1. `oademo`, stored. The line is `oademo 1`.
2. `meta.json`, deflated. What the Library shows.
3. `manifest.json`, deflated. What has to be installed to watch it.
4. `timeline.json`, deflated, when the replay has one.
5. `recording.tad.zst`, stored, one zstd frame. Or `recording.tad`, deflated,
   when the recording is stored without zstd. A file holds one of the two.

`recorded.how` says how the game was recorded. The values include
multiplayer, skirmish, imported, core-prime and campaign. `game.kind` is
free text: skirmish, campaign, multiplayer, or a kind an extension names.
An object under `game` whose name is that kind is kept, and so is every
member this version does not know.

`recorded.after_crash` is true when the recording was finished after a crash.
`recording.addresses_removed` is true on a file this game writes: the
recording holds no network address, and the writer refuses a replay whose
recording holds one.

`manifest.json` lists what has to be installed to watch the replay. A package
that holds maps, such as a map pack, also lists each map it has to hold, by
name and SHA-256.

The recording is compressed at level 3 when a game ends, and at level 19
when a replay is exported. `--deflate` stores it with deflate at level 6
instead of zstd. Whatever the level, a zstd frame declares a window of at most
8 MiB, so a reader that refuses a larger window can read every replay.

## Recording a game

Every game you play is kept as a replay, a network game, a skirmish and a
campaign mission, without anyone else in a network game noticing and without
your address in the file.

Whether the game records, and for how long it keeps what it recorded, are
two settings, **Record my games** and **Keep replays for**, in
[Settings › Replays](settings.md#replays).

**What is recorded, and when.** A game is recorded from the moment its match
starts, when any of these holds:

- the setting `open-annihilation.replays.record` is on, which it is unless the
  preferences file says `off`, and a person plays the run;
- the mod's profile turns on the recorder hack
  ([recorder.ta-demo-recorder](mods/standard-hacks/recorder.ta-demo-recorder.md));
- `--net-record FILE` names a file;
- `--record-game FILE` names a file.

A headless run or a check is not a player's game: it is recorded only when
`--net-record` or `--record-game` names a file, whatever the setting says, and
a headless run that names neither writes no `Recordings` folder at all. The
setting is the player's own: recording because of it never presents a
recorder to the other players, so `recorder_protocol` in every lobby setup
block, and every other byte of the lobby and the game, stay as the mod's
profile makes them. The other players see the same game, and a game recorded
and one not recorded play the same. A skirmish is recorded for the same
reasons, including `--record-game` and `--net-record` in a headless run. Your
seat and every computer player are in the file, each with its status block,
and the unit checks are this machine's, taken from the unit table the match
played. Each player's kills, metal and energy are recorded on the same beat a
network game sends them, and once more as the recording finishes, so a replay
shows those totals. Starting the skirmish again finishes this recording and
begins another. A skirmish opened from a saved game is recorded too: the file
begins with a create record for every unit already on the map, and that
recording plays back.

A campaign mission is recorded the same way. Both of its players are in the
file, the local player and the computer. The units the mission places before
the first tick are create records ahead of the first unit states, and a unit
a trigger places later is recorded as it is created. The map's name is the
mission map's own name, which is how playback finds the file; playback places
no commanders and mirrors both players. Playback keeps the unit definitions
the mission kept, so a unit the recording creates is that unit. `recorded.how` and `game.kind` are
`campaign`, and `game.campaign` holds the campaign's name, the mission's
number and its name. The manifest names the expansion the campaign's file
comes from. Starting the mission again finishes this recording and begins
another. A mission opened from a saved game is recorded as a skirmish opened
from a saved game is: a create record for every unit already on the map, and
the replay starts from the map's own features.

A fresh skirmish records every feature the game destroys, reclaims or raises
again, and the replay's features match the game's. A replay of a skirmish
opened from a saved game starts from the map's own features, then applies the
changes recorded from the first tick of that recording. A tree, a rock or a
wreck the save had already removed or added is not in the recording, so the
replay shows the map's feature there until a recorded change moves it.

**What is written.** One file, in one of two layouts, never both:

- a `.oademo`, zstd level 3, when the setting records the game or
  `--record-game` names a file whose name does not end in `.tad`;
- the recorder hack's `.tad`, exactly as before except that it holds no
  sector 6, when the profile's recorder hack is on, when `--net-record` names
  the file, or when `--record-game` names a `.tad`. The hack's layout keeps
  every packet as the hack's `compress-files` says; a `.oademo` holds every
  packet plain.

**Where it goes.** A file is named after the local date, time and map, as in
`2026-11-04 1432 Show Down.oademo`, with ` (2)` and so on added to a name
already taken, and goes into `Recordings/<mod id>` in the Open Annihilation
folder (`Recordings/default` without a mod). The content note
`<file>.oa-content` is written beside it, as it is beside a recorder's
file ([Saves, recordings and films](saves.md)). A file `--net-record` or
`--record-game` names is written where it is named, replacing what is there.
The folder may be on another drive or a network share, including one that
refuses to set permissions: the file is copied there as any new file is made
there, and a recording that cannot be written leaves no file at the name.

**While the game goes.** The recording is streamed to
`Recordings/.replays/partial/<mod id>` from its start: the header, the
players, their statuses and the unit checks are known when the match starts,
and each packet is added as it is made. A thread of the recording's own
writes them, so a frame never waits for the disk. The stream's name says its
layout: a replay's ends `.oademo.part` and the recorder hack's `.tad.part`,
whatever the file will be called. A replay's description (`.start`) is written
before its stream is made, and a game whose description cannot be kept is not
recorded (the log says so). When the game ends or you
leave it, the recording is finished and moved into place: a `.oademo` is
compressed off the thread that draws. The log says
`multiplayer: recorded the game to FILE`,
`skirmish: recorded the game to FILE` for a skirmish, or
`campaign: recorded the game to FILE` for a campaign mission.

**After a crash.** A recording the game left in `partial` because it stopped
is finished at the start of the next run, whatever that run is: cut to its
last whole packet and moved into `Recordings/<mod id>`, with the content
note beside it. A replay finished this way has `recorded.after_crash` true
and `game.ended` `cut`; the recorder hack's `.tad` is cut and moved. A
recording of a game still being played, in this program or another, is left
alone, one that holds no packet is dropped, and one that cannot be finished
is kept under its own name ending `.failed`: a replay's stream whose
description is lost or cannot be read is never put in `Recordings`, as a
recorder's file or as anything else. When the disk refuses a read or a write
the recording stays where it is for a later start. A file made this way never
replaces another: it takes ` (2)` and so on.

**What `meta.json` holds.** What the game knows when the match starts and
when it ends:

- `recorded.at`, the clock's time in UTC; `recorded.local`, the local date as
  the recorder's sector writes it; `recorded.by` and `recorded.platform`,
  the program and the machine; `recorded.how` `multiplayer` for a network
  game, `skirmish` for a skirmish and `campaign` for a campaign mission;
- `game.kind` `multiplayer`, `skirmish` or `campaign`, `game.length_ticks` and
  `game.length_ms`, and `game.ended`: `finished` when the match ended, `left`
  when the player left it, `cut` after a crash, or when a packet did not fit
  the file and is missing. A campaign mission's `game.campaign` holds the
  campaign's name, the mission's number and its name. A game an extension
  launched names its own kind and carries an object under `game` named for
  it: an online battle's game id, its planet and its sector;
- the map's name; each player's name, side, colour, team and kind (`human`,
  `computer` or `watcher`), with `recorded_here` for the player on the machine
  that recorded, and, for a campaign mission that ended in its victory or
  defeat, that player's `result`, `won` or `lost`;
- the base game, the expansions that change the rules, the mod (id, name,
  version, revision and sim hash), the mutators and the brains the game played
  with, and the two limits;
- the unit table's digest.

`manifest.json` names the base game, each expansion, the mod, the map pack a
pack map came from, and each mutator. Each package is named by the SHA-256
the installed package's record keeps for it, or, when the game keeps none, by
the digest of the package's folder (for a mod that no folder holds, its
profile's own hash), so that every `oamod`, `oamap` and `oamut` need carries
one. A replay of an older map pack keeps that pack in `Replay releases`
and plays it from there; the pack in `Maps` stays. A map pack that is not
installed is left out. `engine.needs` is the first release that reads a
`.oademo`, and `engine.recorded_with` the version that recorded it.

**No replay holds an address.** The players' network addresses are never
written: the `.tad` the game writes holds no sector 6, `meta.json`,
`manifest.json` and the content note hold none, and `recording.addresses_removed`
is true. A recording of another recorder that holds such a sector still plays;
only writing stops.

## Opening one

A `.oademo` or a `.tad` opened from outside the game is copied into
`Recordings/Imported` and selected in the Library's Replays section
([the Library](library.md#replays)). The same recording already under
`Recordings` is selected and not copied again. A `.tad` is written with its
sector 6 left out and nothing else changed. A `.oademo` whose recording holds
a sector 6 is rewritten without it. The replay's identity is the SHA-256 of
the stream as kept.

`.oademo` is the game's own type. `.tad` is offered beside it and is never
the default. **Open replay files with Open Annihilation**, in
[Settings › Replays](settings.md#replays), registers or withdraws both on
Windows and Linux, and on macOS chooses whether this copy is the default
opener of `.oademo`.

## Playing one

`--play-demo` takes a `.tad` or a `.oademo`. See
[Demo Recorder](../README.md#demo-recorder) and the
[command line](command-line.md). A path named on its own is imported, as
[Opening one](#opening-one) says, and is not played at once. **+** and **-**
step the speed it is watched at, and **Pause** holds it.
[Playback](#playback) says how a jump to another moment works.

While a replay plays, a bar along the foot of the battlefield shows play
and pause, the time, a timeline you can press or drag, and the speed. It
hides after three seconds away from the battlefield's lower edge and comes
back on a playback key. **B** pins it for this watch; the pin is not saved.
**View** chooses whose sight you see: every player, the whole map, or one
player's view. Its menu lists each player beside their colour. A player
who was defeated or who left stays in it for the whole replay, and the row
says **Defeated 18:02** or **Left 21:40** once that moment has passed.
Choosing that row shows the sight the match still holds for the seat. When
the seat itself is gone, the row stays, the mark stays on it, and the view
is the whole map: the match no longer holds that player's sight. The seat
the replay is watched from is never listed. The top bar shows only while
one player's view is chosen, with that player's stored metal and energy
and the rates the recording gives; under every player or the whole map
there is no bar, and the battlefield takes its rows, the map staying where
it was on the screen, or flush with its top edge when the view was there.
**Camera** is free, **Director** when the index has recorded a fight, that
player's own camera when the recording already carries camera records, or
one unit. **Director** follows one fight at a time, a little
ahead of the moment on show. It stays with that fight while the fight is
just ahead or no more than ten seconds past, and changes then only for a
heavier fight. Either way it changes only once four seconds of the battle
have passed and about a second and a half of real time, so a fast playback
does not flicker between fights. A
nearby fight is panned to. A fight farther than the picture comfortably
shows is a cut. It does not sweep across the map. The filmed script
(`--generate-script`, in the developer notes) is a different tool: playing
it between the fights cut back and forth and swept the map, so the replay
does not use it to place the camera. With nothing ahead, Director stays
with the latest fight just passed. Through a quiet minute it stays with the
nearest fight of that minute, the one ahead when the two are equally far,
rather than a start base. Before the first fight it shows a start base: the
first commander's. It is not offered for a replay whose index has no fight. **Follow their camera** while every player is shown follows
the first player who shared a camera. Double-click a unit to keep it in the middle until you pan; when
that unit is gone the camera returns to free. The players' stats stand at
the battlefield's top right, over ground darkened as under the kills
board, so they read on any map and the battlefield shows through: for each
player
the **View** menu lists, in its order, their colour, name, stored metal and
energy with a bar for each, and both incomes, worked out from the
recording. A player whose economy the recording never gave, as a
computer's often is not, and a player who has left, show their colour and
name alone. On a narrow window and a phone the stats take a smaller step,
then fewer rows, rather than reach the bar or the touch controls. A
double-click on a row shows that player's view, and **F4** hides or shows
them. **Tab** and **Shift+Tab** move through the views, **Q** returns to
every player, and **C** changes the camera.
At the end, a card names the winner, the length and each player's kills and
losses, and offers **Watch again**, **Back to the Library** and **Export…**.
A damaged recording says so, with the time it failed.

`--replay-viewer` with `--join` watches a recording another machine's replayer
is playing to the session. When that game ends, the watcher prints the same
`demo summary` line `--play-demo` prints: records applied and refused, units
created, refused and past the table, record errors, tick errors, dropped
frames, oversized frames, bad packets and untimed unit states, and the last
error when there is one. The line is printed when the host ends the replay,
when the host leaves, or when the match itself ends, and only once a record
of the replay was applied. A host that sends no packet for 60 seconds drops
the session, and the watcher prints no summary. A join that is still in the
battle room, with the replay not started, prints no summary. A windowed one
keeps waiting there. A headless one ends after five minutes.

`--headless-check --join ADDRESS --replay-viewer` plays that session through
without a window. It sets READY itself once it is seated in the battle room
and has reported its own unit types, and sets it again if the room clears the
bit. The line `replay viewer: READY after the unit sync` is printed once, the
first time. It exits with status 0 when the watched game ends. It exits with a
non-zero status, and prints no summary, when the join fails or the session
drops before one of those ends; the line it prints then says which. A host
silent for 60 seconds is one of those drops. A headless join still in the
battle room five minutes after it arrived, with the match not started, prints
`replay viewer: the replay did not start within 5 minutes of the battle room`
and no summary. `--play-demo` still owns the summary when both are set.

A joined watcher has not read the recording, so oversized frames, bad packets,
untimed unit states and tick errors are 0 in its line. Records applied can
differ from `--play-demo`'s for the same file: TA Demo Recorder sends records
of its own (it removes the units of a player whose recording has ended, and
the units left at the end), and its end can arrive before the last records
have been applied. Every other field of the line is compared exactly.
Records applied may differ by up to 1%.

## In the Library

The Library's **Replays** section, second in its section list, lists every
replay in `Recordings`: this month's, older ones, the ones you keep and the
ones you imported, by kind, searched by map, player, mod or planet, and
sorted. **Watch** plays one in the running game, and leaving its match comes
back to the same row ([the Library](library.md#replays)). Leaving a Watch
remembers where it was left, and the card offers **Watch from** that time.
While the Library is open and no game is running, each replay that has not
been read yet is read in the background, one at a time. The row says
**Reading…** until that finishes, then **Won** or **Lost** for the player
whose computer recorded it, when that is known. The card shows each player's
result, how many units they destroyed and lost, and a strip of the game's
moments. Closing the Library, starting a game or pressing **Watch** stops
the read. A read that was stopped writes nothing, so the row says
**Reading…** again next time. A replay the read could not play is not read
again until what is installed changes or another build of the game runs:
each new build reads it once more, in the same background read, one replay
at a time. A phone, which cannot start that read, shows
what the file's header says and no **Reading…** chip. Exporting a replay
carries what that read wrote. A timeline the file already holds is kept.

**On Core Prime** the same section lists the replays Core Prime publishes.
**Download** writes the `.oademo` into `Recordings/Core Prime`, and **Watch**
plays that file. The game does not download the `.tad`.
[The Library](library.md#replays) says how the switch, the filters and the
empty list behave. **Sign in** and **Sign out** sit beside that switch.
A signed-in player can share a replay from its card. The file that is sent
keeps the chat and holds no network address. After a ranked battle the game
says that battle goes on Core Prime by itself, a few minutes after it ends.
After a recorded For Fun battle it offers **Share…** and **Not now**.
**Share…** leaves the battle screens and opens the Library on that recording
with the share question, after Sign in when the player is signed out. A For
Fun battle that was not recorded says so and does not ask to share. **Open
on Core Prime** is on a listed battle's card whether or not the player is
signed in. **Share my own recording** waits until they are. The card does
the upload.

**Delete replay** on the card, confirmed by the button beside it, moves that
replay's file to the system's trash and takes it out of the list. If the
trash refuses, the file stays and the card says so. At start,
replays older than **Keep replays for** go the same way, except one you
keep (even when its bytes have changed) and one you imported, downloaded or
exported. Exporting a replay keeps it. **Forever** reads nothing; any
other span is read off the main thread, and the files that then go are
moved off it too. Quitting stops the read. The first time any would
go, one question names how many and offers to keep them all, which sets the
keep setting to forever and removes nothing, or to let them go. Nothing goes
before that question is answered. Escape does nothing: the question stays up.

The Library keeps what it read of each file in `Recordings/.replays/index.json`:
one entry for each `.oademo` and `.tad` below `Recordings`, at most three
folders down (names that start with a dot, `.replays` among them, are left
out), with the file's path, size and time, the recording's SHA-256, the
map, the kind, the players, the mod, the expansions and mutators, the planet
and sector a kind's object names, the length, how it ended and whether it was
finished after a crash. A `.oademo` is read for its
`meta.json` and `manifest.json` alone, not its recording. What it needs is
taken from `manifest.json`: the packages and the engine it names. The
`.oa-content` note beside a `.oademo` is left unread for that. A `.tad` is
read for its header, players, recorder and length, and what it needs is
taken from that header, its mutators sector and the `.oa-content` note
beside the file. The note's mod line is the name the player saw or the
mod's id, with no release, and any installed release of that mod plays the
file. A `.tad` with no note, or one whose note names no mod, plays with
the installed mod whose id names its folder below `Recordings`; the
`default`, `Imported` and `Core Prime` folders name none. A file that does
not read is listed with the reason and marked damaged. A file whose path, size and time are
those of its entry is not opened again. The index is a cache: deleting it
only makes the Library read every file again, and forgets nothing; a damaged
one is moved to `index.json.damaged` (and `index.json.damaged-2` and on
when that name is taken), not overwritten. The replays you keep
are not in it. They are marked in `Recordings/.replays/kept.json`, each by its
recording's SHA-256 with its path as a hint, written whole the moment you
press **Keep**: a replay you move or rename stays kept, and a file a mark
names by path stays kept even when another copy still has that recording.
The changed file gains a mark of its own. A deleted or
damaged index loses no mark. Two copies that still share one recording share
one mark; deleting one leaves it for the other. Exporting a replay marks
it the same way in `exported.json`, once the exported file has been written,
and **Keep replays for** leaves that recording until you delete it. An
`exported.json` that cannot be read is set aside and is not replaced, so
the export is not marked. While a set-aside `exported.json.damaged` is
there, an export is not marked and old replays stay, until that file can
be read. A `kept.json` that cannot be read is
set aside, not overwritten, and every whole mark still in its text is written
to a new file. That consideration removes nothing. While a set-aside
`kept.json.damaged` file is there, a replay whose SHA-256 or path appears in
its bytes stays, and so does every replay recorded before that file was set
aside, however old: a mark cut off with the damage cannot be known, so those
replays are no longer removed by age, and nothing asks again. Replays
recorded later go by age as usual. Once no `kept.json.damaged` file is
there, the older ones go by age again. A game's kind that an extension records, such
as the Boneyards' battles, is offered in the section's kind menu by the
title the extension registers, and its rows show the planet of the battle.

## The replay index

A replay's length, who won and when things happened are known before it is
watched. `--replay-index` plays a `.tad` or a `.oademo` to its end, with no
window and no sound, and writes that as a small file, `timeline.json`:

```sh
open-annihilation --replay-index games/game.tad --output games/game.timeline.json
```

A `.oademo` holds the same file as its `timeline.json` entry. The Library
stores that text on the replay's index entry once its background read
finishes, and an export copies it into a replay whose own timeline is
missing or empty. The pass runs
on the fixed clock and the fixed seed, and changes nothing the replay
computes: it ends on the same tick as `--play-demo`. The same recording
always gives the same file, byte for byte. `--output` is required. The
pass mounts one kept map pack with `--replay-map-pack`.

The pass exits 0 when it wrote the file, also for a recording that did not
replay cleanly or that stops early: the file says so. It exits 1 and writes
nothing when the recording cannot be replayed at all: it cannot be read, its
map or its unit definitions are not installed, or no extension of the build
replays it.

While a replay is watched, a timeline the Library already holds for that file
is what the bar shows, and no pass is started when that timeline names its
battle events. A timeline with no `events` member is read again. Otherwise
the bar starts this same pass beside the game, at low priority, and the
timeline it writes is stored in the Library's index, so the next Watch and
the card need no pass. That pass plays with what Watch plays with: the
mod folder the game layers (`--mod-dir`) and the map pack kept for the
replay (`--replay-map-pack`). The Library's own pass plays a replay with the
mod Watch would choose for it: the release it names, the mod a `.tad`'s note
or folder names, or the mod being played when that is the one it names.
The pass keeps a preferences file of its own and does not change the player's.
It prints `replay index: progress N` on standard output each thousand ticks,
`N` the percent of the recording's length it has read, and the bar shows that
percent beside "No markers yet". The bar stops the process when the replay is
left. The Library's own pass does not run during a match.

### What the file holds

One compact JSON object, its members in this order:

| Member | What it holds |
|---|---|
| `oademo_timeline` | `1`, the version of this file. Version 2 and later are refused. A version 1 file with no `events` member still reads |
| `length` | `ticks`, the last tick, which is the length of the replay (a second of game time is 30 ticks); `ms`, the recording's length on the recorder's clock, when known |
| `ended` | `how`, `finished`, `left` or `cut`, and `tick`, the tick it ended on |
| `verdict` | how the replay went: `finished` (everything recorded was replayed), `clean`, `paced`, `content_differs`, `errors`, `first_error_tick` (0 for none) and `last_error` (text, empty for none) |
| `result` | `winners`, the numbers of the players who won (empty when none is known), and `by`: `commanders`, `mission`, or `none`. A campaign mission that finished uses `mission`: the recording player is the only winner when that mission was won, and is left out of `winners` when it was lost. A mission that was left, and every skirmish and network game, keep `commanders` or `none` |
| `players` | each player in the recording's order: `number`, `name`, `watcher`, and `final`, the totals on the last tick |
| `markers` | the markers a timeline shows, in tick order (below) |
| `markers_omitted` | how many markers were left out; only when it is not 0 |
| `totals` | `from_tick`, `period_ticks` and, for each player who plays, one array of samples for `kills`, `losses`, `commanders_lost`, `units`, `metal` and `energy` |
| `intensity` | `from_tick`, `period_ticks` and `damage`: the health points taken off units in each stretch |
| `events` | where the fighting was, in tick order, as compact arrays (below). Always written. Empty when the game had no fight. At most 512 |

A number of a player is the player's number in the recording, the one the
description's `players` list gives. A name is UTF-8. `totals` holds one
sample every 300 ticks, ten seconds of game time, from tick 0, so the
sample `i` is the totals on tick `from_tick + i * period_ticks`, and
`damage[i]` is the damage in the 300 ticks from that tick. A player whose
slot was emptied by leaving keeps what was scored, with no units and
nothing stored. Watchers have no samples.

A marker has a `tick` and a `kind`, and these members:

| Kind | Members | Meaning |
|---|---|---|
| `start`, `end` | | the first and the last tick |
| `commander_death` | `player`, `by` | the player's commander died; `by` is the player whose unit last hurt it, left out when no player's did |
| `player_left` | `player` | the player left the game |
| `player_dropped` | `player` | the player's connection was lost |
| `player_defeated` | `player` | the player was removed as defeated |
| `alliance` | `player`, `other`, `allied` | the two players became allies of each other, or no longer are |
| `pause` | `player`, `paused` | the game was paused, or carried on |
| `chat` | `player`, `text` | the player said `text`, as the line was sent, with the name before it |

A commander displaced from its slot and replaced by another of its player's
on the same tick is no loss. A chat marker is a line a player typed, which
starts with the player's name between `<` and `>` and a space: a line that
does not (the recorders' notices, such as `***Name tried to unpause.`, and
their replies to commands, such as `Name 12`), a line that is a command
(`+` or `.` first, after the name) and a private line are left out. Pauses
called in the first three seconds are left out: the machines hold the start
of a game with pauses of their own. A player who left, was dropped or was
defeated is marked once. An alliance record sets one player's side of a
pair, as playback applies it, whatever else the record asks of the other
machine; a pair is allied once both players have set theirs, and the marker
says so. A file keeps at most 1,000 markers of each kind and counts the rest
in `markers_omitted`, so that it stays small whatever was said.

An event of the battle is a compact array: the `tick`, the `kind`, an `x`
across the map and a `z` on the row the picture draws, then `player`,
`metal`, `health` and `amount` through the last of those that is not 0.
`[1200,"unit_lost",40,80,1,90,600]` is a unit lost at tick 1,200. The member
is always written, and it is an empty list when the game had no fight. The
version stays 1, so a reader that knows only the moments, the intensity and
the winners ignores `events`.

| Kind | Meaning |
|---|---|
| `unit_lost` | a unit was destroyed. `player` owned it. `metal` is what it cost and `health` is its full health, when the type is known |
| `building_lost` | a building was destroyed, or a loss whose type is not known. The same members as `unit_lost` |
| `engagement` | a large cluster of hits. `amount` is the health points taken. The place is the centre of the hits, weighted by how hard each one was |
| `commander` | a commander was created. This alone is not a fight |
| `commander_lost` | a commander's real death. A commander displaced from its slot and replaced by another of its player's on the same tick is left out, and this alone is not a fight |

Hits are gathered into squares of 512 map pixels over each stretch of 300
ticks. A square becomes one engagement when the hits take 2,000 health or
there are eight of them. Healing and paralysis are not hits. A capture, a
reclaim, a dismissal and a cancelled build are not losses. Of the fights in
one of those squares and stretches, the heaviest is kept. A file then keeps
at most 512 events. The commanders are kept, and the fights that remain are
spread across the whole game rather than taken from its opening. When the
commanders alone fill the file, the fights are left out. A commander
displaced from its slot leaves out that creation as well as the loss. The
commander that replaces it is kept.

**Director** is offered once the index holds a `unit_lost`, a `building_lost`
or an `engagement`. It follows one fight. The fight it would choose at a
moment is the heaviest in the next eight seconds (240 ticks). When that
stretch is quiet, it is the most recent fight in the ten seconds just passed
(300 ticks), not the heaviest and not the earliest. When both are quiet, it
is the nearest fight inside one minute (1,800 ticks), the one ahead of the
moment when a fight ahead and a fight behind are equally far. The camera
keeps the fight it is already on while that fight is inside the next eight
seconds or the ten seconds just passed, and leaves it then only for a fight
at least one and a half times as heavy. An older fight gives way to the
fight the camera would choose, however heavy it was, so a big engagement
does not hold the picture for a minute while the battle moves on. Either
way the picture changes only once four seconds of the battle (120 ticks)
and about a second and a half of real time have passed since the last
change. A fast
playback therefore skips fights rather than cutting on every one. A fight
within 64 map pixels is the same subject: the picture eases across and the
hold is not started again. A move within 75 percent of the picture's width,
which is also within about two screens, is an eased pan that stays under one
screen a second at 1×.
Anything farther is a cut. A place just left is not shown again within ten
seconds. An engagement's weight is its `amount`; a loss's weight is its
`metal`, then its `health`. Before the first fight, the picture shows the
start of the first commander the index names, the lowest player number
first. Each step reads only the events within a minute of the moment on
show, so it costs the same however many events the index keeps. A seek, or
choosing Director, plays these rules from the start of the replay at 1×,
so the picture after a jump is the one a steady 1× watch shows there.

The Library stores one timeline, the JSON text, on each replay in
`Recordings/.replays/index.json`. The file is at most 64 MiB. It keeps a
timeline it stored, including the watched place. A version 1 timeline with
no `events` member is read again, and the old text stays until the new one
is stored, so a replay the pass can no longer read keeps its result and its
marks. While it waits, its row says Reading… and the Library counts it as
not yet read; a pass that refuses it ends that, as for a row with no
timeline. The version is read from the start of the text, `{"oademo_timeline":1,`,
without parsing the whole timeline. An empty timeline is a pass that
finished with nothing to store, and is left as it is. A version 1 timeline
that names `events`, including an empty list, is kept. Version 2 and later
are refused.

### How it ended and who won

The commanders decide who won, as a director script's ending does: the
first commander lost after which at most one team has a commander. Teams
are the players who are allied with each other, both ways, at that tick,
and every death of the tick counts first. The winners are the players of
the team that is left, whichever of them still has a commander. A game in
which every team is out on one tick has no winner. A game no commander lost
decides has `winners` empty and `by` `none`.

A campaign mission whose `meta.json` gives the recording player's `result`,
and which played to the end of what was recorded, takes its result from the
mission instead, with `by` `mission`: that player alone when it was won, and
every other player who plays (or `0` when nobody else does) when it was lost.

`ended.how` is, in this order:

1. `cut` when the replay could not go on to the end of what was recorded: the
   tick is where it stopped;
2. `finished` when the commanders decided the game;
3. `left` when a player left, was dropped or was defeated and the commanders
   decided nothing;
4. `finished` when the machines exchanged the scores of the end screen (an
   economy record that asks for a reply, which the machines send only when a
   game ends);
5. `finished` or `left`, as `meta.json`'s `game.ended` says, when that word
   is one of those two. This is a `.oademo` that played to the end of what
   was recorded and that the commanders, a departure and the end screen did
   not decide. The tick is the last one. A `.tad` has no `meta.json`, so
   this step does not apply to it;
6. `cut` otherwise: the recording stops with the game undecided, as one that
   was cut short does.

The six recorded 3.1c games all end `finished`, every one decided by the
commanders; a copy of one cut at half its packets ends `cut`.

## oa-tool

`oa-tool replay` reads a replay, wraps a `.tad`, and writes the recording
back. The commands are in [oa-tool](../tools/oa-tool/README.md). Pack leaves
out any network address the `.tad` held. It describes a recording as playback
reads it: each player's side and colour come from the player's lobby block in
the recording, the recorder's text is turned from the game's code page into
UTF-8, and the mutators and brains a game used come from its sectors.
The recorder's date is kept as text in `recorded.local`; `recorded.at` is left
out because a recorder's date has no time zone.

`replay export` writes a `.oademo` or a `.tad`. A `.oademo` is one zstd frame
at level 19. A recording that holds a network address, or a packet that is
not plain, is cleaned first; one that is already clean is stored unchanged,
so its identity stays. A timeline already in a `.oademo` is kept. A `.tad`
is version 5, every packet plain, with no network address and no address
sector. Map and player names longer than 63 bytes are cut, and each status
datagram longer than 512 bytes is cut. The command says so and still writes
the file. `--no-oa-sectors` leaves out the mutators, brains and limits
sectors and applies only to a `.tad`. Two exports of the same replay are
the same bytes.

## Developer notes

### What the index pass costs

The pass replays a recording as `--play-demo` does and records its timeline
as it goes. The table gives what a Mac Studio (a Check build) took to play
each of three of the recorded 3.1c games the tests keep with
`--headless-check --play-demo FILE --match-ticks N`, and to index it,
alternating the two. The machine ran other builds and tests all the while
(a load average of 40 to 70 on 24 cores), so each figure is good to about
15 %. CPU time is the user time `time -l` reports, in seconds, one figure
for each run; memory is the peak resident size in megabytes.

The pass records the fight as well as the players' standings
(`Detail::battles`: every hit, the commanders created and killed, every unit
and building destroyed, and each player's totals every second). It still
takes no sample of where a unit stood. The table is what the standings-only
pass cost, measured before the fight was added, and what the whole recorder
costs, as `--generate-script` uses it.

| Recording | Ticks | Plain `--play-demo` | The pass, standings | The pass, whole recorder |
|---|---:|---|---|---|
| 720 Show Down | 42,965 | 2.30 to 2.39 s, 254 to 258 MB | 2.27 to 2.32 s, 243 to 257 MB | 2.63 to 2.71 s, 252 to 255 MB |
| 884 Caldera's Rim | 81,556 | 15.6 to 18.1 s, 360 to 364 MB | 16.5 to 18.2 s, 372 to 377 MB | 16.1 to 17.7 s, 419 to 423 MB |
| 5573 Painted Desert | 469,856 | 106.3 to 113.4 s, 639 to 647 MB | 105.7 to 119.0 s, 730 to 740 MB | 116.2 to 119.1 s, 1,317 to 1,383 MB |

(The plain runs beside the whole recorder, taken later under another load,
took 2.64 to 2.76 s, 18.9 to 20.8 s and 113.1 to 114.4 s.)

- **Time.** The pass takes the time of a plain run, to the noise of the
  machine: no run of the pass took more than 5 % longer than the slowest
  plain run beside it, and several took less. Its own work, the timeline
  recorder and the walk over the recording's records, is a few percent of
  the replay's, which is nearly all the simulation. A four-and-a-half-hour
  game takes about two minutes, a three-quarter-hour one about 17 seconds
  and a 24-minute one about 2.5 seconds.
- **Memory.** The whole recorder doubles the peak of a long game: 5573 holds
  13.7 million samples of its mobile units (every unit twice a second,
  28 bytes each) and 771,556 events, and peaks at 1.3 to 1.4 GB against
  0.64 GB for the plain run. That is too much for a pass the Library runs in
  the background on a player's computer. That measurement kept the recorder
  to the standings (191,732 events on 5573, and no samples): 90 MB above
  the plain run on 5573, and 12 to 17 MB on 884. The pass now records the
  fight as well and still takes no sample of a unit.
- The index of a recording was a file of 7 to 100 kilobytes when it held no
  battle events (720: 7.6 KB; 5573: 98 KB, 184 chat lines). It stays version
  1 and adds `events`: the heaviest fight in each square of 512 map pixels
  and each stretch of 300 ticks, then at most 512 events, written as
  compact arrays. 64 events left a four-hour game one fight every four
  minutes, and the camera on a stale place for minutes at a time; 512 add at
  most about 21 KB to an index. Recording the fight costs about 8 to 10 percent more processor
  time than the standings-only pass, and the same peak memory. On this
  Studio, 884 took 15.43 s of CPU against 14.00 s, and peaked at 376 MB
  against 377 MB. 5573 took 109.0 s against 100.5 s, and peaked at 741 MB
  against 746 MB. 5574 took 86.1 s against 78.7 s. The sizes after this
  change are in the table of the six games below. Markers, totals, the
  verdict and the winners of those six stay as that table says. The Library
  stores that timeline text once per replay.

The playback bar's pass is that same program, started beside the game at low
priority. Its time and its memory are the pass's own, and they are not part
of the game's. A timeline the Library holds is used instead, and what a pass
writes is stored in the Library's index.

### The six recorded 3.1c games

The index of each of the six recorded 3.1c games the tests keep. Two indexes
of one recording are the same bytes. The bytes changed from the pins that
had no battle events because version 1 now holds `events`, including when a
game's events are only its commanders:

| Id | Length (ticks) | Ended | Winners | Verdict | Index |
|---|---:|---|---|---|---:|
| 720 | 42,965 | `finished` | player 1, NJ_THUG | clean | 16,618 bytes |
| 721 | 41,571 | `finished` | player 1, freakmaster | clean | 9,992 bytes |
| 884 | 81,556 | `finished` | player 4, GG_Med | clean | 35,608 bytes |
| 5573 | 469,856 | `finished` | player 2, RawDawgFU | clean | 119,256 bytes |
| 5574 | 250,601 | `finished` | player 1, Gnug315 | clean | 75,342 bytes |
| 5575 | 87,567 | `finished` | player 1, TAG_Sensation | not clean: one error, first on tick 82,901 | 39,185 bytes |

Each length is the end tick the table of those games (`demos.tsv`) pins for
the game. Each holds a second commander death a few seconds after the one
that decided it; the index marks both, and the first decides. None of the
six has a departure or an alliance marker. Their chat markers are 8 (720),
6 (721), 11 (884), 184 (5573), 91 (5574) and 46 (5575). 884 holds pauses
called in its first three seconds, which the index leaves out, and 5574 and
5575 each hold a few short pauses a player called, which it marks.

### Keyframes of a playback session

A keyframe is the state of a replay at one tick, kept in memory, from which
the replay can play on as it did from there the first time. Watching needs
them to jump back: nothing in a replay runs backwards, so rewinding restores
the nearest keyframe before the target and plays the rest of the way. A
keyframe is never written to disk, and a replay that takes none computes what
it always computed. The viewer takes them while a replay plays, and seeks with
them, as [Playback](#playback) describes.

`oa::session::demo` makes one with `demo_keyframe_take` and puts the session
back with `demo_keyframe_restore`. A keyframe is sections of bytes: the
world's tables (`oa/core/world_image.hpp`), the match's own state
(`Match::write_keyframe`), the network match, the packet layer and the
binding, the playback's cursor, and what the program that runs the playback
keeps (`DemoSession::keyframe_extension`). The sections that never change in
size are the keyframe's fixed ones: the first keyframe a session takes is the
base, and every other keyframe stores its fixed sections as their difference
from it, so the terrain and the tables a battle leaves alone cost nothing. The
whole is compressed with zstd at level 1.

Each keyframe also keeps the world digest `--play-demo` prints. A restore that
is given the next keyframe the caller holds checks, when its replay reaches
that keyframe's tick, that the world digest is the keyframe's. A
different digest is a keyframe fault: the session lists it (`keyframe_faults`)
and the program logs `keyframe fault at tick N`, so that a field the keyframe
missed shows in the field as a log line, not as a replay that is quietly wrong.

#### What a keyframe holds

Everything the next tick reads is in one of three places: kept, kept as the
player has it, or not kept for a reason that holds in every playback. A
playback has no order, no computer player and no unit steered on this machine:
the recorded players are mirrored, and the viewer owns nothing. A session that
holds one of them is refused with a sentence (`demo_keyframe_take` returns
false), never written wrongly. The sentence is for the log and the check, in
English: a viewer that cannot take a keyframe tells the player in its own
words.

**The world** (`oa::World`, `oa/core/world.h`). Every table, as bytes:

| Table | Records | Size |
|---|---|---|
| `game` | the game state | 234,061 bytes |
| `units` | the unit pool | 280 bytes a slot; ten players times the unit limit |
| `unit_types`, `feature_types`, `extra_weapons`, `extra_sides` | the types and sides | the tables never change in a match; kept for exactness, they cost nothing against the base |
| `projectiles` | the 300 shots | 107 bytes each |
| `economy`, `player_setup` | the players' economy and setup blocks | 520 and 2,035 bytes |
| `plots` | the map's cells | 13 bytes a cell |
| `sight_grid` | the map's sight words | the match keeps its sight in its own grids, below, so this table is empty |
| `placed_features` | the placed-feature pool | 48 bytes a record |

The loose fields of `World` (the table sizes, the map's water damage) are set
when the match is built and never change. The game state comes back whole but
for three runs of bytes that belong to the player and the clock: the video
capture's settings, the profiler's times, and the chat ring and its two
positions. The viewpoint, the local player and the pause bit come back as they
were at the keyframe, so the viewer sets its own again after a restore.

Part of the camera and the interface lives in the world's own tables, which a
keyframe copies whole, so a restore puts it back as it was at the keyframe:

- in the game state, the followed unit and point (`follow_unit`,
  `follow_target`, `follow_point`, `follow_point_ticks`); the camera bookmarks
  (`camera_slot_x`, `camera_slot_y`, `camera_slot_valid`); the glide target
  (`camera_target_x`, `camera_target_y`), `camera_flags` and the camera shake
  (`shake_duration`, `shake_remaining`, `shake_amplitude_x`,
  `shake_amplitude_y`); `scroll_speed` and `debug_overlay`;
  and the pointer, the cursor and the drag box (`pointer_state`,
  `saved_pointer_state`, `pointer_command`, `pointer_flags`, the `cursor_*`,
  `drag_start`, `drag_end` and `mouse_look_*` fields);
- in each unit record, the selection (`OA_UNIT_FLAG_SELECTED` in
  `Unit.flags`).

`camera_x`, `camera_y`, the view's size and `radar_view_rect` are written
from the runtime on every drawn frame (`Runtime::bind_match_view`), so a
restore does not keep them. The rest of that list is the replay's as it was.
After a restore the viewer writes back the picture it held before the
restore: the pause bit, the viewpoint, the local player, the followed unit
and point, the camera bookmarks, the overlay, and each unit's selection.
Only the pause bit is written again when the jump lands, because each tick
of the skip clears it.

The chat ring is not in the picture. A keyframe leaves it out, so a restore
would keep the lines the player already has and the replay would post them
again. Giving the picture back clears the ring.

**The match** (`class Match`, `oa/sim/match_runtime.hpp`). Every data member:

| Members | What they hold | In the keyframe |
|---|---|---|
| `state_`, `simulation_`, `world_`, `slots_`, `units_`, `map_listeners_` | the world's tables, the side tables and the views over them | the tables, above; the views own nothing |
| `state_.assets`, `bridge_` | each slot's model instance and script interpreter, the generation of its instance, its first-sight flag | the interpreter's contexts, statics and pieces (`Vm::export_state`, in the format a saved game keeps), the generation, the cached image word and the flags; an instance is built again for a slot that holds another type |
| `movement_` | a movement object for each ground unit | geometry, movement, mirrored route points, previous vector, occupancy tick; made again for a slot that holds another type |
| `air_drivers_`, `mirrored_air_goals_` | each aircraft's driver and goal | bytes |
| `weapons_` | each unit's three weapon slots | bytes |
| `spatial_units_`, `spatial_` | the units' projection, the plots' occupants, the buckets, the list of plots written | bytes, with `fill_round` and `last_filled` |
| `sight_`, `other_player_coverage_`, `plot_metal_` | each player's coverage and sight words, the metal under each plot | bytes |
| `sightings_`, `sighting_slots_`, `seen_before_`, `seen_before_count_`, `seen_before_room_` | what each player has seen, and its seen list at the last refresh | the counts, the lists' fronts and the lists; the room a list has is not kept |
| `effects_` | the ten emitter layers and their ring, five particle pools, 300 explosions, 100 debris pieces, 300 shatter fragments | bytes; see below for why |
| `random_`, `lcg_seed_` | the shared stream and the `rand()` stream | the two states |
| `wind_`, `environment_wind_` | the wind | bytes |
| `groups_`, `wrecks_` | the squads, the corpses placed | lists |
| `player_alliances_`, `weapon_sweep_cursor_`, `remembered_sight_` | alliance rows, each player's weapon sweep cursor, the viewer's dead units' stamps | bytes |
| `scenario_`, `outcome_state_`, `outcome_result_`, `local_game_ended_`, `load_finished_`, `load_pass_waiting_`, `load_tick_`, `occupants_projected_` | the conditions and the end of the game, whether the load's pass has run | bytes |
| `rule_state_` and what it names (`wind_generator_`, `structure_gifts_`, `repair_remainders_`, `slot_reuse_ticks_`, `build_site_kickout_spots_`, `allied_sight_state_`) | a mod's rule state | each table's bytes through its own `bytes` and `restore` |
| `search_`, `movement_classes_`, `occupancy_feed_` | the path search and the movement maps | not kept: only a search reads them, and a unit steered elsewhere has no navigator, so none runs |
| `orders_`, `order_buckets_`, `target_observers_`, `spare_order_`, `spare_ground_` and the order queues in `state_.orders` | orders | not kept, and refused when any exists: no unit of a playback is ordered |
| `strategic_states_`, `strategic_types_`, `strategic_environment_`, `computer_player_state_`, `seat_hooks_`, `seat_state_` | computer players and brains | not kept, and refused while a computer player plays: none does in a playback |
| `player_contacts_`, `player_contacts_kept_`, `player_contacts_tick_` | radar and sonar pictures kept for the players asked | not kept, and refused when one is kept |
| `selection_`, `speech_hooks_`, the hooks (`event_hooks`, `event_listeners_`, `event_listener_count_`, `next_event_listener_id_`, `multiplayer`, `kill_board`, `last_unit`, `respawn`, `watch`, `observer`, `order_panel`, `point_sound`, `named_sound`, `meteor`, `profile`, `combat_activity`), `trace_`, `fault_`, `applied_shot_`, `slotless_viewer_`, `slotless_view_switched_` | the order panel's unit, the sounds, the interface's hooks, diagnostics and what the viewer chose | not kept: the match's own interface state and its sounds are not in a keyframe. The camera and interface fields of the game state, and each unit's selection flag, are, in the world's tables above |
| `input_`, `services_`, `unit_type_rules_`, `weapon_rules_`, `type_bounds_`, `terrain_`, `turned_yard_starts_`, `turned_yard_cells_`, `collision_terrain_`, `scenario_gravity_`, `lava_world_`, `defeat_allowed_`, `host_stays_watching_`, `campaign_outcomes_`, `multiplayer_outcomes_`, `flash_tiers_`, `effect_sequences_resolved_` | what the match was built from | not kept: fixed when the match is built |
| `outcome_view_` | the outcome check's view of the viewer: its player, its alliances (`local_allies`), the live-unit counts and the units of player zero | not kept: the counts and the units are filled again before every check. `local_allies` is not refilled: alliance changes keep it up (`configure_player_alliances`, `follow_player_alliances`), so it is left out only because the viewer's alliances never change in a playback (the match's alliance rows, `player_alliances_`, are kept) |
| `settled_spending_` | what each player's last settlement paid out | not kept: the computer players read it (`Match::settled_spending`) and the tick never does |
| `outcome_unit_status_`, `sighting_query_`, `target_projection_`, `seen_work_`, `seen_work_room_`, `commander_placement_`, `disc_check_loss_` | scratch the next tick fills before it reads, a placement the tick clears, and a loss that is never active | not kept |

**The network match** (`oa/netgame/match`). `NetMatch` (the phase, the load
barrier, the counters the demo summary prints, the rules, the recorder's
session, the votes, the integrity check, the recorder's take) is copied whole.
Most of it is values; it also holds the pointers to the connection, the world
and the match rules, and the hooks. Those are the session's own and are the
same at the restore. `PacketLayer` is copied as bytes: the receive state with
the frames the layer holds, the condensers, the sequence numbers and the
traffic counters. `NetConnection::economy_periods` and `MatchBinding`
(the movement record of each slot and its counters) are kept.

**The playback** (`oa/session/demo.hpp`). `DemoPlayback`'s cursor, tick and
statistics; `DemoSession`'s tick errors, last error, the full records' statistics,
the type and place each slot's last full record gave, the tracked unit's
samples and the economy samples. The recording's frames, its players and the
battle room are fixed when it is loaded. The lines waiting for the console are
dropped.

**The program** (`Runtime`). The meteor storm's state lives in the runtime and
the match's next tick reads it (`Runtime::meteor_state_bytes`), so the keyframe
carries it through the session's extension.

Four things the inventory found differ from what the plan expected:

- **The effects are kept.** The plan left them out ("after a seek the
  explosions in progress are gone"), but the particles draw from the
  simulation's own `rand()` stream, which the wind and the features that spread
  and burn draw from too. A restore without them changes that stream from the
  first particle on, and with it the wind and the trees, which the viewer
  sees. The effect world is two megabytes of mostly empty pools and compresses
  to a few kilobytes.
- **The meteor storm is the runtime's.** Show Down's storm drew its next meteors
  from the same stream, and the digests differed 1,465 ticks after a restore
  until the storm's state was in the keyframe.
- **The profiler's times and the chat ring differed between two runs of one
  replay**: the first holds the clock, and the second an elimination notice
  whose phrase is drawn from the message log's own random stream, which the
  match does not hold. They stay as the player has them, and no keyframe
  carries them.
- **Part of the camera and the interface is in a keyframe.** The plan said
  neither is. The followed unit, the camera bookmarks, the glide target, the
  overlay, the pointer and each unit's selection flag live in the world's own
  tables, which a keyframe copies whole. Nothing shows that no tick reads them,
  and the fixtures prove the restore as it is, so they stay in, and a restore
  puts them back as they were at the keyframe (the list is after the world's
  table). The viewer puts back its own.

An effect emitter and a smoke puff are written without the bytes the compiler
leaves between their fields, which hold what the last write to the slot left,
so that two moments of one run in which the replay holds the same state are
the same bytes. So are the records made on the stack and then copied in: a
player's sighting counts and seen units, a wreck, and the network binding's
movement record of each unit. Their padding held what the stack held there,
and the address sanitizer's build, which moves the stack from call to call,
made the check's state line differ in them while every digest was the same. Most records keep the compiler's padding, and the match's
sections hold this run's addresses: an aircraft's driver and goal point at its
unit and goal records, the packet layer points at its transport and the game's
player table, and a unit's spatial record points at its type's yard map. An
image therefore means something only to the run of the game that took it, and
the image of one tick compresses to a few bytes more or less from one run to
the next: 720 at tick 21,482 compressed to between 53,347 and 53,477 bytes in
nineteen runs. The state line of the check compares two images of one run.

#### What a keyframe costs

Measured on the Mac Studio, a Check build, with the other builds and tests of
the machine running, for each of the six recorded 3.1c games at its middle
tick. The sizes are from the earlier measurement, the median of ten runs of
`--replay-keyframe-check` (five at a load average of 10 to 15 and five at 20
to 46). The Take and Restore columns are a later measurement of the take a
viewer makes, which leaves the hash off: the median of three runs of the same
check, again with other work on the machine. The base is the first keyframe's
fixed sections, kept once and uncompressed: it is the cost of the map and of
the unit pool. The take is timed as a viewer makes one, with the default
options of `demo_keyframe_take`; the size without the base comes from a second
take at the same tick that also copies and compresses the image alone, which
the clock does not see.

| Game | Tick | Raw | The base | What is in play | Compressed | Without the base | Take | Restore |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 720 Show Down | 21,482 | 11.3 MB | 11.1 MB | 0.2 MB | 53 KB | 215 KB | 3.2 ms | 3.0 ms |
| 721 Coast To Coast | 20,785 | 9.1 MB | 8.8 MB | 0.3 MB | 38 KB | 156 KB | 2.6 ms | 2.4 ms |
| 884 Caldera's Rim | 40,778 | 53.1 MB | 52.2 MB | 0.9 MB | 161 KB | 401 KB | 12.1 ms | 11.9 ms |
| 5575 Painted Desert (49 min) | 43,783 | 31.2 MB | 30.4 MB | 0.8 MB | 149 KB | 1,018 KB | 8.3 ms | 7.1 ms |
| 5574 Painted Desert (2 h 35) | 125,300 | 24.2 MB | 22.0 MB | 2.2 MB | 451 KB | 1,332 KB | 11.3 ms | 7.6 ms |
| 5573 Painted Desert (4 h 27) | 234,928 | 23.9 MB | 22.0 MB | 1.9 MB | 544 KB | 1,426 KB | 11.9 ms | 7.6 ms |

- A keyframe's raw size is the map and the unit pool (fixed, 9 to 52 MB) and
  what is in play (the match's units, scripts and effects: 0.03 MB at the
  start of a game and up to 2.6 MB in a big one). Against the base it
  compresses to 7 to 13 KB at the start and to 38 to 622 KB later in a game.
  Compressed alone the same bytes are 1.8 to 6.8 times larger later in a game,
  and 17 to 94 times larger at its start.
- In that earlier measurement, while every take hashed the image, taking
  one cost 15 to 84 ms (the slowest single run, 884's, took 107 ms) and
  restoring one 2.2 to 9.4 ms. A viewer's take leaves the hash off. The
  Take and Restore columns are the median of three runs of
  `--replay-keyframe-check` at these ticks, on a busy machine: 2.6 to 12 ms
  to take and 2.4 to 12 ms to restore. A restore does not compress. Both
  grow with the map and the number of units in play, and neither with the
  length of the game.
- The base counts in the budget: 9 to 52 MB of the fixtures' maps, held once.

**Where the time of a take went, while every take hashed the image.** A
scratch build that timed each step (not kept; its totals are within 5 % of
the table's) gave, as the median of five runs at the same ticks:

| Game | A take | Writing the image | Hashing it | Against the base, copying, compressing |
|---|---:|---:|---:|---:|
| 720 | 17.6 ms | 0.9 ms | 14.0 ms | 2.7 ms |
| 721 | 14.5 ms | 1.0 ms | 11.3 ms | 2.2 ms |
| 884 | 83.3 ms | 3.3 ms | 65.8 ms | 14.2 ms |
| 5575 | 50.5 ms | 2.4 ms | 38.7 ms | 9.4 ms |
| 5574 | 43.8 ms | 4.0 ms | 30.0 ms | 9.9 ms |
| 5573 | 45.7 ms | 4.0 ms | 29.6 ms | 12.1 ms |

The hash took two thirds to four fifths of that take. It is
`DemoKeyframe::image_digest`, an FNV-1a over every byte of the image. A
viewer's take leaves it off; a unit test that compares the hash asks for
it. The check compares the images themselves. Compressing, at zstd level
1, was 1.2 to 6.6 ms of the rest.

**A take's working memory.** A take compresses the writer's two regions as
one frame, without copying them into one image first, into one buffer the
session keeps and reuses. The buffer is a little past `compress_bound` of
the image, so the next image, only slightly larger, reuses the same block.
It is not filled with zeros, and it is not shrunk after the take. Only the
bytes the frame wrote are copied into the keyframe. The session keeps the
writer's uncompressed image too, and the next take builds into it. The
budget counts the room that writer holds, both regions' capacity, which the
first take may leave larger than the bytes it wrote, and the compress block
the take will hold: the block already kept, when it is large enough, or
`compress_bound` plus the spare room. Both are counted with the base. A take
that would pass the budget is refused before the base is stored and before
the block grows, and jumping back then says the device does not have the
memory. On iOS the budget is 96 MB. 884's base is 52 MB, so the base, the
writer's room and one compress block do not fit: the first take is refused
before any base is stored, and 884 on iOS holds no keyframe. A refused take
releases the image. A restore decompresses into the same buffer. (The
check's own process peaks higher than a take does: it also holds the two
images it compares and the measurement's buffer.) An earlier measurement
zero-filled a fresh buffer on every take, and a replay of 884 then held 0.9
to 2.6 GB. With the compress buffer reused, and with each take still building
its image in a fresh buffer, the maximum resident set size on the Studio was
376,913,920 bytes for a plain replay of 884 (no keyframes) and 675,315,712
bytes for a plain replay of 5573. Watching 884 peaked at 619,413,504 bytes
over two jumps and at 626,638,848 bytes over the jump from tick 75,100 back
to 73,300. Watching 5573 over its two jumps peaked at 1,026,588,672 bytes.
Neither replay refused a take: the counted base, image, compress buffer and
keyframes stayed inside the computer's 256 MB. On 884's forty jumps back,
from tick 20,000 to 78,500, the compress block was re-made at the exact new
`compress_bound` each time that bound rose: 177 blocks, from 52,410,363 to
54,527,886 bytes. Each freed block was too small for the next, so the
allocator kept them all. Keeping the image while still re-making the block
left the peak after the forty jumps at 1,503,182,848 bytes. The spare room
ended the climb: with it, eight runs of the same forty jumps peaked between
617,676,800 and 640,942,080 bytes, inside the budget plus the plain replay's
peak (651,149,312 bytes in the run that measured both). With neither change,
those jumps peaked at 1,372,651,520 bytes.

**The pause a take makes on its frame.** A take runs between two frames, on the
thread that steps the replay, so the frame that takes it is that much longer.
Near the play head a keyframe is due every 300 ticks: every 10 seconds at 1×
and every 0.625 seconds at 16× (480 ticks a second). Each costs the take time
of the table:

| Game | A take | At 1× (frames of 16.7 ms) | At 16× (share of 0.625 s) |
|---|---:|---:|---:|
| 720 | 3.2 ms | < 1 | 0.5 % |
| 721 | 2.6 ms | < 1 | 0.4 % |
| 884 | 12.1 ms | < 1 | 1.9 % |
| 5575 | 8.3 ms | < 1 | 1.3 % |
| 5574 | 11.3 ms | < 1 | 1.8 % |
| 5573 | 11.9 ms | < 1 | 1.9 % |

A take stays on the frame, after a whole tick and before the next drawn
frame. A take on another thread would race the world it is reading. The
hash of the image is off unless a test asks for it, so a take the viewer
times does not walk the image. The first keyframe, the base, is taken while
the replay loads, before the first drawn frame, so that cost falls on
loading. The table is the median take from the same three runs as the size
table. At 1× each of them is shorter than one frame. At 16× the share is
that take divided by 0.625 s, the time between two keyframes.

#### The intervals and the budget

The plan's numbers hold, and the measurement says why.

- **A keyframe every 10 seconds of game time (300 ticks) within two minutes
  either side of the play head, and every 60 seconds (1,800 ticks) elsewhere.**
  Undrawn the replay runs 4,100 to 18,000 ticks a second (the table of the
  index pass above), so the 1,800 ticks of a minute take 0.1 to 0.44 seconds.
  A restore of the nearest keyframe then a replay to the target takes under
  half a second: the longest stretch is 60 seconds of game time, and the
  stretches near the play head are 10. In the earlier measurement, while
  every take hashed the image, a keyframe cost 15 to 84 ms: 10 to 25 % of
  the time to replay a minute on 720, 884 and 5573 (0.10, 0.34 to 0.40 and
  0.41 to 0.43 s), which is what a skip adds away from the play head, and
  0.6 to 1.5 times the time to replay the 300 ticks between two near
  keyframes (16 to 72 ms). Two thirds to four fifths of that was the hash.
  With the hash left off, the same check now takes 2.6 to 12 ms.
  Near the play head the price is also a pause on the frame that takes the
  keyframe. The intervals hold as they are. The pause is paid on the frame,
  because the take reads the world and cannot run beside it.
- **256 MB on a computer and 96 MB on iOS, counting the base.** A game kept
  whole at those intervals takes, with the base: 720 14 MB, 721 11 MB, 884
  63 MB, 5575 41 MB, 5574 95 MB and 5573 178 MB (the four and a half hours of
  Painted Desert: 281 keyframes of about 0.55 MB and the 22 MB base). Every
  fixture fits a computer's budget with room to spare. On iOS 5573 and 5574 do
  not fit whole: the keyframes furthest from the play head go first, the sparse
  ones last, and the replay still jumps from the ones near it. 884's map
  makes its base 52 MB, over half of 96 MB. A take counts the compress
  block, including its spare room, and the room the writer holds, with the
  base. On a map as big as Caldera's Rim those three pass 96 MB, so the
  first take is refused before any base is stored, and 884 on iOS holds no
  keyframe. Jumping back says it needs more memory than the device has.
  A computer's 256 MB holds the base, the writer's image, one compress
  buffer and the keyframes the schedule keeps. 884 and 5573, watched
  through the jumps above, kept that schedule: no take was refused.
- **Thinning.** "Every 10 seconds near the play head" means the near keyframes
  are thinned to the 60-second interval once the play head leaves them: a
  keyframe more than two minutes from the play head that is not on a minute is
  dropped. The totals above already assume it. Without it, 5573 watched through
  at 1× would hold 1,566 keyframes (469,856 ticks, one every 300), about 860 MB
  at the same 0.55 MB each and 880 MB with the base, over three times a
  computer's budget. The viewer takes the near keyframes as the play head
  moves and thins them behind it; the interval and window constants are in
  `demo.hpp` (`keyframe_near_interval_ticks`, `keyframe_far_interval_ticks`,
  `keyframe_near_window_ticks`). (Taken at twelve ticks across 5573, a keyframe
  averages 0.47 MB, so these totals are about a sixth high.)

#### The check

`--replay-keyframe-check TICK` (new, headless, with `--play-demo`) plays to
`TICK`, takes a keyframe, plays 3,000 ticks and records the digest, restores the
keyframe and plays the same 3,000 ticks again, and prints

```text
keyframe at 21482: raw 11332145 bytes (fixed 11106443, varying 225702), compressed 53453 bytes, ...
keyframe check at 21482: digest afe90061861cd185, restored afe90061861cd185, same
keyframe check at 21482: state at tick 24482: same
```

`taken in`, in the first line, is the time of one take with the default
options, as a viewer makes one; `without the base` comes from a second take of
the same tick, which the clock does not see. The check then plays on to
`--match-ticks`. `same` says the two runs reached one digest and had one digest
at every tick between. The state line compares the match's image where both
runs ended, section by section, and names a section whose bytes differ: a field
the keyframe did not carry shows only through a section it changes within the
3,000 ticks. The run exits 1 when the digests differ or a keyframe fault is
reported. The state line is printed, not counted in the exit status, and
`native-replay-keyframes` requires it to say `same`. The replay ends on the
digest and the frame count `demos.tsv` pins whichever tick it checks at, and
the six recorded games were checked at 1 %, 50 % and 95 % of their end ticks
and at tick 0, 21 other ticks of 720, 9 of 721, 4 of 5575 (two of them across
its record error) and 4 of 884: every check printed `same`.
`native-replay-keyframes` records a loopback game and checks its start, its
middle and its end.

### Playback

While a replay plays, the viewer's clock decides how many ticks run in a
frame, and only the last tick of that frame is drawn. A speed the recording
sets still paces the replay, and the speed the viewer chose multiplies it. A
speed the recording sets does not put the viewer's speed back.

The speeds are ½×, 1×, 2×, 4×, 8× and 16× the recorded pace. **+** and **-**
step through them. **Skip** runs as fast as the machine can, and draws
nothing; a jump uses it. Skip is not one of the speeds **+** and **-** step
through. While a jump is skipping, **+** and **-** still step the speed
underneath, and the replay returns to it when the jump lands.

**Pause** holds the playback, and so does the in-game menu. The time a pause
holds is not played later. Above 4×, and while a jump skips, each unit sound
plays at most once a second and speech is quiet. Music plays on.

A frame that cannot run every tick it was asked keeps the speed it did reach,
so a slow machine does not fall behind without that being known. The ticks
that frame did not run are not added to the next one.

The first keyframe is taken while the replay loads, before the first drawn
frame, in the window and in a headless run that was asked to jump. A
headless run with no jump takes none. Later keyframes follow the schedule
[above](#the-intervals-and-the-budget): every 10 seconds of game time within
two minutes of the play head, and every 60 seconds elsewhere, under the same
memory budget. Past the budget, the keyframes furthest from the play head go
first and the ones on a minute go last. While a jump is catching up, a
keyframe is taken only when the schedule will still keep it at the target,
and thinning uses that target, so a keyframe taken for the landing is not
dropped because the head has not reached it yet.

A jump back, or forward to a moment already watched, restores the nearest
keyframe at or before the target and plays the rest at Skip. A jump past the
furthest moment watched plays forward at Skip, taking keyframes on the way,
and keeps how far that catch-up has got, as a percent of the way from where
it restarted to the target. A jump that lands on the tick it started from
stays there. A jump back with no keyframe at or before the target stays. The
log says so, and the state carries one sentence for the player: jumping
back is not available, or it needs more memory than the device has. A
restore that fails ends the playback, and the player is told the replay
could not jump to that time and has stopped.

`--replay-speed SPEED` watches at that speed. `SPEED` is `1/2`, `0.5`, `.5`,
`1`, `2`, `4`, `8` or `16`. In a `--headless-check` run the speed is kept
and the loop still steps one tick at a time, so the speed changes nothing
there. The window paces each frame from the milliseconds since the last
one. `--replay-seek FROM:TO` may be given more than once. Only the first
jump is armed. It fires when the play head reaches or passes `FROM`,
including a `FROM` already passed when the jump is armed, and the next jump
is armed when it lands. It prints

```text
demo seek FROM -> TO: digest HEX in N ms
```

`N` is the time the jump took, the restore included. A jump the viewer asks
for while one is waiting or running replaces it, and the latest target is
the one that runs. Neither option makes the run headless, and both need
`--play-demo`.

What a viewer can ask, besides those options, is to play, to pause, to set
the speed, to jump to a tick, and to read the state: the tick, the length
known so far, the furthest moment watched, the spans a jump can restore
into, the catch-up percent, the speed reached, and the sentence when
jumping back is not available.
