# The OAKEYS standard

A key layout is one `.oakeys` file: a zip, like every other package, with a
strict-YAML manifest `oakeys.yaml` at its top. It says which keys, mouse
buttons and wheel turns start which of the game's actions, which input
helpers it wants on, and nothing else. It holds no code, no pictures but its
badge, and nothing that reaches the simulation, so a layout can never change
how a game plays out, only how the player asks for things.

This is the standard this build installs. The action ids are the
[action registry](../../src/data/input-actions/registry/actions.yaml).
`docs/registries/packages.md` names `.oakeys` as the seventh kind, beside
`.oamut` and `.oabrain`.

## Contents

- [1. Goals](#1-goals)
- [2. The package](#2-the-package)
- [3. The manifest](#3-the-manifest)
- [4. Chords](#4-chords)
- [5. Bindings](#5-bindings)
- [6. Pointer schemes and helpers](#6-pointer-schemes-and-helpers)
- [7. Resolution](#7-resolution)
- [8. Problems](#8-problems)
- [9. The text form](#9-the-text-form)
- [10. A full example](#10-a-full-example)

## 1. Goals

| Goal | What it means |
| --- | --- |
| Every action, once | A layout names actions by their stable ids from the action registry, never by the key that does them today. |
| Small diffs | A layout names a base and lists only what it changes, so a fix to the base reaches every layout made from it. |
| Any keyboard | A binding says whether it means the letter printed on the key or the key's place, so an AZERTY or Dvorak player gets the same meaning without a second layout. |
| Nothing hidden | Every binding is one chord to one action. No macros, no timers, no orders repeated by the game. |
| Safe to share | No code, no paths, nothing the game runs. A layout from anywhere is read with the same strict reader as a mod profile and either loads whole or shows each problem. |
| Forward and back | An action this build does not know is kept and shown as "not in this version", never dropped, so a layout made on a newer OA survives an older one and back. |

## 2. The package

The archive is the zip every package kind uses. The names it may hold and
its size limits are the engine's [packages](../registries/packages.md).

| Path | What it is |
| --- | --- |
| `oakeys.yaml` | The manifest and the bindings. Required. At most 256 KiB. |
| The icon | The PNG named by `icon`: square, 1 to 256 pixels a side, at most 256 KiB, as a mutator's. |
| A top-level name ending `.md` or `.txt` | A note for people. The game does not load it. |

Any other file is refused before anything is written. The packer writes
`oakeys.yaml` first and the other files in byte order of their names. A
name ending `.png` is stored. Every other file is deflated. The same files
give the same bytes on every machine: the archive records no clock time.

A layout installs into `Documents/Open Annihilation/Keys/<id>`, by the
manifest's `id`. The install plan is the mutator's
([Where it installs](../mutators/oamut-standard.md#23-where-it-installs)).
Installing never makes a layout the one in use, and it never writes that
preference. The player chooses it afterwards.

The installer refuses an id that starts with `own-`. It will not replace a
folder the key library marks as the player's own changes, or a folder named
`own-…`. Those folders are plain folders with an `oakeys.yaml`, made by the
editor rather than installed. They are the files SHARE… packs.

## 3. The manifest

Strict YAML, exactly as a mod profile
([Strict YAML rules](../mods/oamod-standard.md#33-strict-yaml-rules)).
`oakeys` is the integer `1`. A key not in this table is refused with
`<key> is not a key of oakeys.yaml`. The writer prints the keys in this
order.

| Key | Type | Required | Limits | Meaning |
| --- | --- | --- | --- | --- |
| `oakeys` | integer | yes | `1` | The format. |
| `id` | string | yes | 1 to 64 bytes, lower-case kebab-case | The layout's id. `oa-default` and `classic` are reserved, and ids starting `own-` are refused. `classic` is never a base and never a built-in layout. |
| `name` | string | yes | 1 to 64 bytes | The name players see. |
| `version` | string | yes | as a mutator's | One to three numeric parts, quoted. |
| `author` | mapping | yes | `name`, optional `email` | As a mutator's. |
| `description` | string | no | 1 to 2,000 bytes | First line is the summary. |
| `homepage` | string | no | as a mutator's | |
| `icon` | string | no | a `.png` in the package | The badge. |
| `tags` | list | yes | 1 to 4 tags | Library filters. The Library shows a translated label for the tags in [3.1](#31-tags). |
| `base` | string | yes | `oa-default` or `none` | What this layout changes. `none` starts from nothing bound: every action the layout leaves out is unbound. `classic` is not a base. |
| `works-with` | list | no | 1 to 32 conditions | Mods this layout is made for, as a mutator's `works-with`. Absent means any. A layout made for one mod is offered when that mod is chosen. |
| `requires` | mapping | no | only `engine` | `requires.engine`, as a mutator's. |
| `keyboard` | string | no | `any`, `ansi`, `iso`, `jis` | The keyboard the layout was drawn for, for the picture only. Default `any`. |
| `bindings` | mapping | no | at most 2,048 action ids | Action id to a list of chords. See [5](#5-bindings). |
| `helpers` | mapping | no | the keys in [6](#6-pointer-schemes-and-helpers) | Input helpers the layout turns on or off. |
| `strings` | mapping | no | at most 16 language tags | Translations of `name` and `description`. |
| `imported-from` | mapping | no | `tool`, `file`, `date` | What an import made this layout from, as text. Shown in the details. |

### 3.1 Tags

Any lower-case kebab-case tag of 1 to 32 bytes is read. The Library shows a
translated label for these ten tags, and the catalogue site draws an icon
for each:

| Tag | For |
| --- | --- |
| `classic` | Close to how 3.1c played, without OA's additions. |
| `grid` | Orders and the build menu on a grid of keys under the left hand. |
| `left-hand` | Everything within reach of the left hand, mouse in the right. |
| `right-hand` | Keyboard under the right hand, mouse in the left. |
| `one-hand` | Plays with one hand on one device. |
| `mouse-first` | Most actions from the mouse alone. |
| `accessibility` | Made for a need: tremor, reach, fatigue, low vision. |
| `tournament` | Used in a tournament or ladder. |
| `mod` | Made for one mod (it names it in `works-with`). |
| `legacy` | Keeps the keys of an older tool, such as a recorder or hotkey patch. |

## 4. Chords

A chord is one string: optional modifiers joined to a trigger by `+`, and an
optional gesture after `:`.

```text
chord    = [ modifier "+" ]* trigger [ ":" gesture ]
modifier = "Ctrl" | "Shift" | "Alt" | "Meta" | "Primary"
gesture  = "double" | "hold" | "release"
```

- **Modifiers.** `Meta` is Command on macOS and the Windows key elsewhere.
  `Primary` is Command on macOS and Ctrl elsewhere, so one layout serves both.
  Left and right modifiers are one modifier. The writer prints them in the
  order Primary, Ctrl, Alt, Shift, Meta.
- **Gestures.** `double` is two presses within the double-press time;
  `hold` is a press held for the long-press time; `release` fires when the
  key comes up. A plain chord fires on the press.

### 4.1 Triggers

| Form | Meaning | Examples |
| --- | --- | --- |
| A printable character | The key that types that character on the player's keyboard layout, unshifted. Letters are written in capitals and match either case. | `A`, `M`, `.`, `/` |
| `@` and a key position | The key in that place on a US keyboard, whatever it types. The names are the W3C UI Events `code` values. | `@KeyA`, `@Digit1`, `@Backquote`, `@IntlBackslash` |
| A named key | Keys that print no character. | `F1`…`F24`, `Escape`, `Enter`, `Space`, `Tab`, `Backspace`, `Delete`, `Insert`, `Home`, `End`, `PageUp`, `PageDown`, `Up`, `Down`, `Left`, `Right`, `Numpad0`…`Numpad9`, `NumpadAdd`, `NumpadSubtract`, `NumpadMultiply`, `NumpadDivide`, `NumpadDecimal`, `NumpadEnter`, `PrintScreen`, `Pause`, `ContextMenu` |
| A mouse button | | `MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseBack`, `MouseForward`, `Mouse6`…`Mouse8` |
| A wheel turn | One notch, or one step of a trackpad's scroll. | `WheelUp`, `WheelDown`, `WheelLeft`, `WheelRight` |
| A controller button | Reserved for the controller phase; read and kept, not yet used. | `Pad.A`, `Pad.L1` |

A modifier is never a trigger on its own, except for the modifier roles in
[5.2](#52-modifier-roles). `CapsLock`, `NumLock` and `ScrollLock` are not
triggers: their lights would lie.

**Letter or place.** A printable character follows the player's layout: `A`
is where A is printed, on QWERTY and AZERTY alike, so mnemonic orders keep
their meaning. A position stays put: `@Digit1` is the key left of `@Digit2`
whether it types 1 or &, so group keys and a grid stay in one shape. The
editor writes a letter for an action marked `mnemonic` in the registry and a
position for one marked `positional`, and the player can switch either
binding. When no key on the player's layout types a character a layout
names, the binding is shown as "not on this keyboard" and does nothing.
That check is the editor's. The installer passes no keyboard map, so it
does not refuse a character this keyboard cannot type.

### 4.2 Text form of a list

A list is a flow or block sequence of chords. `[]` means unbound.

```yaml
order.attack: [A]
group.select-1: ["@Digit1"]
camera.zoom-in: [WheelUp, NumpadAdd]
order.self-destruct: ["Ctrl+D:hold"]
```

A chord with `@` or `:` is quoted, because YAML would otherwise read it
differently.

## 5. Bindings

`bindings` maps action ids to their complete list of chords. An action named
replaces the base's list; an action left out keeps the base's. At most four
chords per action.

### 5.1 Action ids

Lower-case kebab-case words joined by dots, at most 96 bytes:
`<area>.<name>`, with these areas:

| Area | For | Example |
| --- | --- | --- |
| `order` | Orders to the selected units | `order.attack`, `order.patrol` |
| `select` | Choosing units | `select.same-type`, `select.idle-builders` |
| `group` | Numbered groups | `group.select-3`, `group.assign-3` |
| `camera` | The view | `camera.pan-left`, `camera.zoom-in`, `camera.bookmark-go-2` |
| `build` | The build menu and queues | `build.page-next`, `build.cell-5` |
| `game` | The match | `game.pause`, `game.speed-up`, `game.menu` |
| `chat` | Messages | `chat.open`, `chat.open-allies` |
| `pointer` | The mouse's roles | `pointer.select`, `pointer.command` |
| `panel` | The order panel's tabs | `panel.orders-tab` |
| `ui` | Open Annihilation's own screens and the window | `ui.settings`, `ui.full-screen`, `ui.keys-overlay` |
| `watch` | Spectating and replays | `watch.player-view` |
| `megamap`, `whiteboard` | The keys of those standard hacks | `megamap.toggle`, `whiteboard.draw` |
| `debug` | Developer Mode and 3.1c's debug keys | `debug.keys` |
| `gui` | A 3.1c menu's quick key, from the mod's GUI files | `gui.ridge.ridge-pad` |

`gui.` actions exist only while the mod that defines them is loaded; their
names are `gui.<gui file stem>.<gadget name>`, lower-cased with spaces and
underscores turned to hyphens. An action that a standard hack adds exists
while its hack is on; the registry names the hack. The full list is the
[action registry](../../src/data/input-actions/registry/actions.yaml).

### 5.2 Modifier roles

A few actions are not presses but modifiers held during another action: hold
Shift to queue an order, Ctrl to add to a selection. They are actions of the
kind `modifier` (`order.queue-modifier`, `select.add-modifier`, …), and
their chord is one modifier alone (`Shift`) or a key to latch (`Q:toggle`
is not written; the helper `toggle` in 6 makes any modifier role latch).

### 5.3 What the game refuses

- A chord that two actions of overlapping contexts both use: the second
  binding in the file is dropped and a problem names both
  ([8](#8-problems)). A layout made in the editor never has one.
- More than four chords for an action.
- Every key is rebindable. The only chord the game keeps is Escape, which
  always closes the chat line and Open Annihilation's own screens. A layout
  may add chords to those actions. It may not take Escape away. A binding
  that would take it is a warning, and the layout still loads.

## 6. Pointer schemes and helpers

`helpers` turns on input helpers the layout is made around. A player can
always turn each one off again in Settings › Accessibility; the layout only
sets where it starts.

| Helper | Type | Meaning |
| --- | --- | --- |
| `toggle` | list of action ids | Hold actions that latch instead: one press on, one off. |
| `sticky-modifiers` | boolean | A tapped modifier holds for the next key or click. |
| `hold-to-confirm` | list of action ids | Actions that need a hold of the long-press time. |
| `pointer-keys` | boolean | The `pointer.*` actions move and click the pointer. |
| `radial-orders` | `off`, `hold-right`, `hold-middle` | Holding that button opens the order wheel. |
| `edge-scroll` | `on`, `off` | Scrolling at the window's edge. |

Timings (double-press, long-press, repeat, slow keys) are the player's and
never part of a layout.

## 7. Resolution

The bindings in use are made in this order; each later step replaces whole
lists for the actions it names:

1. The base layout (`oa-default`, or nothing for `base: none`).
2. The chosen layout's `bindings`.
3. The mod's own quick keys and declared actions, for actions the chosen
   layout leaves out: a `gui.` action keeps its GUI file's quick key unless
   a layout or the player names it.
4. The player's own changes, kept apart as a layout of their own whose base
   is the chosen layout ("OA default · 4 changes").

A clash that only the mod's quick keys make (step 3 against step 1 or 2) is
resolved for the layout: the layout wins, and the editor lists the quick key
under "Moved by your layout".

## 8. Problems

A layout loads whole when every problem is a warning. A file that breaks the
strict YAML rules, the table in 3 or the chord grammar is refused, with the
value's path, line and column, as a mod profile's is. The installer refuses
on an error and still installs on a warning. The question for a file the
player opened lists up to three warnings.

| Problem | Kind | Shown as |
| --- | --- | --- |
| An action id this build does not know | warning | "Not in this version of Open Annihilation" in the editor's Unknown group; kept and written back |
| A `gui.` action, or a hack's action, whose mod or hack is not on | none | listed under that mod or hack, greyed |
| Two bindings that clash | warning | the later one dropped, both named |
| A character no key types on this keyboard | warning | "Not on this keyboard" |
| A binding that takes Escape from the chat line or Open Annihilation's screens | warning | "Escape always closes the chat line and Open Annihilation's screens" |
| A malformed chord | error | refused with its path, line and column |

## 9. The text form

COPY AS TEXT in the editor writes the `bindings` mapping alone, in the
writer's form, preceded by one comment line naming the base:

```yaml
# oakeys bindings, base oa-default
order.attack: [A]
order.guard: [G]
group.select-1: ["@Digit1"]
```

PASTE accepts that text, or a whole `oakeys.yaml`, and shows what it would
change before it changes anything. Short layouts can be pasted into a forum
post or a chat window and back.

## 10. A full example

```yaml
oakeys: 1
id: grid-left-hand
name: "Grid, left hand"
version: "1.0"
author: {name: "OA team"}
description: "Orders on Q W E R, the build menu on a 4 by 3 grid of keys, groups on the number row."
tags: [grid, left-hand]
base: oa-default
keyboard: any
bindings:
  order.move: ["@KeyQ"]
  order.attack: ["@KeyW"]
  order.patrol: ["@KeyE"]
  order.repair: ["@KeyR"]
  order.stop: ["@KeyS"]
  order.guard: ["@KeyG"]
  build.cell-1: ["@KeyZ"]
  build.cell-2: ["@KeyX"]
  build.cell-3: ["@KeyC"]
  build.cell-4: ["@KeyV"]
  select.idle-builders: ["@KeyB"]
helpers:
  toggle: []
strings:
  de: {name: "Raster, linke Hand"}
```
