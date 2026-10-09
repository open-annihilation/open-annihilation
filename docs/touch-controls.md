# Touch controls

Open Annihilation can be played with fingers on a touch screen: on an iPad
or an iPhone, and on a computer with a touch screen. The touch controls add
a few buttons around the battlefield and read taps, drags, holds, pinches
and two-finger gestures. Every order they give is one the mouse and the
keyboard can give, through the same paths, so a game played by touch saves,
replays and plays over the network exactly as one played with a mouse.

## When the touch controls are on

The touch controls switch on, for the rest of the run, when:

- the game runs on an iPad or an iPhone (the build for them always has
  them on; see [platforms/ios/README.md](../platforms/ios/README.md));
- a finger touches a touch screen that the system reports as a direct touch
  screen (a trackpad does not count);
- the command line passes `--touch-controls`, which shows them with no
  touch screen, for instance to see the phone layout on a computer with
  `--touch-controls --resolution 852x393`.

Until then nothing changes: with a mouse and a keyboard and no finger, the
game looks and plays exactly as without touch controls, at every window
size. Once they are on, a mouse, a trackpad and a keyboard keep working as
they always do, beside the fingers.

## Two layouts

The layout depends on the window's size in points (the size the system
measures windows in, not pixels): a window whose shorter side is under 460
points gets the **phone** layout, every other window the **tablet** layout.
Every iPhone held sideways is a phone; every iPad is a tablet. Both layouts
keep every control inside the screen's safe area, clear of rounded corners,
the camera and the home indicator. A larger **Control size** (below) makes
the window that many times fewer points: a Steam Deck's 1280×800 screen at
Larger is 853×533 points and keeps the tablet layout.

### Tablet (iPad)

The 3.1c screen stays as it is: the minimap and the side panel with the
order and build buttons on the left, the resource bar at the top and the
status bar at the bottom, drawn at the size the window gives them. The
touch controls sit on the battlefield beside them:

- **Thumb column**, at the bottom of the battlefield next to the side
  panel: **QUEUE**, **ADD** and **CLEAR**, with **x5** above QUEUE while
  the panel shows a build page.
- **Group bar**, along the bottom: a chip for each stored group, **STORE**
  and **SELECT ▾**.
- **Right rail**: **PAUSE**, **SPEED**, **CHAT** (in a multiplayer game),
  **CENTRE**, **FOLLOW**, **NEXT** and **INFO**.
