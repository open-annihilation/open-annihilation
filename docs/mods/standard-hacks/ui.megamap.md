# Megamap

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `ui.megamap` |
| Area | Interface (`ui`) |
| Scope | view: local display only; each player may differ |
| Runs on | Each machine, for its own view. |
| Parameters | 5 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

Adds a full-screen strategic map: Tab or the mouse wheel replaces the battlefield with the whole map, drawn with its indestructible features, unit category icons and sensor rings, and the player can select units and give orders on it. It can also redraw the minimap at a finer quality. 3.1c has only the minimap. The hack acts only while the player's Mouse wheel zoom setting is off: with the setting on, as it is by default, the wheel's zoom takes the megamap's place.

![Megamap open on Painted Desert, showing the whole map: blue Arm and red Core unit icons meeting on the central plateau, small dark pictures of the map's indestructible features scattered over the sand, and a faint green radar ring around the selected radar tower.](images/ui.megamap-battle.png)

*The megamap open in the middle of a battle on Painted Desert: Arm (blue) and Core (red) unit icons meeting on the central plateau, the selected radar tower's radar ring, and the map's indestructible features drawn as small pictures of themselves.*

![Close-up of the megamap: a yellow drag box around a cluster of blue Arm icons, each with a white selection frame; red Core icons below it have no frame.](images/ui.megamap-box-select.png)

*Box-selecting an army on the megamap: the drag box drawn over the Arm army's icons, each unit inside marked selected with its white frame, the Core army below left unselected.*

![Two enlarged copies of the same corner of the Fox Holes minimap side by side: on the left, crater shadows with stray reddish pixels; on the right, the same craters with clean dark shading on slightly lighter sand.](images/ui.megamap-minimap.png)

*Close-up of the minimap on Fox Holes, enlarged 3x: left, 3.1c halves the map file's minimap through its palette pair-mix table and leaves stray reddish pixels along the craters' dark edges; right, ui.megamap's enhanced minimap averages each 2x2 block in full colour before picking the nearest palette colour, so the crater shading stays clean and the sand keeps its lighter tone.*

## Configuration example

```yaml
hacks:
  ui.megamap: true
```

```yaml
hacks:
  ui.megamap:
    enhanced-minimap: false          # keep the game's own minimap picture
    ring-minimums: [0, 0, 0, 0, 0]   # draw every sensor ring, however small
```

## Details

#### Opening and closing

Tab opens and closes the megamap, playing the interface's open and close
sounds. Rolling the wheel toward the player opens it; rolling it away closes
it and centres the battlefield view on the map point under the pointer.

#### With Mouse wheel zoom on

The settings' Mouse wheel zoom (Controls, on by default) zooms the
battlefield with the wheel, out to the whole map with Maximum zoom out at
Whole map, which makes the megamap redundant. While it is on, the hack is
off for this player, as if the profile left it out: Tab and
the wheel do what they do without it, the wheel zooming the battlefield, no
megamap opens and the minimap keeps the game's own picture. Turning the
setting on during a match closes an open megamap, without its sound, and
puts the game's minimap picture back; turning it off brings the megamap and
the enhanced minimap back.

#### What it shows

The megamap fills the battlefield's rectangle with the whole map, scaled to
fit with its proportions kept and dark grey bars on the long sides. Each
pixel of the picture takes the mean colour of the map pixels it covers, then
the colour that looks nearest among those the map's tiles use (the least
distance in the OKLab colour space) or, with `dither`, a checkerboard of the
two nearest. The picture is shaded by what the viewer has mapped and sees,
as the minimap is.

With `feature-blobs`, the map's indestructible features that cannot be
reclaimed, such as rocks, vents and metal patches made of features, are
drawn on the picture as the map placed them. Each is the first frame of its
standing picture, shrunk in full colour to the map's scale and at least 2
pixels across; each of its pixels is mixed with the terrain under it in
proportion to how much of it is opaque, then given the nearest palette
colour. It stands on its
footprint's centre, raised by half the ground's height. A feature without a
picture, such as a 3DO one, takes a mark 3 pixels square: one colour for
features worth metal, a second for a feature described as a spire and a
third for the rest. Features that can be reclaimed or destroyed, such as
trees and wrecks, are not drawn.

Over the picture it draws:

- an icon for each unit the minimap shows, by category;
- the sensor and anti-nuke rings of the selected units;
- a drag box while one is being drawn.

It draws no rectangle for the main view; the minimap shows where the view
is.

#### Icons

The icons come from the game folder's `Icon/iconcfg.ini`:

