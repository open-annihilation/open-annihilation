# Controls

The keys a match answers, beyond the buttons on the order panel.

## Pause and the game menu

Pause toggles the simulation pause. On a Mac, Cmd+P does the same during a
match, without needing a Pause key.

F2 opens the game menu; Mac keyboards may require Fn+F2. In a local match,
Resume or another press of F2 closes the menu and resumes the simulation,
including a pause set with Pause or Cmd+P. In a shared match, closing the
menu leaves the synchronized pause unchanged; use Pause or Cmd+P to toggle
it.

Escape first cancels an armed command, then clears the selection. With
**Escape opens the game menu** enabled under Settings > Controls, another
press opens the game menu when there is nothing left to cancel. Escape
closes the open game menu as Resume does. Canceling a surrender dialog or
closing a team panel does not clear an explicit simulation pause.

## The 3D camera

With **Allow Full 3D Mode** on under Settings > Graphics and Full hardware
acceleration, Alt and a left drag on the battlefield turns the camera
about the point under the pointer (Option on a Mac), and Alt, Shift and a
left drag pans it. Alt and a middle drag turns it too, and in the rotated
view a middle drag pans while **Middle-click pans** is on. Alt+R puts the
classic view back, and Alt+U turns **Unlocked Camera** on or off. While
an order or a building is armed, Alt and a click places it unsnapped
instead. The rotated view needs a graphics card SDL can draw on through
Metal or Vulkan ([Graphics](settings.md#graphics)); with Hardware
acceleration Off or Basic, or where neither can be used, Alt and its drags
and keys keep their classic use.

## Groups

Ctrl and a digit from 1 to 9 stores the selection in that group. Alt and
the digit selects the group. With **Select groups without Alt** on
(`+switchalt`), the digit alone selects the group and Alt and the digit
picks a build button.

The first press selects the group, or adds it when Shift is held. A later
press, at once or after a pause, centres the view on the group when that
group is already exactly the selection. The view goes to the average of
the units' positions, and the camera does not
start following a unit. T follows the selection. Home centres on the
selected unit and does not follow it.

A gamepad's group buttons do the same. With the groups grip held, a tap
selects the group and a second tap centres on it. See
[controllers.md](controllers.md).

## Queueing with Shift

An order given with Shift held joins the end of the selected units'
queues. An order armed from its button, or a building being placed, stays
armed for the next click while Shift is held. Letting Shift go returns to
the default order, as in 3.1c: the next click gives the default order, and
with the left-click interface one right click drops the selection. An
order armed with none given yet stays armed when Shift is let go.

A layout can list that hold so a tap of Shift latches it. The next orders
join the queue without Shift held down, and a chip at the top of the
battlefield reads QUEUE · latched until Shift is tapped again. The same row
shows each other latched hold, and, with Sticky modifiers on, a tapped
Shift, Ctrl or Alt. Losing the window clears every latch and sticky
modifier, including a QUEUE latched from the touch controls.

## The window's edge

**Window edge**, under Settings > Controls, decides what a window does with
the pointer while a match's battlefield shows. It has three ways: Capture,
Sticky and Free. Full screen keeps the pointer on the screen whichever is
chosen.

**Capture**, the default, keeps the pointer in the window while you play.
No push and no flick takes it out, so it rests on the last pixel of every
edge and the view edge-scrolls there, as it does in full screen. The game
asks the system to confine it: the window server keeps it in the window on
macOS, the cursor clip on Windows, a pointer grab on X11 and the pointer
constraint on Wayland. The pointer is free again:

- in the game menu, Settings and the other panels it opens, and on the
  pause screen;
- after you press Escape, until you click the battlefield again;
- in Open Annihilation's own menus and setup, and outside a match;
- when the window loses the focus, with Alt+Tab, Command+Tab or a dialog
  of the system's.

A click on the battlefield captures it again. After the window lost the
focus, that first click only brings the window back: it selects nothing and
gives no order, and its release does nothing either. After Escape the click
captures the pointer and acts as any click does. Closing the game menu with
Resume, or the game going on after a pause, captures the pointer again
while it is over the window.

**Sticky** keeps the pointer on the window's edges, as firmly as **Edge
stickiness**, the slider under Window edge, says. It rests on the last
pixel and the view scrolls. A firm push outward, or arriving at the edge
quickly, lets it leave. After that it is held again only once it has come
back in and moved about 24 points away from the edges. Motion along an edge
does not let it go. Menus, the pause screen and a window without the focus
leave the pointer free, as they do with Capture.

**Free** leaves a window's edge as the game always did.

### Edge stickiness

Edge stickiness runs from 0.0 to 5.0 in steps of 0.1, 1.0 by default. It is
used only by Sticky: with Capture or Free the slider is greyed and says
Needs Sticky, and keeps its value for when Sticky is chosen again. At 1.0 a
push of about 300 points past an edge within half a second lets the pointer
go, and so does arriving at the edge at about 6250 points a second, 625
points in the last tenth of a second. An ordinary fast move toward the
edge during play stays held; a deliberate shove leaves. Both distances
grow with the level: 2.0 needs twice as much, 5.0 five times as much, and
at 0.0 the edge never holds, as with Free.

On macOS, Windows and X11 a sticky edge leaves the pointer free to move.
When it crosses an edge the game puts it back on that edge's last pixel and
reads how far past the edge the desktop pointer had gone. That distance adds
up while the push continues, and once it reaches the push the game stops
putting the pointer back until it has come in from the edge. Resting on the
edge adds nothing, so the view keeps scrolling.

On Wayland a program cannot place the pointer, so the game confines it to
the window while a sticky edge should hold. The flick is the approach: the
movement toward an edge over the last tenth of a second, including the
movement before the pointer reached the edge. A slow arrival stays held and
the view scrolls.

## Panning

A middle click on the battlefield starts a pan. A small mark stays where
you clicked, and the view scrolls from that mark towards the pointer. Close
to the mark the view stays still. Further away it speeds up, up to the
game's Scroll speed, and the pointer shows the way. Another mouse button, a
turn of the wheel, any key or leaving the window stops the pan, and that
press or key does nothing else. A middle press that moves before you
release it pans while you hold it and stops when you let go.

It does not start while the allied whiteboard is in use, in the menus, or
on the panels. Settings > Controls, **Middle-click pans**, turns it off.
