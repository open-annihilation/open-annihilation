# Photo mode

Photo mode puts the battle's interface away and shows the battlefield
alone, from the place you were already looking. On this machine a skirmish
or a campaign mission stops where it is. A replay keeps playing until you
freeze it. A match shared with other machines keeps running.
Leaving puts you back in exactly that place: the same view, the same zoom,
the same follow, and the same order you had armed. Your selection is left
as it was.

It works in a skirmish, a campaign mission, a replay, a multiplayer game
you play, and a multiplayer game you watch.

## What you see

The battlefield fills the window. A line at the top left reads "Photo
mode" and "Esc leaves", on a dark strip so it can be read over light ground.

In a match shared with other machines, and while you watch, that line also
shows the game clock, the latest "under attack" line the battle posted
while photo mode was open, and how many chat lines have arrived since you
entered.

Selection boxes and health bars stay off. Each can be drawn on its own
over the bare battlefield. The cursor, the keys overlay and the touch
controls are hidden. So are the order lines, the build ghost, the
selection band, and the rest of the interface: message lines, the clock
on the panel, latch chips and key badges.

The notices the battle would have given you from the view you left stay
the battle's own. Moving the photo camera does not change which of them
are posted.

## While it is open

A click, and the pointer resting on the battlefield, do not give orders,
change the selection, or pick a hover. A drag on Pan moves the view. By
default that is the left button, and it does not use the Middle-click pans
setting. Middle-click pan still pans when that setting is on. The wheel
zooms when Mouse wheel zoom is on, and does nothing when it is off. Page
Up and Page Down zoom on the photo range either way. The wheel does not
open the megamap. The classic view, an unlocked view, the speed and pause
still answer. On a battle on this machine, + and − are slow motion: −
goes down from the match's speed through Slow 9 to Slow 1 and then
freezes, and + goes back up. In a
replay they are the replay's speeds. In a shared match they are taken and
do nothing. A display rule, a build tool and the chat macro do not answer.

Follow does not come with you. The battlefield stays where you were
looking, and the camera does not jump to a unit. Pressing follow follows
a unit for this visit, and a pan stops that follow. Leaving puts follow
back as it was when you entered: on, following the same unit, or off.

Home centres on the selected unit. With nothing selected it centres on
your commander and selects nothing. Other commands wait until you leave.
Opening the game menu, or Settings, leaves photo mode first.

A new window size lays the battlefield out alone at that size. Leaving
puts back the play layout of the size the window has then.

## Time

On a skirmish or a campaign mission, entering photo mode stops the battle
on the tick it has reached. No "Paused" title shows, and a save does not
record a pause. You can run it on, one tick at a time, or slowly. Slow
plays at 3 to 27 ticks a second, inside the speeds the battle allows.
With Smooth motion on, the picture glides between those ticks. You can
also pick the moment between two ticks, in eighths from the tick before
to the tick you stopped on. That choice is drawn whether or not Smooth
motion is on. Running on continues from the tick you are on. The time
the battle spent stopped is not played.

In a replay, stopping uses the replay's own pause, and slowing uses the
replay's own speeds. Slow lifts that pause, so the replay plays at the
speed you chose. The line says Frozen only while the replay's pause is
on. Stepping, and the moment between ticks, come later.

In a match shared with other machines, whether you play or you watch, the
battle is not stopped, slowed or stepped, and nothing is sent to the other
machines. You can hold the picture. The battlefield stays as it was, the
camera stays where it is, and the battle goes on underneath. Holding
again puts that camera back and shows the live battlefield. Leaving
shows it too. The line keeps the game clock, and that clock goes on.

The line names the state: Frozen, Slow and its ticks a second, or Held,
with the game clock.

## Camera

The view runs from the whole map out to eight times normal size, whatever
Maximum zoom out and Maximum zoom in are set to. Where the picture in use
cannot draw a zoom, the view reaches the closest it can draw in that frame.

Units stay models at every zoom. They are drawn as dots only when the
machine cannot spare the memory, or when they sit farther from the camera
than the models can be placed. A far view costs what it costs in play.

The view stays within View past the map's edge, as in play, in the turned
view as well. Follow follows a unit for this visit and centres it where
it is drawn. A pan stops the follow.

Turning and tilting work where the machine draws the turned view, within
the same limits as holding Alt and dragging. Classic view and Unlocked
camera work as in play. Elsewhere a turn is refused, and the reason is the
one Settings gives: Needs Full acceleration, Needs Allow full 3D mode, or
Not available here. A held picture is not turned, tilted, panned or zoomed.
The picture does not roll, and it is not a perspective view.

Leaving puts the zoom and the angles back.

## Keys

Photo mode is its own context in the key editor, titled "Keys in photo
mode", and its own area, Photo mode, with no legend letter. Every command
in that area can be moved. F9 opens photo mode while you play or watch.
F9 leaves it while it is open. They are different commands. Escape leaves
too, and no photo command takes Escape.

