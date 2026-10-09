# Gamepad controls

Open Annihilation can be played with a gamepad: the Steam Deck's built-in
controls, and any gamepad SDL knows, such as an Xbox, PlayStation or
Nintendo Switch Pro controller. On the Steam Deck the right trackpad is a
precise mouse and the triggers are its buttons, so everything the desktop
has keeps working: hover, the order cursor, the status bar's description,
a building's ghost following the pointer, edge scrolling and help text. The
rest of the controller adds what a mouse lacks. Every order a gamepad gives
is one the mouse, the keyboard or the touch controls can give, through the
same paths, so a game played with a gamepad saves, replays and plays over
the network exactly as one played with a mouse.

Steam Deck support is experimental: it has been tried on one Deck so far,
and the feel of the pointer, the haptics and the Steam Input routes below
still need a Deck to be checked (see [What needs a Deck](#what-needs-a-deck)).
The [Steam Deck guide](installation/steam-deck.md) covers installing the
game on a Deck.

## When the gamepad controls are on

The gamepad controls switch on, for the rest of the run, the first time a
gamepad sends input: a button, a stick or trigger moved past its dead zone,
or a trackpad touched. Until then nothing changes: with a mouse and a
keyboard and no gamepad, the game looks and plays exactly as without them,
at every window size, and no key is taken. Once they are on, the mouse, the
keyboard and the touch screen keep working beside the gamepad.

The keys F13, F14, F15 and F16 count as the Steam Deck's back grips (R4,
R5, L4 and L5) only while a gamepad is connected, so a desktop keyboard
with those keys is unchanged without one. A gamepad connected or
disconnected during a run is picked up or let go at once.

## How the controller reaches the game

On a Steam Deck, Steam decides what the game sees. There are three routes:

| Route | What the game gets | What works |
|---|---|---|
| **Steam Input on, with Steam's own template** (Steam's default for a non-Steam game) | A gamepad with sticks, triggers, bumpers, A/B/X/Y, D-pad, View, Menu and stick clicks, and Steam's mouse from the right trackpad | The [fallback map](#the-fallback-map): no grips; the pointer is Steam's mouse; the rings aim by moving the pointer toward a wedge, or with the right stick |
| **Steam Input on, with the "Open Annihilation" template** (the [Steam Input templates](#the-steam-input-templates)) | As above, with the back grips sent as F13 to F16, the right trackpad as the mouse and the left trackpad as the left stick | Everything below but the left-pad minimap, the rings aimed by the right trackpad, the group ring, the gyro and the trackpads' own ticks |
| **Steam Input off for Open Annihilation** | The controller itself, through SDL: both trackpads, the four grips, the stick touches, the gyro and the haptics | Everything below |

The game tells the routes apart by itself: a controller with two trackpads
is the Deck with Steam Input off, whose grips are buttons of its own; behind
Steam Input, the grips count from the first press of F13 to F16. The Controller section of the settings
says when Steam Input is on (see [Controller settings](#controller-settings)).

Every other gamepad plays the [Sticks](#sticks-only) scheme, since it has no
trackpad to point with, with the fallback map's buttons for what the grips
do on a Deck. A DualSense's single touchpad does not make it a trackpads
pad.

## The pad pointer (Steam Deck)

```
          L2 right button            R2 left button
          L1 BUILD ring              R1 ORDER ring
   ┌───────────────────────────────────────────────────────────────┐
   │  [View] unit info / hold: game layer   [Menu] game menu       │
   │                                                               │
   │  (L) left stick: pan                       Y  select type     │
   │      L3: centre                         X     B  clear        │
   │  D-pad ↑ commander  ← SELECT ▾             A  click           │
   │        → next unit  ↓ next report   (R) right stick: ↕ zoom   │
   │                                         ↔ build page, R3 follow│
   │  ┌─────────────┐    touch screen    ┌─────────────┐          │
   │  │ LEFT PAD    │   (touch controls) │ RIGHT PAD   │          │
   │  │ slide: drag │                    │ slide:      │          │
   │  │ the map     │                    │ pointer     │          │
   │  │ hold press: │                    │ press:      │          │
   │  │ minimap     │                    │ left click  │          │
   │  └─────────────┘                    └─────────────┘          │
   └───────────────────────────────────────────────────────────────┘
   back:  L4 ADD   L5 GROUPS                 R4 QUEUE / x5   R5 FORCE
```

### The buttons in a match

| Input | What it does | Mouse and keyboard |
|---|---|---|
| Right trackpad: slide | Moves the pointer (see [The pointer](#the-pointer)) | Mouse |
| Right trackpad: press | A left click at the pointer | Left button |
| **R2** | **The left button**: on at a 30 % pull, off below 20 %. A click selects, or gives the order the cursor shows; held and moved, a selection box, or an area order when Attack, D-gun, Reclaim or Repair is armed; two presses within 300 ms, a double click (every unit of the type) | Left button |
| **L2** | **The right button**, as the 3.1c interface type has it: with the left-click interface it takes back an armed order or a building being placed, else clears the selection; on a build button it takes one off the queue (five with R4); with the right-click interface it gives the order | Right button |
| **R4** | **QUEUE**: hold, or tap to latch. On a build button it is **x5** instead, decided as it is pressed | Shift on orders and build buttons |
| **L4** | **ADD**: hold, or tap to latch | Shift on selecting |
| **R5** | **FORCE** while held: a click on a unit or the ground forces fire. With a face button or the D-pad, the standing-orders layer | Ctrl |
| **L5** | The groups layer while held | Alt and Ctrl with a number |
| **R1** | The order ring at the pointer | Order buttons |
| **L1** | The build ring for the selected builder; with no builder, the standing-orders ring | Build page, PREV and NEXT |
| Left stick | Scrolls the map; faster with the square of the push, and with the zoom | Arrow keys |
| L3 | Centres on the selection, or on the commander with none | Home |
| Left trackpad: slide | Drags the map: the ground follows the thumb, and glides on after a flick | Two-finger drag |
| Left trackpad: press and hold | **The minimap under the thumb**: the trackpad stands for the whole map, the camera goes to the point under the thumb and follows it while the press is held | A right drag on the minimap (a left drag with the right-click interface) |
| Right stick ↑ and ↓ | Zooms in and out about the pointer, smoothly (the Mouse wheel zoom, Maximum zoom out and Maximum zoom in settings are honoured) | Wheel |
| Right stick ← and → (a flick) | The previous or next build page while a builder is selected | `,` and `.` |
| R3 | Follows the selected unit; with R4 held, the next of the selection | T, Shift+T |
| **A** | A left click at the pointer | Left button |
| **B** | **Clear**: takes back an armed order or a building being placed, else clears the selection | The CLEAR control |
| **X** | Stop | S |
| **Y** | Every unit of the type under the pointer, else of the selected unit's type | Double click, Ctrl+Z |
| D-pad ↑ | Selects the commander and follows it; with ADD, adds it | Ctrl+C |
| D-pad → | The next unit of your own | N |
| D-pad ↓ | The next unit that reported | F3 |
| D-pad ← | **SELECT ▾**: All, Builders, Factories, Aircraft, On screen, Commander, Same type, Centre, Follow, Next unit and Next report; the D-pad moves through it and A picks, or point and click | The keys each item names |
| View: tap | Unit info for the unit under the pointer; with R4 held, pinned | F1, Shift+F1 |
| View: hold | The game layer while held | |
| Menu | The in-game menu | F2 |
| Touch screen | The touch controls, unchanged ([touch-controls.md](touch-controls.md)) | |
| Gyro | Off unless the Gyro pointer setting turns it on | |

### The grips

The grips press the touch controls' own **QUEUE**, **ADD** and **x5**
latches, with the touch controls' rules and settings
([QUEUE, ADD and x5](touch-controls.md#queue-add-and-x5)):

- a press is on at once;
- released before the hold delay (350 ms by default) with no action done
  meanwhile, it latches on, or off when it was on;
- held longer, or used by an action while held, it was only held;
- with the Touch setting **QUEUE and ADD** at One action, a latch turns off
  after the action that used it.

There is one set of latches: tapping QUEUE on the screen and then pressing
R4 turns it off, as two taps would.

| Grip | Applies to |
|---|---|
| R4 QUEUE | Orders, area orders and ring picks join the queue; placing buildings stays armed until R4 goes off; the queued orders are drawn |
| R4 on a build button, x5 | A factory's +5 and −5 |
| L4 ADD | Clicks on units add them or take them out; boxes add; group recall adds |
| R5 FORCE | Clicks while held force fire. It is not a latch |

### Held layers

**Groups (hold L5).** The D-pad and the face buttons, clockwise from the
top, are groups 1 to 8; while L5 is held the group chips show their
buttons, and the left trackpad is a ring of all nine groups.

| With L5 held | Tap | Tap again | Hold | With L4 (ADD) |
|---|---|---|---|---|
| D-pad ↑ → ↓ ← | Selects group 1, 2, 3, 4 | Centres on it | Stores the selection in it | Adds the group |
| Y B A X | Groups 5, 6, 7, 8 | Centres | Stores | Adds |
| Left trackpad, a ring of 9 | Touch a wedge, press to select | Press again: centres | Press and hold: stores | Adds |

These are the touch group chips' rules, and give the same keys: Alt and
the number selects, Ctrl and the number stores, Shift, Alt and the number
adds.

**Game (hold View).**

| With View held | Does | Key |
|---|---|---|
| A | The chat line (multiplayer); Steam's keyboard opens | Enter |
| B | The kill board | F4 |
| X | Pauses and resumes | Pause |
| Y | Health bars | `` ` ``, the key below Esc on any layout |
| D-pad ↑ and ↓ | Faster and slower (not for a watcher) | `+` and `−` |
| D-pad ← | Clears the messages | F12 |
| D-pad → | The team menu (multiplayer), the next primary unit (single player) | Tab |
| L1 | The share panel (multiplayer), the previous primary unit (single player) | h, Shift+Tab |
| R2 | A screenshot | Ctrl+F9 |

**Standing orders (hold R5 with a face button or the D-pad).** R5 with R2
or A stays a forced click.

| With R5 held | Does | Key |
|---|---|---|
| Y | Fire orders (cycles) | f |
| B | Move orders (cycles) | v |
| D-pad ↑ | Cloak | k |
| D-pad ↓ | On and off | x |
| X held for a second | Self-destruct (starts or stops the countdown); a ring fills at the pointer | Ctrl+D |
| L2 with the right trackpad | Mouse look (Ctrl and a right drag) | |

A layer takes effect the moment its button goes down; a button pressed
with View held uses the game layer, and View's release is then no tap.

### The rings

**The order ring (R1)** is the touch controls' order wheel
([The order wheel](touch-controls.md#the-order-wheel)), opened at the
pointer: the same twelve places clockwise from the top, Move, Patrol,
Attack, D-gun (Load), Capture, Stop, Info, Type, Reclaim, Repair, Unload and
Guard. Orders the selection cannot take are greyed and keep their places;
the order a click would give is ringed.

- **Aiming.** The right thumb's place on the trackpad, measured from its
  centre, picks the wedge: rest the thumb toward it, with no need to move
  far. The pointer stays where the ring opened, a dot shows the aim and
  each change of wedge gives a tick. Within 15 % of the centre nothing is
  aimed. The right stick aims too, past half its push.

| While R1 is held | Does |
|---|---|
| Release R1 on a wedge | Gives that order at the ring's point, as arming its button and clicking there does; with R4 held, queued, and the order stays armed |
| A on a wedge | **Arms** the order and closes the ring; the next R2 click gives it, and an R2 drag gives an area order |
| Release R1 at the centre, or B | Closes the ring; nothing is given |
| Tap R1 (released before the hold delay, nothing aimed) | The ring stays open: R2 or a trackpad press gives at its point, A arms, B closes |

A finger can still tap a wedge of a ring the gamepad opened, and the
gamepad can aim a ring a finger opened. The ring shows its hint: "release
R1: give · A: arm · B: close".

**The build ring (L1)** holds eight wedges: the builder's current build
page (six buttons) at the top, top right, bottom right, bottom, bottom left
and top left, with **PREV** at the left and **NEXT** at the right. Each
wedge shows the page's own 3.1c build picture, with the queue count a
factory shows. A press on a wedge is a press on that build button.

| Selection | While L1 is held | On release |
|---|---|---|
| A builder that moves | Aim a building | On a building, its ghost follows the pointer to be placed; on PREV or NEXT the page turns and the ring stays; at the centre it closes |
| A factory | R2 or a trackpad press on the aimed wedge adds one (five with R4); L2 takes one off (five with R4); the counts update | Closes |
| No builder | **The standing-orders ring**: fire orders, move orders, on and off, cloak, INFO and SELF-DESTRUCT (a one-second hold) | Release on a toggle changes it |

### Selecting, orders and building

- **Click**: R2, A or a trackpad press. When nothing is under the pointer,
  the nearest unit within 12 points is taken.
- **Box**: hold R2 and slide the right thumb. The thumb may lift and land
  again while R2 stays down, and the box's corners stay on the ground, so
  scrolling with the left stick meanwhile stretches the box across the
  map. A short haptic marks the start of a box.
- **Every unit of a type**: Y, two R2 presses, or the ring's Type.
- **Orders**: click where the cursor shows the order; arm one with the
  order ring and A, or with the side panel's button and the pointer; give
  one at a point in one move by releasing R1 on it. Take back an armed
  order with B or L2. Orders on the minimap: point and click, as with a
  mouse.
- **Buildings**: choose one on the build ring or the side panel. Its ghost
  follows the pointer, green or red for a site it can or cannot stand on;
  R2, A or a trackpad press places it. On a refused site the game sounds
  its refusal, a thump plays on the right trackpad and the ghost stays.
  With R4 held or latched, placing stays armed; once a building is placed,
  letting R4 go or turning its latch off ends it, as letting Shift go does.
  B or L2 cancels.
- **Placement a finger started** follows the touch controls' rules; one the
  gamepad started follows the pointer until a finger lands on the
  battlefield, and from then on follows the touch rules.
- **Factories**: point at a build button. R2 adds one, R4 and R2 five, L2
  takes one off, R4 and L2 five; taking one off gives the queue-reduced
  haptic.

### The pointer

| Property | Behaviour (a setting where named) |
|---|---|
| Speed | One trackpad width at slow speed moves the pointer half the screen. **Pointer speed**, 50 to 300 % |
| Acceleration | Unchanged below half a trackpad width a second, rising to two and a half times at three widths a second, so a flick crosses the screen and slow movement stays precise. **Pointer acceleration**: Off, Low, High |
| Landing | For the first 20 ms after the thumb lands, small movements are ignored, so landing does not nudge the pointer |
| Click lock | The pointer holds still for 40 ms around a trackpad press and release, so the press does not drag the click off its target. R2 has no such problem |
| Glide | **Trackpad glide** (off by default): after a flick the pointer coasts and slows, and stops when the thumb lands |
| Ticks | A light tick every 32 pixels of travel, and a firmer one when what is under the pointer changes (ground to a unit, or the cursor between select and an order) |
| Edges | The pointer rests like a mouse, so at the screen's edge it scrolls the map by the desktop's rule |
| Absolute | **Right trackpad**: Pointer (absolute) maps the trackpad onto the battlefield: quicker to jump with, less precise. Not the default |
| Gyro | **Gyro pointer**: Off, While the right pad is touched, While the right stick is touched, or Always; **Gyro speed**. Held to a touch, as Steam's "gyro on touch" is, so it does not drift |

When a finger rests on the battlefield and the right trackpad then moves,
the pointer takes the hover back. A finger on the minimap and the pointer
both act at once.

### The camera

| Action | Gamepad |
|---|---|
| Scroll | Left stick; left trackpad drag, with glide; the pointer at the screen's edge |
| Jump to a point of the map | Left trackpad pressed and held; or L2 on the minimap with no order armed, and with the right-click interface R2 there too. L2 with the left-click interface, and R2 with the right-click interface, then move the view with the pointer until they are let go. With the left-click interface R2 and A on the minimap never move the view: they select, or give the order the cursor shows there, as a left click does |
| Zoom | Right stick ↑ and ↓, about the pointer |
| Centre, follow, next unit, next report | L3, R3, D-pad →, D-pad ↓ |
| Mouse look | R5 and L2 held, sliding the right trackpad |

## Sticks only

The **Scheme** setting's other choice, and the scheme every gamepad without
two trackpads plays. It has the same layers, rings, latches, HUD and menus;
only the pointer and the camera change.

| Input | Does |
|---|---|
| **Right stick** | **The pointer**, a stick cursor. At the battlefield's edge it pushes the map |
| R3 held and a right-stick flick | Jumps the pointer to the nearest unit on screen in that direction, with a tick as it lands |
| Left stick | Scrolls; L3 centres |
| D-pad ↑ and ↓ | Zoom in and out about the pointer, for as long as they are held |
| D-pad ← | SELECT ▾ (Commander, Next unit, Centre, Follow and the selections) |
| D-pad → | The next unit that reported |
| R1 and L1 rings | Aimed with the right stick; the pointer holds still while a ring is open |
| Trackpads and gyro | Still work when the gamepad has them, but nothing needs them |

The stick cursor ignores the stick's first 12 % and treats the last 5 % as
a full push. Its speed is 1200 points a second times the push to the power
2.2; after 0.4 s at more than 90 % it rises to 1.8 times over 0.3 s. Within
24 points of a unit on screen it slows to 45 %. With **Magnetism** on, when
the stick comes back to rest within 16 points of a unit, the pointer eases
onto it over 80 ms, unless another unit lies within 8 points of it; not
while R2 is held, while a building is being placed, or over the side panel,
where the stick moves the pointer from button to button instead.

## The fallback map

Without grips (a gamepad with no grip buttons, and no F13 to F16 pressed
yet: Steam's own templates, and every gamepad but the Deck), the grips' jobs
move:

| Deck role | Fallback |
|---|---|
| R4 QUEUE and L4 ADD | **Tap R1** latches QUEUE on or off, **tap L1** latches ADD; holding R1 or L1 opens its ring. The on-screen chips work too |
| R5 FORCE | Arm Attack from the ring and click: attacking the ground is Attack's own behaviour |
| L5 groups | **Hold View**: the D-pad and A/B/X/Y are groups 1 to 8; a tap of View is still unit info |
| View's game layer | **Hold Menu**; a tap of Menu still opens the in-game menu |
| The left trackpad | The left stick only |
| Ring aim by the right trackpad | Move the pointer toward the wedge, or push the right stick |

Once one of F13 to F16 is pressed, the gamepad plays the full map for the
rest of the run. F13 stands for R4, F14 for R5, F15 for L4
and F16 for L5, which is what the "Open Annihilation" template sends.

## Menus

Every screen outside the match: the 640×480 menus, the in-game menu, the
settings dialog, the Game files screen and the folder chooser.

| Input | Does |
|---|---|
| Right trackpad (or the stick cursor), R2, L2 | The pointer, a left click and a right click |
| D-pad, left stick | Move the focus between buttons and list rows; held, they repeat after 400 ms, then every 120 ms |
| L1 and R1 | The previous and next group of buttons (Shift+Tab and Tab) |
| A | Presses the focused button. With none focused, as when the main menu opens, the 640×480 menus focus their first button and the other screens press their default |
| B | Back (Escape), as each screen has it |
| Menu | Enter: the screen's default button; on the 640×480 menus, as A |
| Right stick ↑ and ↓ | The wheel: steps the map and mission lists |
| A twice quickly on a list row | Starts the campaign, mission or map, as a double click does |

**Text.** A chat line, a save's name, the player's name or a marker's text
opens Steam's on-screen keyboard in Game Mode, placed clear of the field
being typed in; Enter on it sends, and B cancels the line. STEAM and X open
the keyboard by hand. The touch screen's keyboard and a USB keyboard work
too.

## The pad HUD and button prompts

**Before a finger touches the screen**, a gamepad in use adds a slim HUD
over the battlefield, beside the 3.1c screen:

- a **status pill** saying what R2 and L2 would do now, from the order
  cursor: "R2 MOVE · ENEMY: ATTACK · L2 CANCEL" (with the right-click
  interface, "R2 SELECT · L2 MOVE");
- **QUEUE R4**, **ADD L4** and **FORCE R5** chips, lit while held or
  latched; over a build button QUEUE's chip reads x5;
- the **group chips** with their sizes, each showing its button while L5
  is held, and the group ring while L5 is held on a Deck;
- the rings' aim dot and hint, and the build ring's pictures.

**Once a finger lands**, the full touch controls appear as they always do
([touch-controls.md](touch-controls.md)), and each control gains a small
badge with its gamepad input when a gamepad has been used in this run:
QUEUE R4, ADD L4, CLEAR B, SELECT ▾ D-pad ←, PAUSE View+X, CHAT View+A,
CENTRE L3, FOLLOW R3, NEXT D-pad → and INFO View. The FORCE chip joins the
thumb column. Badges change how controls look, never where they are. The
long-press help on a touch control names the gamepad input too, for
example "QUEUE: hold or tap R4".

**Button prompts** are drawn by the game itself, never Valve's or a console
maker's art: rounded squares with letters for A, B, X and Y, pills for the
bumpers, triggers and grips, outlines for the trackpads and sticks, and
marks for View and Menu. The **Button prompts** setting picks the set:
Automatic, Steam Deck, Xbox, PlayStation, Nintendo or Off. Automatic picks
the Steam Deck's for Valve's Deck controller (which Steam's own gamepad
also reports), and otherwise follows the gamepad's type: PlayStation's
symbols, Nintendo's letters in Nintendo's places, and Xbox's for any other
gamepad.

## Controller settings

The OA settings gain a **Controller** section once a gamepad has sent input
in this run, after the Touch section. Each setting takes effect at once.

| Setting | Choices | Kept as |
|---|---|---|
| Scheme | Trackpads (the default), Sticks | `open-annihilation.pad-scheme` |
| Right trackpad | Pointer (relative), Pointer (absolute) | `open-annihilation.pad-right-trackpad` |
| Pointer speed | 50 to 300 %, 100 by default | `open-annihilation.pad-pointer-speed` |
| Pointer acceleration | Off, Low (the default), High | `open-annihilation.pad-acceleration` |
| Trackpad glide | Off (the default), On | `open-annihilation.pad-glide` |
| Right stick | Zoom and build pages (the default), Pointer, Nothing | `open-annihilation.pad-right-stick` |
| Magnetism (stick pointer) | On (the default), Off | `open-annihilation.pad-magnetism` |
| Gyro pointer | Off (the default), While the right pad is touched, While the right stick is touched, Always | `open-annihilation.pad-gyro` |
| Gyro speed | 50 to 400 %, 100 by default | `open-annihilation.pad-gyro-speed` |
| Haptics | Off, Light (the default), Strong | `open-annihilation.pad-haptics` |
| Button prompts | Automatic (the default), Steam Deck, Xbox, PlayStation, Nintendo, Off | `open-annihilation.pad-prompts` |
| Left-handed | Mirrors the roles: the left trackpad points, the right one moves the camera, and the sticks, triggers, bumpers, grips and stick clicks swap sides; the face buttons, D-pad, View and Menu stay | `open-annihilation.pad-left-handed` |
| Control size | Standard, Large, Larger: the touch controls' size, shared with the Touch section | `open-annihilation.touch-control-size` |
| Hold delay, QUEUE and ADD | Shared with the Touch section | `open-annihilation.touch-hold-delay`, `open-annihilation.touch-latches` |

When the controller comes through Steam Input, the section says so: "Steam
Input is on: the trackpads and back grips reach the game as Steam's mouse
and keys. Turn Steam Input off for Open Annihilation in Steam's controller
settings to use them here."

On a Steam Deck the game starts with **Control size** at Larger, so the
touch controls come out near their size on an iPad, and with **Maximum
frame rate** at the screen's rate (60 on the LCD model, 90 on the OLED).
A stored choice always wins, and **Restore defaults** brings the Deck's
starting values back.

## Haptics

With **Haptics** at Light or Strong, the gamepad gives the touch controls'
haptics (a hold starting, a box starting, a refused site, a factory's queue
going down) and three of its own: the pointer's tick, the firmer tick when
what is under the pointer changes, and a tick for each change of wedge in a
ring. A refused building site thumps on the right side.

On the Steam Deck with Steam Input off, the game plays each one as a short
pulse on one trackpad's own actuator, through a small change to SDL's
Steam Deck driver that the packages carry (the SDL patches in
`tools/sdl-patches`, offered upstream). Where that is not there, a system
SDL for one, and on every other gamepad, the same feels play as short
rumbles on the left or right side. Off plays nothing.

## The Steam Input templates

The Steam Deck package's `steam-deck` folder holds two layouts in Steam's
controller format. Copied into Steam's `controller_base/templates` folder,
they are listed under Templates once Steam restarts:

- **Open Annihilation**: the sticks, triggers, bumpers, face buttons,
  D-pad, View, Menu and stick clicks reach the game as the gamepad's own;
  the right trackpad is the mouse, its press the left button, with
  Steam's haptics; the left trackpad is the left stick; R4 sends F13, R5
  F14, L4 F15 and L5 F16. With it the game gets the grips through Steam
  Input (the second route above).
- **Open Annihilation: keyboard and mouse**: mouse and keys only, for
  players who want Steam's mouse and keys, and for builds without gamepad
  controls. The right trackpad is the mouse; R2 and L2 the left and right
  buttons; the left trackpad the wheel (zoom), its press Space; the left
  stick the arrow keys; the right stick a slow mouse; R4 Shift and L4 a
  latched Shift; R5 Ctrl; L5 makes the D-pad and A/B/X/Y send Alt and 1 to
  8 (Ctrl and 1 to 8 held); R1 a radial menu of the order keys on the right
  trackpad and L1 one of the selection keys on the left; A, B, X and Y
  Enter, Escape, S and Ctrl+Z; the D-pad F3, N, `,` and `.`; View F1 (F4
  held) and Menu F2.

Neither has been tried on a Deck yet. The
[Steam Deck guide](installation/steam-deck.md) says how to install and
choose them and how the community layout is published.

## How it is checked

`--check-pad-controls` plays a skirmish headless with virtual gamepads: a
Steam Deck with both trackpads, the grips, a gyro and haptics, and an Xbox
controller. It drives each row of the tables above, compares what the
gamepad gives with what the mouse and keys give from the same state
(selection, orders, the queue, the latches and the camera), checks the pad
HUD's layout, the haptics sent, the fallback map and F13 to F16, and that
nothing changes before a gamepad is used. With touch controls on, as on an
iPhone or an iPad, it checks the badges on the touch controls instead of the
slim HUD, and on a phone it presses the build ring's wedges, since a phone
shows no side panel. The `native-pad-controls` tests run it; the pure
model's tables and timings have their own unit tests (`ui-pad-controls`).
See [development/testing.md](development/testing.md).

## What needs a Deck

These are checked by hand on a real Steam Deck before the README's badge
changes from "experimental":

- the Steam Deck package starting in Game Mode at 1280×800, with sound, and
  the templates, copied in by hand, in Steam;
- the pad pointer's speed, acceleration and ticks, the left-pad minimap and
  the trackpads' own pulses;
- Steam Input off: SDL's own Deck driver, and the Steam and ··· buttons,
  which players report behaving oddly with Steam Input off;
- whether Steam offers F13 to F16 to the template and passes them on;
- whether Steam opens its keyboard for a non-Steam shortcut's text fields;
- native touch with Steam's touch setting for the game;
- the frame rate and power at 60 and 90, and suspending and resuming in a
  single-player match.

On a Mac, the check is that starting the gamepad support shows no request
for input monitoring.