- **MENU**, in the empty strip right of the resource bar (or at the
  battlefield's top-right corner when there is no room there): the in-game
  menu, as F2 opens it.

The side panel's buttons are pressed with a finger as with the mouse: the
order buttons arm their orders, the toggles change their states, and the
build buttons build.

### Phone (iPhone)

The battlefield fills the whole screen, edge to edge, and the controls
float over it:

- **Left column**: the **minimap**; **BUILD**, which opens the build
  drawer (**ORDERS** for a unit that builds nothing); **QUEUE** and
  **ADD**; **CLEAR** and **SELECT ▾**; zoom **−** and **+**.
- **Top**: a compact resource strip, and under it a status pill that says
  what a tap would do now ("TAP: MOVE · ENEMY: ATTACK"); **PAUSE** and
  **MENU** at the top right.
- **Order rail**, down the right edge: the orders the selection can take,
  as many as fit (six on most phones), in the order MOVE, ATTACK, PATROL,
  GUARD, STOP, REPAIR, RECLAIM, CAPTURE, LOAD, UNLOAD, D-GUN, and **MORE**
  last. The armed order is lit.
- **Group chips** along the bottom for the stored groups, and **+**.

**MENU** opens a small menu: Game menu (the in-game menu), Slower, Faster
and Chat. A phone match starts at zoom 1.25, so units are a little larger,
or at the game's own scale with Maximum zoom in at None; the zoom then
stays where the player puts it. A pinch and the zoom buttons keep to the
Maximum zoom out and Maximum zoom in settings, with Mouse wheel zoom off
too ([Settings](settings.md#controls)).

In both layouts the messages and panels drawn over the battlefield keep to
the part of it the controls leave clear: the message log starts at its top
left, the kills board (F4) hangs from its top right, and the chat line is
typed along its bottom edge, across its width (on a tablet too, over the
battlefield above the thumb column rather than in the bottom bar). The
megamap draws the map inside that part, and the commander placement's
prompt and Done button stand in it.

## Gestures

| Gesture | What it does |
|---|---|
| Tap | Selects a unit, or gives the order the cursor shows for what lies under the finger: a left click |
| Double tap a unit | Selects every unit of its type, as a double click does |
| One-finger drag | Tablet: a selection box; with ATTACK, RECLAIM or REPAIR armed, an area order. Phone: scrolls the map |
| Hold, then drag | A selection box, on both layouts |
| Hold, then lift without moving | Opens the order wheel at that point (below) |
| Two-finger drag | Scrolls the map, which follows the fingers and glides on a little after they lift |
| Pinch | Zooms in and out about the fingers; the map stays under them |
| Two-finger tap | Takes back an armed order or a building being placed; with none, clears the selection |

A tap does not have to land exactly on a unit: when nothing lies under the
finger, the nearest unit within about 12 points is taken. On buttons, in
menus and dialogs the nearest button within about 22 points is pressed. A
button shows pressed while a finger rests on it, and a finger that slides
off before lifting presses nothing, as with a mouse.

While a finger rests on the battlefield, the order cursor is drawn at the
finger; on a tablet the status bar names the unit under the finger. The map scrolls by itself while a selection box or a
building's ghost is dragged near the battlefield's edge. Resting a finger at
the edge of the screen does not scroll; only a real mouse pointer at the
edge does.

## QUEUE, ADD and x5

On a keyboard, Shift does three jobs. The touch controls give each its own
button:

- **QUEUE** is Shift for orders: the orders a tap gives join the end of the
  selected units' queues, area orders and buildings queue too, an armed
  order stays armed, and the queued orders are drawn on the battlefield.
- **ADD** is Shift for selecting: a tap on a unit adds it to the selection
  or takes it out, a box adds what it takes in, and a group chip adds its
  group.
- **x5** is Shift for build buttons: a factory's button adds or takes off
  five at a time.

Each latches with a tap (lit until tapped again) or works for as long as a
finger holds it. Because they are separate, a player queueing waypoints
with QUEUE latched who taps another unit switches to that unit rather than
adding it. A hardware keyboard's Shift still does all three. The Touch
setting **QUEUE and ADD** can make a latch turn off after one action.

## The order wheel

A finger held still on the battlefield and lifted opens a wheel of orders
around that point; the player then taps one. Its twelve places, clockwise
from the top, are Move, Patrol, Attack, D-gun (Load for a selection with no
D-gun), Capture, Stop, Info, Type, Reclaim, Repair, Unload and Guard. An
order the selection cannot take is greyed and does nothing; the order a
plain tap would give is ringed. A picked order is given at the held point,
exactly as arming its button and clicking there does. **Info** opens the
unit info panel for the unit at the point (or the selected unit); **Type**
selects every unit of that unit's type. The hub in the middle is **QUEUE**
for the next pick only: the order joins the queue and stays armed. A tap
outside the wheel closes it.

Holding and then dragging, instead of lifting, draws a selection box.

## Building

**Factories.** A tap on a build button adds one to the queue; a hold takes
one off (as the right button does); with **x5**, five. The factory's
ORDERS tab sets the orders its new units start with.

**Buildings.** A tap on a building's button shows its ghost in the middle of
the battlefield, with **✕ CANCEL** at the bottom. A tap on the ground moves
the ghost there, and a drag moves it under the finger. A double tap places
the building where it is tapped, and a hold places it: at the ghost when the
finger is on it, else where the finger is. On ground where the building
cannot stand the ghost shows refused, the game sounds its refusal and the
ghost stays. With **QUEUE** on, placing goes on after each building until
QUEUE goes off (as letting Shift go ends it), ✕, CLEAR or a two-finger tap.

**The phone's drawer.** BUILD slides in a drawer of the builder's buttons,
three across, with BUILD and ORDERS tabs, **x5**, and page dots with PREV
and NEXT. Choosing a building closes the drawer and starts its ghost; a
factory's buttons keep the drawer open. The close button in the drawer's
corner, BUILD again or a tap outside closes it.

## Other controls

- **The minimap.** A tap or a drag moves the camera. A tap or a hold with
  an order armed gives it there where the cursor shows it, and one while a
  building is being placed places it there. A hold with neither gives the
  selection the default order there with the right-click interface, as a
  right press on the battlefield does; with the left-click interface it is
  a left click on the minimap, which never moves the camera: it selects
  the unit of yours whose dot is under the finger, or gives the selection
  the order the cursor shows there, and with nothing selected over open
  ground it does nothing.
- **Groups.** A chip shows a stored group and its size. A tap selects the
  group, a second tap centres the camera on it, and a hold stores the
  selection in it. **STORE** (tablet) or **+** (phone) stores the
  selection in the lowest free group.
- **SELECT ▾** opens a menu: All, Builders, Factories, Aircraft, On screen,
  Commander, Same type, Centre, Follow, Next unit and Next report, each the
  key that does it on a keyboard (Commander with ADD on adds the
  commander).
- **CLEAR** takes back an armed order or a building being placed, else
  clears the selection, as Escape's first two presses do.
- **PAUSE** pauses and resumes the game, as the Pause key does. **SPEED**
  (tablet) offers Slower and Faster. **CHAT** opens the chat line.
  **CENTRE**, **FOLLOW** and **NEXT** are Home, T and N.
- **INFO** opens the unit info panel for the selected unit.
- **MORE** (phone) opens a sheet with the rest of the side panel's order
  page: the fire and move orders, on/off and cloak toggles, the orders the
  rail has no room for, **INFO**, and **SELF-DESTRUCT · HOLD**, which
  starts or stops the countdown after a one-second hold. On a tablet the
  side panel's self-destruct button also needs a one-second hold; a tap
  only says so.
- **Help.** A long press on a touch control, or on a button of the side
  panel other than a build button, shows what it does.

## With a gamepad

Once a gamepad has sent input, the touch layer shows the pad's part too
(the gamepad's own controls are described in docs/controllers.md):

- Before the first finger lands, a **slim pad HUD** goes over the 3.1c
  screen's battlefield: a pill at its top saying what R2 and L2 would do
  now ("R2 MOVE · ENEMY: ATTACK · L2 CANCEL"; with the right-click
  interface "R2 SELECT · L2 MOVE"), the **QUEUE**, **ADD** and **FORCE**
  chips under it, lit while held or latched (QUEUE reads **x5** while the
  pointer is on a build button), and the stored groups' chips along its
  bottom. Once a finger lands, the full touch layout takes its place.
- With the touch controls on, each control gains a small **badge** naming
  the pad buttons that give it: QUEUE R4, ADD L4, CLEAR B, SELECT ▾ D-pad
  left, PAUSE View+X, CHAT View+A, CENTRE L3, FOLLOW R3, NEXT D-pad right,
  INFO View and FORCE R5; a group chip shows its button while L5 is held.
  Badges change how controls look, never where they are, and are drawn in
  the glyph set the **Button prompts** setting chooses (Off hides them). A
  long press's help names the pad input too ("On the pad: hold or tap R4").
- **FORCE** tops the tablet's thumb column while the pad has back grips: Ctrl
  for orders and selecting for as long as a finger (or R5) holds it, so a
  click forces fire. QUEUE and ADD are one set of latches for fingers and
  grips: a tap on QUEUE and then a tap of R4 turns it off.
- The pad's rings are drawn by the touch layer: the order wheel with the
  pad's aim dot and a line saying how to give, arm or close it; the build
  ring with the build page's own pictures and counts, PREV and NEXT; and the
  ring of the nine groups at the battlefield's lower left while the groups
  layer is held. A finger can tap any of their wedges too.
- A building the pad started placing follows the pad's pointer until a
  finger lands on the battlefield; from then the ghost is placed by touch.

## Touch settings

The OA settings gain a **Touch** section while the touch controls are on:

| Setting | Choices | Kept as |
|---|---|---|
| One-finger drag | Automatic (a box on a tablet, scrolling on a phone), Box, Scroll | `open-annihilation.touch-drag` |
| Hold delay | 250 to 700 ms, 350 ms by default | `open-annihilation.touch-hold-delay` |
| QUEUE and ADD | Stay on, or One action | `open-annihilation.touch-latches` |
| Haptics | On or off: a tap of the device's haptics as a hold starts, a box starts, a site is refused or a factory's queue goes down | `open-annihilation.touch-haptics` |
| Left-handed layout | Mirrors the touch controls left to right; the tablet's side panel stays | `open-annihilation.touch-left-handed` |
| Control size | Standard, Large or Larger: the touch controls at 1, 1.25 or 1.5 times their size in points, the 3.1c screen kept as it is; a Steam Deck starts at Larger | `open-annihilation.touch-control-size` |

Each takes effect at once.

## Keyboard, mouse, trackpad and Pencil

A keyboard, a mouse or a trackpad connected to an iPad works as on a
computer, every key and button included. Because iPad keyboards often lack
the function keys, Pause and Escape, while the touch controls are on:

| Keys | Stand for |
|---|---|
| Cmd+. | Escape |
| Cmd+1 to Cmd+4 | F1 to F4 |
| Cmd+P | Pause |

The Apple Pencil works as a mouse that hovers: the order cursor, the unit
under the tip and a building's ghost follow it before it touches, on iPads
that report the Pencil's hover, and a touch of its tip is a click.

## Leaving the game

When the game goes to the background (the home screen, another app, a
call) during a game played on this device alone, the in-game menu opens and
holds the game, and the settings are saved; Resume carries on. A
multiplayer game keeps running until the system stops the app.

## Not yet

These parts of the design are not written yet: offering to resume a game
after the system closed the app, the Pencil's squeeze and double tap,
hiding the touch controls while a keyboard or mouse is used, a magnifier
and drag-scrolling of lists in menus, a tap on a message to jump to its
unit, larger controls in the settings dialog on a phone, and the phone
drawer showing several build pages as one grid (it shows one page at a
time). On a phone the in-game menu's Help does not open yet, since its page
is taller than the screen, and the on-screen keyboard covers the lower half
of the save dialog, its OK and Cancel among it, while it is shown.

The touch controls are checked by `--check-touch-controls`, on a tablet
window and on a phone window; see
[development/testing.md](development/testing.md).