- `[Option]` holds `FillColor` (replaced by the owner's colour),
  `TransparentColor` (not drawn), `SelectedColor` (drawn only while the
  unit is selected), `HoverColor` (the selected colour's stand-in under the
  pointer), `UseCircleHover` (a ring marks the hovered unit instead) and
  `UseDefaultIcon` (use the built-in icon set rather than the file's lines).
- `[Icon]` holds one line per unit category word and its picture file,
  plus `Unknow`, `Nothing` and `NukeIcon`. Only the first `icon-categories`
  category lines are kept.

A unit out of the viewer's sight takes the `Nothing` icon. A seen unit takes
the first category line whose category holds its type, or the `Unknow` icon
when none does. An icon is at most 22 by 22 pixels; a missing picture draws
a square of the owner's colour. Without the file, the built-in set is used.

#### Rings

For each selected unit the megamap draws up to five rings, in this order:
radar, sonar, radar jammer, sonar jammer and interceptor coverage.
`ring-minimums` gives each kind's least radius in pixels, in the same order;
a ring is drawn when its range is above 0 and reaches its minimum.

#### Clicks

Clicks on the megamap act as clicks on the battlefield do in the player's
interface type (the Interface option: left-click or right-click), at the
map point they stand for and on the unit whose icon is under the pointer.
The orders they give are ordinary orders, queued while Shift is held:

- left-click interface: a left click gives the selection the order the
  battlefield's cursor would there, the armed command or the default order
  (a move on open ground, an attack on an enemy); one on a unit of the
  viewer's otherwise selects it in place of the selection, or with Shift
  flips it in or out of it. A right press drops the selection, with Ctrl
  held too, since the megamap has no view to look round, or the armed
  command;
- right-click interface: a left click on a unit of the viewer's selects
  it, or with Shift flips it, and elsewhere drops the selection, but gives
  an armed command. A right press gives the default order there (a move on
  open ground, an attack on an enemy, a guard on a unit of the viewer's),
  or drops the armed command.

In both interface types:

- left drag (more than 3 pixels): box-select the viewer's selectable units,
  added to the selection while Shift is held;
- left double-click on a unit of the viewer's: select every unit of its type.

A click that drops the selection closes the unit info panel too, as on the
battlefield, and so does a box that takes in none of the viewer's units.

With the touch controls, a finger on the megamap acts as the mouse there,
on the icon under the finger and at the map point under it, never on the
battlefield hidden under the megamap. A tap is a left click; in the
right-click interface it is the right press an order needs, except on a
unit of the viewer's or with a command armed. A double tap on a unit of the
viewer's selects every unit of its type. A drag draws a box. A hold opens
the order wheel for the icon and the ground under the finger, and the order
picked is given there. A tap or a hold with a building armed places it
where the finger is, and a double tap places it once.

A panel or dialog over the megamap takes its own clicks and taps, as it does
over the battlefield, among them the exit menu and its questions, the unit
info panel and the save page. While the in-game menu or another panel holds
the match, a left click on the megamap does nothing and a right press beside
the menu acts as on the battlefield; in the left-click interface it drops the
selection and closes the menu, which lets the match run again. A tap acts as
the click or right press it stands for, and a hold opens no order wheel.

#### Enhanced minimap

With `enhanced-minimap`, the minimap's picture is redrawn at the minimap's
own size, from the map file's minimap where it has one, else from the map's
terrain: each pixel the mean colour of the pixels it covers, then the
nearest colour of the whole palette by red, green and blue; `dither`
applies to it too.

3.1c has no megamap and draws the minimap from the map file's picture as it
is. Turning the hack on with every parameter at its baseline gives a
megamap without icons, features or the redrawn minimap, and only
interceptor rings of 512 pixels or more.

### In a network game

This is a view rule: it changes only what this machine shows and how its own input works, never what another machine is told. It is not part of the match hash, so players in one game may have it on, off or set differently without breaking the game.

Orders given on the megamap travel as any other order does. Each player's
own Mouse wheel zoom setting decides whether the hack acts for them; the
match hash and every machine's simulation stay as they are.

### Related hacks

- [ui.camera-sharing](ui.camera-sharing.md) and
  [ui.allied-unit-display](ui.allied-unit-display.md) change what the
  minimap shows, and so which units the megamap draws.
- [ui.options-dialog](ui.options-dialog.md) keeps the megamap key with the
  player's settings; [console.key-remaps](console.key-remaps.md) moves keys
  that would clash with it.
- [ui.map-features-ignore-los](ui.map-features-ignore-los.md) decides
  whether map features are drawn out of sight on the battlefield.

### Implementation notes

- The engine reads `UiRules::megamap` (fields `enabled`,
  `enhanced_minimap`, `dither`, `feature_blobs`, `ring_minimums`,
  `icon_categories`).
- `src/app/runtime_megamap.cpp` opens, draws and clicks the megamap and
  redraws the enhanced minimap (`enhance_radar_picture`); its clicks go
  through the battlefield's cursor and order resolution
  (`megamap_click`, `megamap_right_press`, `pick_map_cursor`);
  `src/app/include/oa/app/megamap_state.hpp` holds what a match keeps for it.
- `src/ui/hud/include/oa/ui/hud/megamap.hpp` and `src/ui/hud/src/megamap.cpp`
  hold the layout and its two-way mapping, the terrain downscale and its
  choice of colours by how they look, the features drawn, their pictures
  shrunk, placed and laid over the terrain, the marks of those without a
  picture, the icon file reader, the icon choice and recolouring, and the
  rings.
- `Runtime::megamap_on` holds the hack off while the Mouse wheel zoom
  setting is on; `megamap_wheel_zoom_changed` closes the megamap and puts
  the minimap's picture back when the setting changes in a match.
- Test: `ui-hud-megamap` (`src/ui/hud/tests/megamap_test.cpp`) covers the
  layout, the terrain downscale by red, green and blue and among the map's
  own colours as they look, the features drawn and the marks' colours, a
  picture shrunk, placed and laid over the terrain, the icon file, the
  icon choice and the ring minimums.
- Test: `native-megamap-clicks` (`src/app/runtime_megamap_check.cpp`)
  first checks what the open megamap draws on the 3.1c skirmish's map: the
  terrain keeps to the colours of the map's tiles; only the indestructible
  features that cannot be reclaimed change the picture, each by its picture
  rather than a flat mark, while the reclaimable and destructible ones leave
  it as it is; moving the main view leaves the megamap as it was, with no
  rectangle for the view; and the bars beside the map are the dark grey.
  Then it makes the same clicks on the battlefield and on the megamap in both
  interface types and checks that each leaves the same selection, armed
  command and orders: moves, an attack, a guard, selecting, deselecting,
  and an armed command given and dropped. With `--snapshot` it writes each
  surface's frame just before and just after each of those clicks beside the
  snapshot. Over the open megamap it then presses the exit menu's CANCEL,
  EXITGAME and MAINMENU and their questions' CHOICE2, by mouse and by the
  touch controls' taps in both interface types, and checks that each acts;
  that the save page and the unit info panel take their own presses; that a
  left click beside the in-game menu does nothing; and that a right press
  beside it leaves what the battlefield's does. It checks that a right press
  and a left click on open ground that drop the selection, and an empty box,
  close the unit info panel. Last it turns the touch controls on and, with
  fingers through their dispatcher, checks taps on open ground, an enemy and
  an own unit in both interface types, a double tap, MOVE from the order
  wheel, a box, a building placed by a tap and by a double tap, a tap and a
  hold beside the in-game menu, and an empty box closing the unit info
  panel, each acting on the megamap as the mouse does there.
- Test: `native-render-tiers-visual-rules`
  (`src/app/runtime_visual_rules_check.cpp`) checks that with Mouse wheel
  zoom on, Tab and the wheel open no megamap; that with it off, Tab opens
  it; and that turning the setting on closes it; then that the open
  megamap is presented as the processor composes it.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `ui.megamap`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `ui.megamap: true` | On, every parameter at its default. |
| `ui.megamap: {enhanced-minimap: true}` | On, the parameters named set and the rest at their defaults. |
| `ui.megamap: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `enhanced-minimap` | `bool` | - | `true`, `false` | - | `false` | `true` | `fixed` | `view` | - |
| `dither` | `bool` | - | `true`, `false` | - | `false` | `false` | `fixed` | `view` | - |
| `feature-blobs` | `bool` | - | `true`, `false` | - | `false` | `true` | `fixed` | `view` | - |
| `ring-minimums` | `list<int>` | pixels | `0` to `65535` | 5 | `[0, 0, 0, 0, 512]` | `[128, 128, 64, 64, 512]` | `fixed` | `view` | - |
| `icon-categories` | `int` | categories | `0` to `128` | - | `0` | `9` | `fixed` | `view` | - |

Adjustable: `fixed`: only the profile sets it.

### Presets

This hack has no presets.

### Shorthand

This hack takes no shorthand: write `true`, `false` or a parameter map.

### Every parameter at its default

```yaml
hacks:
  ui.megamap:
    enhanced-minimap: true
    dither: false
    feature-blobs: true
    ring-minimums: [128, 128, 64, 64, 512]
    icon-categories: 9
```
<!-- END GENERATED: schema -->

## Keys

<!-- BEGIN GENERATED: keys -->
| Action | Default chords | What it does |
| --- | --- | --- |
| `megamap.toggle` | `Tab` | The megamap on or off |
| `megamap.open-wheel` | `WheelDown` | Opens the megamap |
| `megamap.close-wheel` | `WheelUp` | Closes the megamap and centres on the pointer |
| `megamap.jump` | `—` | Jumps the view to a point of the megamap |
<!-- END GENERATED: keys -->

These keys move in the [key editor](../../keys.md#the-key-editor), in **Megamap**, while this hack is on.
