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

Clicks, drags and the pointer resting on the battlefield do not give
orders, change the selection, or pick a hover. The camera still pans, and
the wheel zooms. The wheel does not open the megamap. The classic view, an
unlocked view, the speed and pause still answer.

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

## Leaving

Escape leaves photo mode. Opening the game menu leaves photo mode first,
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
