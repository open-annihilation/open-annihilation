# Controls

The keys a match answers, beyond the buttons on the order panel. Every
key named here can be moved in the [key editor](keys.md#the-key-editor).
The chapter is [Keys](keys.md).

## Keys in a match

Ctrl+/ (Cmd+/ on a Mac) opens a translucent panel beside the side column
listing the keys that work right now: "Keys while playing", "Keys while
watching", or "Keys while placing a building". The keys are grouped as the
key editor groups them, with the keys a mod's helpers add under "This mod".
A run of numbered keys is shown as a range, such as Ctrl+1…9. The panel
names the layout in use and how many of your own changes it carries, and
the latches and sticky modifiers that are on show as chips in its "On:"
line instead of on the battlefield. EDIT KEYS… opens the key editor, which
pauses a game you play alone. Escape, or Ctrl+/ again, closes the panel.
The match keeps running underneath, and a click or a key the panel does not
use goes to the match. With **Move overlays** on,
[Arrange overlays](#arrange-overlays) puts the panel elsewhere on the
battlefield.

With **Show keys on buttons** on (Settings › Controls; it is off by
default, so the game's own panel looks as it always has), each order button
wears its key in its top right corner and each build cell its key in its
bottom left. An order button's underlined letter, where its caption has
one, is the letter that presses it, so a key you move leaves no line under
the old letter. Turn **Show keys on buttons** off again to take the badges
away. The panel's own key is **Keys overlay**
(`ui.keys-overlay`), Primary+/ by default (Cmd+/ on a Mac), and it moves
in the editor.

Resting the pointer on an order button shows that order's name in the
bottom panel, and its description ending with its key. A build button's
cost line ends with its cell's key when that cell has one. While you place
a building that can turn, the hint beside it is made from the rotate key and
the wheel you have bound, such as "Press /, or Alt+wheel, to rotate".

## The key editor

Edit keys… on the key layout card in Settings › Controls, EDIT KEYS… on
the keys panel and in the Library, and CHOOSE… beside Hold keys that latch
open the key editor. It shows every key and button the game answers, and
lets you change them. It has three tabs: **Commands**, **Keyboard** and
**Mouse**. Every command it lists moves here.
[Keys](keys.md#the-key-editor) is the full account.

**Commands** opens on Common commands, the two dozen commands players
change first (Attack, Move, Stop, Patrol, Guard, Repair, Reclaim and the
rest), each with its area beside it. The other areas follow, and All
actions, every one, comes last. The filters are All, Changed, Clashes,
Unbound and Latchable. Typing in the search finds commands by their names,
descriptions, areas and 3.1c's own order words; in Common commands it
searches every action, the common ones first. A row shows the command,
Changed while your edits name it, and its keys. The chosen command's
details sit beside the list, or, at Compact, on a page of their own that
‹ leaves: what it does and where it works, each key with **Change…** and
a mark that removes it, **+ Add a key**, More options (Latch and Hold to
confirm where the command allows them, and Find on keyboard), and at the
foot where its keys come from, with **Reset**.

**Find by key**, beside the search, listens for a key as the editor does
when you change one, then lists every command on that key, in every place
it works, each with Open ›, which shows that command in the list. It
changes nothing. Escape tapped closes it; Escape held for a second looks
Escape itself up.

### Changing a key

Change… or + Add a key opens "New key for {command}" and listens: press
the key, with its modifiers, or a mouse button or a wheel turn in the well.
Escape tapped cancels; held for a second it binds Escape. Once the key is
heard, a line under the well says what it would do:

- "Ctrl+Q is free while playing", and **Use Ctrl+Q for {command}**
  writes it;
- "Ctrl+B is in use while playing", with the commands that use it, and
  **Choose what happens…** opens the clash question;
- or why that key cannot be used.

A click in the well listens again. More options holds **Same place on
every keyboard**, which binds the key by where it sits rather than by its
letter, and how it acts: a press, a double press or a hold.

The clash question, "Ctrl+B is already in use", lists each way out with
what it leaves on both commands: **Move it to {command}**, and **Swap
them** when you are changing a key that is already bound. A press on a way
out only marks it; the button named after it writes it. **Choose another
key** listens again, and **Cancel** changes nothing. A clash already in
your edits, after a paste for example, shows on the command's details as
"{key} does two things", with **Settle…**, which opens the same question
with Keep it for this command and Keep it for the other. While a clash
stands, Save changes waits.

### The keyboard and the mouse

**Keyboard** draws your keyboard, ANSI, ISO or JIS, from the system or the
choice beside it, each key with its legend as your keyboard prints it and
tinted by its area; the legend under the picture names the tints. Holding
Shift, Ctrl, Alt or Cmd shows that layer. A click on a key, or Enter on
it, only shows what it does, in the inspector beside the picture (under it
at Compact): what the key does in each place, **Change {command}'s
key…**, and **Use {key} for {command}…** for the command chosen in the
list. Both open the change and wait for its button. **Show as list** lists
every key that does something in that layer, and Show keyboard brings the
picture back.

**Mouse** draws the buttons and the wheel with what each does in each
place, and whether clicks give orders with the right or the left button.
A click on a button, or on a role, opens the change for it.

### Your edits

Every change is an edit: the footer counts them ("2 unsaved edits") beside
**Undo**, which takes the last one back, as Ctrl+Z does (Cmd+Z on a Mac)
while the search is not being typed in. **More** holds two groups under
their headings: Layouts (Import New Layout…, Clone as New Layout…, Rename
and Describe… on a layout other than OA default, and Restore Defaults)
and Sharing (Share My Custom Keys… and Paste Someone's Layout…). At
Compact the layouts come first. **Save changes**
keeps the edits. **Cancel**, the close mark or Escape with edits not saved
asks "Save your edit before closing?" with **Save**, **Discard** and
**Keep editing**; while a clash holds Save back it offers Discard and Keep
editing only. Nothing you press in the keyboard, the inspector, Find by key
or the clash question changes a key until its own button writes it.

## Arrange overlays

OA's overlays stand where the game puts them unless you turn on **Move
overlays** in Settings › Accessibility (it is off by default). The
overlays are the keys panel (Ctrl+/, **Keys overlay**, `ui.keys-overlay`,
which moves in the editor), the chat line that stands over the
battlefield while the touch controls are on or while the line you type is
taller than the TALK field, and the chips that show latches and sticky
modifiers. With Move overlays on, they stand where you put them; turned
off, they go back where the game puts them, and your places are kept for
when you turn it on again.

**Arrange…**, on **Overlay places** under Move overlays, can be pressed
while Move overlays is on, on the main menu or in a match (the in-game
menu's **OA Settings** opens Settings). It opens a picture of the match,
drawn plainly: the side panel, the top bar, the bottom bar and the
battlefield, as they stand at your window's size. On it the keys panel
shows a few keys, the chat line a line being typed, and the chips a
latched QUEUE, each with a handle and its name, and a card with
**Reset**, **Cancel** and **Done**.

Drag a handle to move its overlay. It lands on an 8-point grid, measured
from the nearest edge of the battlefield or from its middle, and an
overlay always stays whole on the battlefield, never over the side
panel. Tab moves between the handles, Reset, Cancel and Done; an arrow key
moves the focused handle 8 points, 32 with Shift. A double-click on a
handle puts that overlay back where it stands by itself. **Reset** puts
every overlay back. **Done** keeps the places and goes back to Settings;
**Cancel**, or Escape, puts them back as they were when the picture
opened. The chat line moves only up and down: it keeps the battlefield's
width.

Each place is kept for the layout in use, Compact, Regular or Large
([Interface size](settings.md#user-interface)), so a 1280x720 and a
1920x1080 window share their places and a 640x480 window keeps its own.
The places are kept in your preferences as
`open-annihilation.overlay.<overlay>.<layout>`, where the overlay is
`keys-overlay`, `chat-line` or `latch-chips` and the layout `compact`,
`regular` or `large`, with values such as `l16,b8` (16 points from the
left, 8 from the bottom). Reset window positions, in Settings, leaves
them alone.

## Pause and the game menu

**Pause** (`game.pause`) toggles the simulation pause. Its keys are Pause
and Ctrl+Pause, and they move in the editor. On a Mac, Cmd+P does the same
during a match, without needing a Pause key.

**Game menu** (`game.menu`) is F2 and Ctrl+F2, and it moves in the editor.
F2 opens the game menu; Mac keyboards may require Fn+F2. In a local match,
Resume or another press of F2 closes the menu and resumes the simulation,
including a pause set with Pause or Cmd+P. In a shared match, closing the
menu leaves the synchronized pause unchanged; use Pause or Cmd+P to toggle
it.

**Back** (`game.back`) is Escape and Ctrl+Escape, and it moves in the
editor. Escape still closes the chat line and Open Annihilation's own
screens, which that binding cannot take away
([Escape's one fixed job](keys.md#escapes-one-fixed-job)). Escape first
cancels an armed command, then clears the selection. With
**Escape opens the game menu** enabled under Settings › User Interface, another
press opens the game menu when there is nothing left to cancel. Escape
closes the open game menu as Resume does. Canceling a surrender dialog or
closing a team panel does not clear an explicit simulation pause.

## Unit information

**Unit info** (`game.unit-info`) is F1 and Ctrl+F1, and **Pin unit**
(`game.pin-unit`) is Shift+F1 and Ctrl+Shift+F1. Both move in the editor.
F1 opens the unit information panel for the build button or the unit under
the pointer: its picture, costs and statistics. OK closes it, with either
mouse button, and so do Enter, Escape and O. As in 3.1c, what drops the
selection closes it too: a box dragged over open ground that selects
nothing, a right click on the battlefield in the left-click interface, and
a left click on open ground in the right-click interface, where a right
click on the battlefield gives the selection its order and the panel stays
open.

While the panel is open it takes the pointer from the side column, as in
3.1c: the side column's buttons and their keys do nothing until it closes.
A click or a drag that starts on the panel is the panel's, so it never
starts a selection box. The battlefield beside the panel, the minimap and
the game's other keys work as usual. The same holds with touch controls and
a gamepad.

## The 3D camera

**Orbit camera** (`pointer.camera-orbit`) is Alt with the left button, and
Alt with the middle button. **Pan camera** (`pointer.camera-pan`) is
Alt+Shift and the left button. **Classic view** (`camera.classic-view`) is
Alt and the place of the R key, and **Unlocked camera** (`camera.unlocked`)
is Alt and the place of the U key. Each moves in the editor.

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

Storing a group is `group.assign-1` through `group.assign-9`: Ctrl and
that digit's place, and the keypad. Selecting one is `group.select-1`
through `group.select-9`, Alt and the digit. Adding one is `group.add-1`
through `group.add-9`. **Follow** (`camera.follow`) is T, and **Centre**
(`camera.centre`) is Home. Each moves in the editor. Ctrl and a digit
from 1 to 9 stores the selection in that group. Alt and
the digit selects the group. **Number keys select groups**, in the key
editor, is 3.1c's SwitchAlt (`+switchalt`): with it on, the digit alone
selects the group and Alt and the digit picks a build button.

A press selects the group, or adds it when Shift is held, and leaves the
view where it is. Two presses of the same group's key back to back, within
the double-press time (Settings › Accessibility › Double press) and with no
other key between, also centre the view on the group. **Group keys centre
the view**, in Settings › Controls, changes that: On a double press is the
default, Never leaves the view, and On every press centres it when the
group is already exactly the selection, however long after the last press,
which is what 0.8.6 did. 3.1c leaves the view where it is on a first press
and on a second. The view goes to the average of the units' positions, and
the camera does not start following a unit. T follows the selection. Home
centres on the selected unit and does not follow it.

A gamepad's group buttons centre on the group when it is already exactly
the selection. With the groups grip held, a tap selects the group and a
second tap centres on it. See [controllers.md](controllers.md).

## Queueing with Shift

**Queue** (`order.queue-modifier`) is Shift by default, and it moves in
the editor. An order given with Shift held joins the end of the selected units'
queues. An order armed from its button, or a building being placed, stays
armed for the next click while Shift is held. Letting Shift go returns to
the default order, as in 3.1c: the next click gives the default order, and
with the left-click interface one right click drops the selection. An
order armed with none given yet stays armed when Shift is let go.

A layout can list that hold so a tap of Shift latches it. The next orders
join the queue without Shift held down, and a chip at the top of the
battlefield, or with Move overlays on where [Arrange overlays](#arrange-overlays) put the chips,
reads QUEUE · latched until Shift is tapped again. The same row
shows each other latched hold, and, with Sticky modifiers on, a tapped
Shift, Ctrl or Alt. Losing the window clears every latch and sticky
modifier, including a QUEUE latched from the touch controls.

## Helping a factory

**Guard** (`order.guard`) is G by default, and it moves in the editor. To
keep a builder helping a factory, select the builder, press G (or DEFEND
on the order panel) and click the factory. A guarding builder, whether a
construction vehicle, a construction aircraft or a commander, helps build
each unit the factory starts, one after another, repairs the factory when
it is damaged, and stays with it until you give it another order.

A click on the unit the factory is building helps build that one unit, as
in 3.1c: when it is finished the builder stops, and it does not go on to
the next unit in the factory's queue. Over the middle of a working
factory the pointer is on the unit being built, since in the classic view
the smaller of two units under the pointer is the one it picks: the
left-click interface shows the repair cursor there. In the right-click
interface a right click there also helps build that unit only, and a right
click on the factory itself, away from the unit, gives Guard while the
factory is undamaged; a damaged factory is repaired instead, and the
builder stops once it is whole.

## The window's edge

**Edge scrolling**, in [Settings › Accessibility](settings.md#accessibility),
is on by default. Turned off, the view does not scroll while the pointer
rests at the window's edge. The arrow keys, and the touch controls' edge
band while a drag is held, keep moving the view.
[Keys](keys.md#accessibility) lists it with the other helpers.

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

**Pan** (`pointer.pan`) is the middle button by default, and it moves in
the editor. A middle click on the battlefield starts a pan. A small mark stays where
you clicked, and the view scrolls from that mark towards the pointer. Close
to the mark the view stays still. Further away it speeds up, up to the
game's Scroll speed, and the pointer shows the way. Another mouse button, a
turn of the wheel, any key or leaving the window stops the pan, and that
press or key does nothing else. A middle press that moves before you
release it pans while you hold it and stops when you let go.

It does not start while the allied whiteboard is in use, in the menus, or
on the panels. Settings > Controls, **Middle-click pans**, turns it off.