Enter takes a photo and does not open the chat line. A photo taken as photo
mode opens waits until a photo-mode frame has been presented, one frame, so
the picture is the battlefield photo mode shows and not the play layout.
Ctrl+F9 takes a photo here and saves a screenshot outside photo mode. Space
freezes a battle on this machine, and pressed again lets it run, or holds
the picture in a shared match and lets go of it. `.` steps one tick.
Shift+`,` and Shift+`.` move the shutter one eighth. Primary+C (Ctrl+C,
or Cmd+C on a Mac) copies the last photo. Home still centres and selects
nothing. Primary+/ (Ctrl+/, or Cmd+/ on a Mac) opens the keys panel, titled
"Keys in photo mode". It lists the keys that answer here: the photo keys,
the camera's pan, Centre, Follow and views, the speed, Pause, the game
menu and Escape, and the window's Settings, Full screen and the panel
itself. The match's other keys are taken and do nothing, so the panel
leaves them out, and Open chat, Status strip and Screenshot do not sit
beside Take photo and Freeze on Enter, Space and Ctrl+F9. The panel is
not in the photo. EDIT KEYS… leaves photo mode, then opens the editor, as
Settings does. Leaving puts the panel back as it was on entering, so one
opened during the visit closes. Stop, a click and Ctrl+C do not reach the
match. On Windows, Ctrl+C is Primary+C, so it copies the last photo and
still does not change the match.

The commands and their default keys are listed under Photo mode in
[Keys](keys.md#photo-mode).

## Leaving

F9 and Escape each leave photo mode. Opening the game menu leaves photo mode first,
then opens the menu. Opening Settings does the same, and then opens
Settings as it does in play.

Photo mode also ends, and puts the view back, when the match ends, when
you leave the match, when a game is loaded, or when the run quits.

Leaving a replay keeps the pause and the speed it had. Pause, and + and
−, change them as they do outside photo mode. Run puts back the speed
the replay had when you entered.

## Taking a photo

A photo saves the battlefield as you see it, at the window's size, in the
render mode in use. The saved picture is the battlefield alone. It is a
PNG, written where the screenshots go: `Screenshots` and the folder of the
mod you are playing, `Screenshots/default` with no mod, or the Image Output
Directory when you have set one.

While you hold the picture in a shared match, a photo saves the held
battlefield, as the screen shows it, not the battle going on underneath.

The name is `Photo <YYYY-MM-DD> <HH.MM.SS> <map>.png`, in your local time.
A photo of Canal Crossing taken at 13:12:08 on 5 November 2026 is
`Photo 2026-11-05 13.12.08 Canal Crossing.png`. A second photo in that
same second is numbered `2`, and a third is numbered `3`, before `.png`.

When the file has been written, the line says "Saved" and the file's name.
When nothing was written, the line says "Photo not saved" and why.

## What a photo records

A photo says which map and which moment it shows. Its title is the map and
the game time, such as "Canal Crossing, 3:41". The clock counts thirty
ticks as one second: minutes and seconds under an hour, and hours, minutes
and seconds from an hour on.

The description names the players, their sides and their teams, such as
"Steve (Arm, team 1) vs Ann (Core, team 2)". A player with no team is named
without one, such as "Player (Arm) vs Core (Core)". Player names are on
unless you turn them off. With the names off, the description gives the
sides, and a team only for a player who has one: "Arm, team 1 vs Core,
team 2", or "Arm vs Core" when nobody has a team. No player's name is
written anywhere in the file. Colours, sides and teams stay.

The file also names the program and its version, the date and time it was
saved (an RFC 1123 date in GMT, such as Sun, 06 Nov 1994 08:49:37 GMT), and
whether the battle was a skirmish, a campaign mission, a replay or a
multiplayer game.

One record, version 1, holds the engine version, the mod's id and version,
the map's name, the kind of game, the tick, the game time and the speed,
each player with side, colour and team, the camera, and the picture's size.
A team is numbered from 1, so the game's first team is team 1. A player
with no team has `team` null. A replay is named by its file name alone when
the photo is of a replay and player names are on. With the names off the
recording is its digest and its tick: a recording is often named after its
players, so the file name stays out. A look is written only when the
picture has one.

A photo never records a folder, an account, or where a machine can be
reached.

The choice is the preference `open-annihilation.photo.player-names`. It is
on when that preference is absent. Only the text `0` turns the names off.
The preference is written when you change it, and not before.

## Clipboard

Copy to clipboard is on until you turn it off. While it is on, the photo
you just took is also placed on the clipboard, so you can paste it into a
chat or an editor. The game keeps the choice as
`open-annihilation.photo.copy`: 1 copies, and 0 leaves the clipboard alone.
When the key is absent, photos are copied.

On a Mac, and on Linux, the clipboard receives the photo as a PNG. On
Windows it receives the photo as a BMP, which is what Paint pastes. The
file in your screenshots folder is a PNG either way. On a Mac and on
Linux the copy is the saved file itself, so it carries what the photo
records, the player names too unless you turned them off. The BMP on
Windows carries the picture alone.

A photo is still saved when a side is above 8192 pixels, or when the
copied picture would be above 128 MB. It is not copied. When copying is
on, or when you copy the photo again, the line says so, and why. When
copying is off, in Steam's Game Mode, or where the clipboard holds text
only, that photo is skipped in silence and the line stays the saved
message. When it is copied, the line says Saved and the file's name, then
Copied.

On iPhone and iPad the clipboard holds text only, so a photo is not placed
on it. In Steam's Game Mode there is no desktop clipboard, so a photo is
not placed on it either. Text already on the clipboard is left as it is.
Copying the last photo again copies it even when Copy to clipboard is off:
the switch decides only whether each new photo is copied. That automatic
copy is skipped in silence in Steam's Game Mode, and where the clipboard
holds text only. Copying the last photo yourself says why, in those two
cases: not available in Game Mode, or the clipboard holds text only.

After the game quits, the copy stays on the clipboard on a Mac and on
Windows. On Linux, X11 and Wayland keep it only when a clipboard manager
takes it.
