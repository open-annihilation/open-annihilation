# The OA UI kit for extensions

The OA UI kit ([src/ui/kit](../../src/ui/kit/README.md)) draws every one of
Open Annihilation's own screens: Settings, its notices and prompts, the
Library and the rest. From version 15 of the extension table
([README.md](README.md)) an extension builds with it too: Settings pages of
its own, screens of its own, notices and prompts, and the kit's text, chips,
progress bars and hover card over a match's battlefield. This chapter shows
how, from the Kit fixture, the engine's own test extension that uses the
kit through the table alone
([tests/extension/kit_fixture.cpp](../../tests/extension/kit_fixture.cpp)).
Every example below is taken from it as it is built and checked.

[ext]: ../../src/app/include/oa/app/extension.hpp
[regkit]: ../../src/ui/screen-registry/include/oa/ui/screen_registry/kit.hpp
[chrome]: ../../src/ui/kit/include/oa/ui/kit/chrome.hpp
[components]: ../../src/ui/kit/include/oa/ui/kit/components.hpp
[components_more]: ../../src/ui/kit/include/oa/ui/kit/components_more.hpp
[controls]: ../../src/ui/kit/include/oa/ui/kit/controls.hpp
[keys]: ../../src/ui/kit/include/oa/ui/kit/keys.hpp
[input]: ../../src/ui/kit/include/oa/ui/kit/input.hpp
[layout]: ../../src/ui/kit/include/oa/ui/kit/layout.hpp
[rows]: ../../src/ui/kit/include/oa/ui/kit/rows.hpp
[fixture]: ../../tests/extension/kit_fixture.cpp

## Contents

- [Why the kit](#why-the-kit)
- [Provisional until it is stable](#provisional-until-it-is-stable)
- [Sizes and scales](#sizes-and-scales)
- [Kit 2's look](#kit-2s-look)
- [A Settings page](#a-settings-page)
- [A kit screen](#a-kit-screen)
- [Notices and prompts](#notices-and-prompts)
- [Drawing in a match overlay](#drawing-in-a-match-overlay)
- [Focus and touch](#focus-and-touch)
- [Automation names](#automation-names)
- [First-run pieces](#first-run-pieces)
- [The gallery](#the-gallery)
- [Limitations](#limitations)

## Why the kit

A page or a screen built with the kit looks like Open Annihilation's own,
and the engine does for it what it does for its own:

- it lays it out in points at the size class of the room the window leaves,
  and draws it at the window's own pixels, sharp at every whole scale;
- it moves the keyboard and controller focus, gives a finger's press its
  reach, and maps the pointer, a finger and the keys to the kit's input;
- it names every control to the automation endpoint, so a program can
  drive the page or the screen by name;
- it shows a notice or a prompt in turn with its own, so that two never
  show at once.

A Settings page is a table of declared rows, with no drawing and no input
code of the extension's own. A screen is a model, a layout made from the
kit's components and a function that takes the presses and keys its
controls do not; the kit does the drawing, the hit testing, the focus, the
scaling and the reach. The [gallery](#the-gallery) shows every component.

## Provisional until it is stable

The kit is provisional (decision D31). Until it is declared stable, a
decision of its own that is not tied to a release, it changes whenever Open
Annihilation's screens need it to, and every kit header says so in its
first comment.

From version 15 the kit's public headers, `oa/ui/kit/*.hpp`, are part of
the extension table's contract, which `OA_EXTENSION_API_VERSION` numbers
([README.md](README.md#the-version)). A change to them before 0.8.0, the
first release that carries version 15, keeps 15. From 0.8.0 on, every
change to a kit header raises the version, so an extension that states the
version it follows stops building until its author has looked at the
change.

So expect rework. A change to the kit may rename a function, add a member,
change a look or move a size, and an extension that uses it follows that
change, as the engine's own screens do.

## Sizes and scales

The kit lays out in points. One point is one pixel of the game's 640x480
picture, and the engine draws a point as `scale` pixels of the window. The
Auto scale is the window's height over 720, the nearest step, never below 1
and never below the display's own density; it is then held to the largest
scale at which Compact's 480 by 324 point dialog fits the window. While the
modern fonts are on and opened, 150% is one of the steps and a tie takes
the larger: 1x below 900 lines, 150% from 900, 2x from 1260 and 3x from
1800. While they are off, or they did not open, the steps are whole: 1x
below 1080 lines, 2x from 1080, 3x from 1800.

The points left over choose the size class: the largest whose settings
dialog fits them with `class_margin` (24) points round it, so Compact
below 768 by 534 points, Regular from `regular_from` (768 by 534) and
Large from `large_from` (1008 by 648) (`SizeClass`, `size_class_of` in
[layout.hpp][layout]). Where Auto's scale leaves the room below Large and
one step down gives Large, Auto takes that step (`layer_viewport`), so a
larger window shows the larger layout rather than the smaller one drawn
bigger. At some common windows, with 150% not offered:

| Window | Auto scale | Points | Size class |
|---|---|---|---|
| 640x480 | 1x | 640 by 480 | Compact, pixel for pixel as 0.7.3 |
| 800x600 | 1x | 800 by 600 | Regular |
| 1024x768 | 1x | 1024 by 768 | Large |
| 1280x720 | 1x | 1280 by 720 | Large |
| 1920x1080 | 1x, a step down from 2x | 1920 by 1080 | Large |
| 2560x1440 | 2x | 1280 by 720 | Large |
| 3840x2160 | 3x | 1280 by 720 | Large |
| a phone, 852 by 393 points at 3 pixels a point | 3x | 852 by 393 | Compact |

With the modern fonts on, 1920x1080 is 1.5x instead: 1280 by 720 points,
Large. 2560x1440 stays 2x.

The player's Interface size (Settings › User Interface) replaces Auto with
100%, 150%, 200%, 300% or 400%, held the same way and never stepped down.
150% is offered only while the modern fonts are on: the game's bitmap fonts
take only whole steps, so the kit draws 150% on its smooth form, in the
modern fonts, with every edge on a whole pixel. A 1920x1080 window at 150%
is Large, at 1.5x. The player's Window size (the same section) chooses the
class instead of Auto: a chosen class is used wherever its dialog fits the
room whole, and is held to the largest that fits elsewhere
(`size_class_within`); Auto's step down then serves that class.

Each class has its metrics (`metrics_of` in [layout.hpp][layout], the
values in `theme.hpp`), of two kinds. Kit 2's scales grow with the class:
the type scale (`title_size`, `label_size`, `body_size`, `small_size`),
the spacing (`space` 4, 6 and 8 points, `row_gap`, `section_gap`,
`window_padding` 12, 18 and 24), the radii (`keycap_radius` to
`dialog_radius`), and Kit 2's control sizes:

| Metric | Compact | Regular | Large |
|---|---|---|---|
| `control_height` | 28 | 34 | 38 |
| `row_height` | 36 | 40 | 44 |
| `text_field_height` | 30 | 36 | 40 |
| `filter_chip_height` | 24 | 26 | 28 |
| `tab_strip_height` | 32 | 38 | 44 |
| `window_header_height`, `window_footer_height` | 36, 48 | 36, 48 | 36, 48 |
| `window_radius`, `dialog_radius` | 10, 12 | 11, 13 | 12, 14 |

The members the older layouts are built on (`button_height`,
`field_height`, `tab_height`, `header_height`, `footer_height`, `padding`
and the rest) keep Compact's sizes in every class and differ only in the
room a dialog takes, so a screen laid out with them shows more rows at a
larger class rather than larger ones:

| Metric | Compact | Regular | Large |
|---|---|---|---|
| `dialog_width` by `dialog_height` | 480 by 324 | 720 by 486 | 960 by 600 |
| `padding` | 12 | 16 | 20 |
| `nav_width` | 144 | 176 | 208 |
| `notice_width` | 400 | 520 | 600 |
| `greatest_notice_height` | 440 | 500 | 640 |

Settings, the Library and the key editor are laid out on Kit 2's sizes. A
layout takes its sizes from the frame it is given (`Frame`: its area in
points, its size class and its scale) and from `metrics_of` that class,
never from the window, and works in the smallest room as in the largest.

The engine rounds the corners of the windows and dialogs it shows: a
window that moves takes its class's `window_radius`, a notice, a prompt or
a question its `dialog_radius`, and the picture behind shows outside the
curve. A screen draws its own rectangle square; the rounding is the
layer's.

## Kit 2's look

The colours are tokens (`kit::colour` in `theme.hpp`): a ramp of five
surfaces from the bars and wells to the selected row, three text tones
(primary, secondary and tertiary, which hints and quiet text take), the
lime accent with its light tone, which is the focus ring, caution amber,
a lifted red for danger, an information blue, and the brand gold, worn
only by the OA mark. Badges and chips take tinted fills of those colours.
Buttons come in looks (`ButtonStyle`): the primary, lime with dark ink;
the secondary, raised with a strong edge; quiet, a lime caption with no
face at rest; and danger, outlined in red. Buttons, fields, drop-downs,
list rows, cards and menus are rounded with the control's or the card's
radius; chips, pills, the switch, the slider and the progress bar are
pills. The switch is a 42-point track with a round knob, lime while On. A
focused control wears a one-point ring of the light accent one point
clear of it, and a focused field also takes a soft ring. A progress bar
drawn paused (`ProgressLook::paused`) fills amber.
[src/ui/kit/README.md](../../src/ui/kit/README.md) lists every token and
its value.

The letters follow the player's Look (Settings › User Interface). In the
modern look, the default, the kit draws in Barlow: labels, values and
buttons in Barlow SemiBold, hints and small text in Barlow Regular, and
tracked capitals, window titles and section headings, in Barlow
Condensed, at the type scale's steps (`TextLevel`), with DejaVu Sans, a
language's own face and Noto Emoji drawing what Barlow lacks. In the
classic look it draws the game's two bitmap fonts, titles and labels in
the regular font and body and small text in the small one
(`classic_font`). `kit::Fonts::modern` is the modern look's font stack,
null in the classic look; `modern_look` tells which look a `Fonts` draws,
and `ModernType`, `modern_type`, `modern_width`, `modern_line` and
`modern_capitals` measure and draw in it. A screen that measures its
words with `text_width` or `tracked_width` and draws them with `box_text`
follows the look with no change of its own. A screen that sizes its parts
from the game font's widths should measure its words instead, since
Barlow's widths differ.

## A Settings page

An extension's page is listed in Settings' nav after the engine's own
sections, under a divider that names its owner, and the engine draws,
scales, focuses, saves and restores it as it does its own pages. The page
is a table of rows bound to preference keys. The Kit fixture's page has a
switch and a drop-down.

Its names, at the top of [kit_fixture.cpp][fixture] (with
`namespace app = oa::app;` and `namespace kit = oa::ui::kit;`):

```cpp
// The Settings page and the preference keys its rows are bound to.
constexpr const char* kPageId = "kit-fixture.options";
constexpr const char* kPageTitle = "Kit fixture";
constexpr const char* kPageOwner = "Kit fixture";
constexpr std::string_view kShowChipKey = "kit-fixture.show-chip";
constexpr std::string_view kModeKey = "kit-fixture.mode";
// What show-chip holds while the chip is drawn, and what it reads when none
// is stored.
constexpr std::string_view kShowChipOn = "1";
```

Its rows:

```cpp
const kit::PreferenceRow* page_rows() {
    static const std::array<std::string_view, 2> mode_ids{"quiet", "loud"};
    static const std::array<std::string_view, 2> mode_captions{"Quiet", "Loud"};
    static const kit::PreferenceRow rows[] = {
        kit::preference_toggle(
            "show-chip",
            "Show the fixture's chip",
            kShowChipKey,
            kShowChipOn,
            {"A chip, a bar and a line of text over the battlefield."}
        ),
        kit::preference_choice("mode", "Fixture mode", kModeKey, "quiet", mode_ids, mode_captions),
    };
    return rows;
}
```

Line by line:

- Everything is `static`. The registry keeps a pointer to the rows, and a
  row keeps views of its texts and of the choices' ids and captions, so all
  of them must live as long as the runtime.
- `mode_ids` are the words the drop-down stores, one for each caption, and
  `mode_captions` what it shows, in English, in the same order. An id is
  one word of `a` to `z`, `0` to `9` and hyphens.
- `kit::preference_toggle` makes a row with an Off/On switch: its id
  (`show-chip`, one word, unique on the page), its label, the key it is
  bound to, the text read when nothing is stored (here `1`, On) and up to
  two hint lines. It stores `1` or `0`.
- `kit::preference_choice` makes a row with a drop-down: its id, its label,
  its key, the id read when nothing is stored, and the ids and captions. It
  stores the chosen id.
- `kit::preference_slider` and `kit::preference_text_field` are the other
  two kinds a page takes ([rows.hpp][rows]). A slider stores a decimal on
  its step between its least and greatest values; a text field stores the
  player's text, up to 256 bytes.

Every stored value is text in the player's preferences file. A missing or
malformed value reads as the row's default. A key is dotted words of `a` to
`z`, `0` to `9` and hyphens, at least two words and at most 64 bytes, and it
never starts with `open-annihilation.`, the engine's own keys
(`kit::preference_key_problem` says why a key is refused). Each hint line is
one line in the small font, kept within Compact's content column: about
50 characters (`compact_metrics.hint_line_characters`).

The page itself, from the table's `register_settings_pages`:

```cpp
void page_applied(void*, const char* key, const char* value) {
    say(std::string("applied ") + (key != nullptr ? key : "") + '=' +
        (value != nullptr ? value : ""));
}
```

```cpp
void register_settings_pages(void* context, app::SettingsRegistry* registry) {
    app::SettingsPageDesc page{};
    page.id = kPageId;
    page.title = kPageTitle;
    page.owner = kPageOwner;
    page.rows = page_rows();
    page.row_count = 2;
    page.applied = page_applied;
    page.state = context;
    // A page the registry refuses stops the start once this returns.
    (void)app::settings_page_register(registry, page);
}
```

Line by line:

- `register_settings_pages` is called once while each runtime is built,
  right after `register_screens`, on the main thread.
- `page.id` is the page's word: dotted words of `a` to `z`, `0` to `9` and
  hyphens, at most 48 bytes, and none of the engine's section words
  (`mods`, `controls`, `developer` and the others, and `registered`). It
  names the page's controls to automation.
- `page.title` is the nav entry's text, and `page.owner` the divider's
  label: the pages of one owner share one divider. Both are UTF-8, and the
  registry copies them, with the id.
- `page.rows` and `page.row_count` are the table. Every row must be bound
  to a key, and two rows of a page never share an id.
- `page.applied` is called on the main thread once for each key OK
  changed, after the preferences are written, with the key and the text
  stored, both read at once. Null calls nothing. `page.state` is handed
  back to it; the fixture passes its context. The fixture's `applied`
  prints the change with `say`, its own function that writes a line its
  check reads.
- `app::settings_page_register` returns 1 when it takes the page and 0
  when it refuses it. A page refused for a malformed or taken id, an engine
  section's word, a missing title or owner, no rows, a row with no binding
  or a malformed key, or a full registry (16 pages) stops the start once
  the hook returns, with "settings page registration rejected: <reason>",
  the first refusal's reason.

The engine does the rest. OK writes each key whose text changed and then
calls `applied`; Cancel drops the changes; Restore defaults puts the open
page's rows back to their defaults. Tab visits the rows in the table's
order. A mod's options dialog lists no extension page.

Code elsewhere in the extension reads a stored value through the services
table, whose `read_string` reads a whole key when its section is null
(version 15). The fixture's overlay reads its switch so:

```cpp
bool chip_shown(const Fixture& state) {
    if (state.services == nullptr)
        return true;
    char stored[8]{};
    const std::string key(kShowChipKey);
    if (state.services->read_string(state.host, nullptr, key.c_str(), stored, sizeof stored) == 0)
        return true;
    return std::string_view(stored) == kShowChipOn;
}
```

`read_string` answers 0 when nothing is stored (or the value does not fit),
and the row's default, On, applies.

## A kit screen

A kit screen is a model, a layout and an action handler, registered by name
from `register_screens` and opened by that name. The engine hosts it on its
OA layer over the front end: modal, over a backdrop, filling the front
end's room, laid out at the room's size class and scale, and focused,
reached and named as its own screens are.

**Registering it.** A `KitScreenDesc` ([screen_registry/kit.hpp][regkit])
holds the screen's name, its `model`, and the functions `lay_out`, `act`
and `closed`, each called on the main thread with the model. The fixture's
`register_screens` registers its screen, and an overlay of the main menu
through which it keeps the services:

```cpp
void register_screens(void* context, app::ScreenRegistry* registry) {
    app::KitScreenDesc screen{};
    screen.name = kScreenName;
    screen.lay_out = lay_out_screen;
    screen.act = act_screen;
    screen.closed = screen_closed;
    // A screen the registry refuses stops the start once this returns.
    (void)app::kit_screen_register(registry, screen);
    app::OverlayDesc overlay{};
    overlay.name = "kit fixture";
    overlay.screen = app::screen_id(app::Screen::main_menu);
    overlay.z = kOverlayZ;
    overlay.create = keep_services;
    overlay.tick = first_main_menu_frame;
    overlay.state = context;
    (void)app::overlay_register(registry, &overlay);
}
```

The name (`kit-fixture.screen`) has a page id's form: dotted words, at most
48 bytes, unique among the kit screens. The registry copies the
description; the name and the model, and what they point at, live as long
as the runtime. The fixture's screen keeps no state, so its model is null.
A screen with no `lay_out` or `act`, a taken or malformed name, or a full
table (16 screens) is refused, and the start stops.

**The model** is the extension's: whatever its screen shows and changes.
The kit keeps no state of a screen's own. The engine keeps the pointer's
hover and press and the focus, and draws them on a copy of the list
`lay_out` returned: the control under the pointer lights up, a press held
on it shows the held look, and the focused control wears the focus ring.
The list the extension returned is left as it was.

**The layout.** `lay_out` returns the screen's display list for a frame:
`items`, what is drawn, in drawing order; `controls`, what a press can
reach, with their names; and `tab_order`, the order Tab follows. The
engine calls it when the screen opens, whenever the frame's size, class or
scale changes, and after `act` asks for it. The fixture's screen is a
heading, three list rows and CLOSE, one under another:

```cpp
kit::DisplayList lay_out_screen(void*, const kit::Frame& frame) {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const int32_t width = std::max(frame.area.width, 2 * kScreenPadding + kCloseWidth);
    const int32_t height = std::max(frame.area.height, 1);
    const int32_t inner = width - 2 * kScreenPadding;
    kit::DisplayList list;
    kit::Item face;
    face.role = kit::Role::fill;
    face.rect = {0, 0, width, height};
    face.colour = kit::colour::panel;
    list.items.push_back(std::move(face));
    kit::Item heading;
    heading.role = kit::Role::heading;
    heading.rect = {kScreenPadding, kScreenPadding, inner, metrics.regular_line};
    heading.look = kit::HeadingLook{"KIT FIXTURE"};
    list.items.push_back(std::move(heading));
    static const std::array<const char*, kRowCount> titles{"First row", "Second row", "Third row"};
    static const std::array<const char*, kRowCount> names{
        "kit-fixture.screen.first", "kit-fixture.screen.second", "kit-fixture.screen.third"
    };
    int32_t top = kScreenPadding + metrics.regular_line + kScreenGap;
    for (kit::ControlId row = 0; row < kRowCount; ++row) {
        const auto at = static_cast<std::size_t>(row);
        kit::ListRowLook look;
        look.title = titles[at];
        look.lines = {"A row of the fixture's screen."};
        kit::add_list_row(
            list,
            {kScreenPadding, top, inner, metrics.list_row_height},
            look,
            kFirstRow + row,
            names[at]
        );
        top += metrics.list_row_height + kRowGap;
    }
    top += kScreenGap - kRowGap;
    kit::ButtonLook close_look;
    close_look.caption = "CLOSE";
    close_look.style = kit::ButtonStyle::accent;
    kit::add_button(
        list,
        {kScreenPadding, top, kCloseWidth, metrics.button_height},
        close_look,
        kClose,
        "kit-fixture.screen.close"
    );
    list.tab_order = {kFirstRow, kFirstRow + 1, kFirstRow + 2, kClose};
    return list;
}
```

- The screen's points start at its own top left corner, and it fills the
  frame's area: `frame.area.width` by `frame.area.height` points.
- The fixture's own constants are points (`kScreenPadding` 12,
  `kScreenGap` 8, `kRowGap` 2, `kCloseWidth` 80) and control numbers
  (`kFirstRow` 1, `kRowCount` 3, `kClose` 4). A control's number is the
  screen's own; `act` is told it.
- Sizes come from `kit::metrics_of(frame.size_class)`: the regular line,
  a list row's height, a button's height.
- Generic items (`Role::fill`, `Role::heading` with a `HeadingLook`) are
  drawn and take no input. Colours are the kit's tokens (`kit::colour`).
- Each `kit::add_*` function appends a component: its item, and the
  control a press reaches, with its number and its name. The components
  are in [controls.hpp][controls] (list rows, chips, fields, tabs, cards,
  hover cards, links, the package badge and the drag handle),
  [components.hpp][components] (buttons, switches,
  level strips, sliders, drop-downs, the lock, the focus ring, and
  `draw_tag_icon` for the eight catalogue tag icons),
  [components_more.hpp][components_more] (the progress bar, drawn amber
  while paused, a question's item lines and its parts, and the update
  pill),
  [chrome.hpp][chrome] (the nav list, whose entries can lead with a tag icon)
  and [rows.hpp][rows] (declared rows, `add_rows`). A tag icon is strokes
  sampled at the canvas's pixels, not a picture. A package badge
  (`BadgeLook`, `add_badge`) draws a picture, else the first tag's icon on
  the hint ground, else letters, else a dashed square. A drag handle
  (`GripLook`, `add_grip`) is two columns of four dots. Keycaps, chord
  chips, a keyboard picture, a mouse picture and a hold ring are in
  [keys.hpp][keys] (`KeycapLook`, `ChordChipLook`, `KeyboardLook`,
  `MouseLook`, `HoldRingLook`, and `add_keycap`, `add_chord_chip`,
  `add_keyboard`, `add_mouse`, `add_hold_ring`). A category tint is one of
  `colour::category_orders` through `category_developer`. A question's
  `items` are lines under its paragraphs, each a badge, a name and an aside.
- A button whose mark is `ButtonMark::refresh` draws a circular arrow
  instead of its caption. `turn` is the eighth of the turn (one full turn
  is `refresh_revolution_ms`, 1.6 seconds) and `turning` keeps the arrow in
  the text colour while the button is disabled. `check_ms` is how long a
  finished refresh has shown its check: the arrow eases into a check in the
  accent over `refresh_ease_ms`, the check holds until `refresh_check_ms`
  (0.8 seconds), and then the arrow is back. The same look draws the same
  animation on every screen. The OA button's spinner uses the same turn.
- Every control's name starts with the screen's name. Names are words of
  `a` to `z`, `0` to `9` and hyphens joined by dots, at most 100 bytes, and
  unique on the screen (`kit::name_problem` reports the first fault).
- `tab_order` lists the controls Tab visits, in order. Only these take the
  focus.
- The list is returned by value and owns its texts, so the screen may build
  its texts on the fly.

**Actions.** The engine calls `act` with a `KitScreenAction`: the control
pressed and `Key::enter` for a press (a pointer released over the control
it pressed, or a finger's press), the focused control and the key for a key
that control takes (Space, Enter on a button, Left and Right on a control
that steps), or `kit::no_control` and the key for any other key (Escape,
Enter with no button focused, Page Up). A Tab or an arrow that moves the
focus does not reach `act`; one that finds nowhere to go reaches it with no
control. It answers 0 for nothing, 1 to lay the screen out again and draw
it, and 2 to close it:

```cpp
int act_screen(void*, const app::KitScreenAction& action) {
    if (action.control == kClose)
        return kActClose;
    if (action.control == kit::no_control && action.key == kit::Key::escape)
        return kActClose;
    return kActNothing;
}
```

**Opening and closing.** `open_kit_screen` in the services table opens a
registered screen by name, from any callback given a `ScreenContext`, or
later with the host and services kept from one. It answers 1 when it opened
the screen and 0 for a name no kit screen has. The fixture keeps the
services from its overlay's `create`, and opens its screen on the main
menu's first frame when `--kit-fixture-screen` asks:

```cpp
void keep_services(app::ScreenContext* ctx, void* state) {
    auto& kept = *static_cast<Fixture*>(state);
    kept.host = ctx->host;
    kept.services = ctx->services;
}
```

```cpp
void first_main_menu_frame(app::ScreenContext*, void* state) {
    auto& kept = *static_cast<Fixture*>(state);
    if (kept.asked || kept.services == nullptr || (!kept.open_screen && !kept.ask_prompt))
        return;
    kept.asked = true;
    if (kept.open_screen && kept.services->open_kit_screen(kept.host, kScreenName) != kOpened)
        say("the screen did not open");
    if (kept.ask_prompt)
        ask(kept);
}
```

The screen closes when `act` answers 2, and `closed` is called once as it
leaves, whatever closed it:

```cpp
void screen_closed(void*) {
    say("screen closed");
}
```

## Notices and prompts

The engine keeps one queue of notices and prompts, its own and every
extension's. They wait first in, first out, and the first shows when no
other modal screen shows, no front-end message box is open and no match is
being played, over any of the menus; so two never show at once, and none
opens over Settings or a match. A notice tells the player something and
closes with OK (or Enter or Escape). A prompt asks, with one to three
buttons (`app::most_prompt_buttons`).

A question can also list packages under its paragraphs. Each `QuestionItem`
is a badge (`BadgeLook`), a name and an aside drawn after the name in the
hint colour. The kit draws them; the caller supplies the words. With no
items the question is unchanged. The engine's own questions can also hold
parts (`Question::parts`): a paragraph in the secondary colour, a tinted
callout with its mark, one box across or two side by side, each a heading
and lines marked done, failed or with a bullet, and a quiet button at the
footer's left or a danger button outlined in red. The update screens use
them; the services table's requests do not carry them.

The engine shows a notice or a prompt with rounded corners, its class's
`dialog_radius`.

Both are requests in [screen_registry/kit.hpp][regkit], queued through the
services table, which copies the request and every string it names at
once, so the arrays may be the caller's own locals. Each call answers the
request's id, never 0, or 0 when it refuses the request. The fixture's
prompt, which `--kit-fixture-prompt` asks for:

```cpp
void ask(const Fixture& state) {
    const char* const paragraphs[] = {"Does the fixture's prompt reach its answer?"};
    const char* const buttons[] = {"No", "Yes"};
    const char* const button_ids[] = {"no", "yes"};
    app::PromptRequest request{};
    request.title = "Fixture question";
    request.paragraphs = paragraphs;
    request.paragraph_count = 1;
    request.buttons = buttons;
    request.button_count = 2;
    request.button_ids = button_ids;
    request.cancel_button = 0;
    request.primary_button = 1;
    request.answered = prompt_answered;
    // The request is copied at once, so these arrays may go when it returns.
    if (state.services->ask_prompt(state.host, &request) == 0)
        say("the prompt was refused");
}
```

- `title` and `paragraphs` are UTF-8 and drawn as given; the kit looks no
  text up.
- `buttons` are the captions, left to right, and `button_ids` their words,
  one word of `a` to `z`, `0` to `9` and hyphens each, which name them to
  automation. Null ids name the buttons by their places.
- `cancel_button` is the button Escape (and No) answers, and
  `primary_button` the one drawn in the accent, which Yes answers; each is
  from 0, or -1 for none.
- A null title, paragraph or caption, no buttons or more than three, a
  malformed id, or a cancel or primary button that is not one of the
  buttons is refused, and the call answers 0.

The answer comes once, on the main thread, to `answered`, with the
request's id and the button pressed, from 0. When the runtime ends with the
prompt still queued or shown, `answered` is called with -1 instead, so
whatever the extension keeps for the prompt is freed either way. `user` is
handed back to it.

```cpp
void prompt_answered(void*, uint32_t, int32_t button) {
    static constexpr std::array<const char*, 2> ids{"no", "yes"};
    const bool known = button >= 0 && static_cast<std::size_t>(button) < ids.size();
    say(std::string("answered ") + (known ? ids[static_cast<std::size_t>(button)] : "none"));
}
```

A notice is a `NoticeRequest` of a title and paragraphs, queued with
`show_notice`. It has no answer: the player reads it and presses OK.

## Drawing in a match overlay

`draw_match_overlay` draws over the battlefield through the `MatchOverlay`
painter it is given ([extension.hpp][ext]). Positions there are the
painter's pixels, and `MatchOverlay::scale` of them make up one pixel of
the game's 640x480 screen: the HUD's scale, which follows the match's
layout and HUD scaling, and is at least 1. From version 15 the painter has
five calls that draw the kit's components at that scale, one point to
`scale` pixels, crisp and in the game's fonts, as the kit draws Open
Annihilation's own screens at a whole scale, each clipped to the
battlefield:

- `kit_text_width`: a line's width as `draw_kit_text` draws it, the kit's
  width in points (`kit::text_width`) times `scale`;
- `draw_kit_text`: a line of text in the kit's text colour, in the regular
  or the small font (`kit::FontRole`), the font's line high;
- `draw_chip`: a chip (`kit::ChipLook`, its text and state), as wide as its
  text and the chip's padding either side and the chip's height high;
- `draw_progress`: a progress bar filled from its left, from 0 to 1000
  thousandths, or busy with -1;
- `draw_hover_card`: a hover card (`kit::HoverCardLook`), its arrow outside
  its rectangle.

A look is read at once. The calls draw nothing while the kit's fonts are
not loaded. The engine's painter fills all five; the fixture checks them
before it calls them all the same. It draws a chip, a half-full bar and a
line of text, 8 pixels of the 640x480 screen in from the battlefield's top
left, while its page's switch is On:

```cpp
void draw_match_overlay(void* context, app::Runtime&, const app::MatchOverlay& overlay) {
    auto& state = *static_cast<Fixture*>(context);
    if (!chip_shown(state) || overlay.kit_text_width == nullptr || overlay.draw_chip == nullptr ||
        overlay.draw_progress == nullptr || overlay.draw_kit_text == nullptr)
        return;
    const int scale = std::max(overlay.scale, 1);
    const int left = overlay.left + kOverlayInset * scale;
    int top = overlay.top + kOverlayInset * scale;
    kit::ChipLook chip;
    chip.text = kChipText;
    chip.state = kit::ChipState::installed;
    // A chip is its text in the small font with the kit's padding either
    // side, and the kit's chip height (controls.hpp, theme.hpp).
    const int chip_width =
        overlay.kit_text_width(overlay.painter, kit::FontRole::small, kChipText) +
        2 * kit::compact_metrics.chip_padding * scale;
    const int chip_height = kit::compact_metrics.chip_height * scale;
    overlay.draw_chip(overlay.painter, left, top, &chip);
    if (!state.chip_printed) {
        state.chip_printed = true;
        // An installed chip's outline is the accent (draw_chip).
        say("chip at " + std::to_string(left) + ',' + std::to_string(top) + ' ' +
            std::to_string(chip_width) + 'x' + std::to_string(chip_height) + " outline " +
            hex_colour(kit::colour::accent));
    }
    top += chip_height + kOverlayGap * scale;
    overlay.draw_progress(
        overlay.painter, left, top, kBarWidth * scale, kBarHeight * scale, kBarHalf
    );
    top += (kBarHeight + kOverlayGap) * scale;
    overlay.draw_kit_text(overlay.painter, kit::FontRole::regular, left, top, kLineText);
}
```

Every size in points, from the kit's metrics or the extension's own, is
multiplied by `scale` before it is passed, and `kit_text_width` already
answers in the painter's pixels.

## Focus and touch

A kit screen and a Settings page are driven by the kit's one input model
([input.hpp][input]), which the engine applies for the extension:

- **Tab** follows the declared order, `DisplayList::tab_order` (a page's
  rows in its table's order), and wraps round; Shift+Tab goes back. Only
  the controls in it take the focus (decision D30).
- **The arrows** and a controller's stick move the focus by where controls
  lie, not by that order (`kit::focus_toward`): to the nearest control that
  lies wholly that way, one in line first. A control's own scroll area is
  searched first. Left and Right go to a focused control that steps, such
  as a switch, a slider or a drop-down, rather than moving the focus. The
  weighting is one function, to be tuned after controller playtests.
- **Enter** presses a focused button, link, list row or tab, and **Space**
  the focused control. With no focus shown, the first key shows it: Tab,
  Down, Left, Right and Space on the first control of the order, Up and
  Shift+Tab on the last.
- **A finger's press** goes to the nearest enabled control within the touch
  controls' reach (`kit::reach`), which the engine gives in the screen's
  points, so a small control is reached by a finger that lands near it.
  Keep controls far enough apart that the nearest one is the one meant.

So a layout needs no focus or touch code: it places its controls, gives
each a name and lists the Tab order.

A notice and a prompt keep a simpler form of it: the arrows and Tab move
the mark between their buttons, Space presses the marked one, and a prompt
takes Enter as Space; a press or a finger with the same reach answers them
too.

## Automation names

The automation endpoint ([docs/automation.md](../automation.md#oas-own-screens))
lists every control of the kit under `oa.` and its kit name, so a program
can drive an extension's page, screen, notice or prompt by name, as the
Kit fixture's check does. A part of a control that takes a press of its own
is named after it with one word more: a switch's halves `.off` and `.on`, a
strip's levels and an open drop-down's items by their ids, a row of
buttons' buttons by their words.

| What | Dialog | Names |
|---|---|---|
| A Settings page's nav entry | `oa.settings` | `oa.settings.nav.<page id>`, a check box checked while the page shows |
| A page's row | `oa.settings` | `oa.settings.<page id>.<row id>`, as `oa.settings.kit-fixture.options.show-chip`, with `.off` and `.on`, or `.<choice id>` for an open drop-down's items |
| A kit screen's control | `oa.<screen name>` | `oa.<control name>`, as `oa.kit-fixture.screen.close` |
| A notice | `oa.registered` | `oa.registered.ok`, and its words `oa.registered.body` |
| A prompt | `oa.prompt` | `oa.prompt.<button id>`, as `oa.prompt.yes`, or `oa.prompt.button-<n>` from 1 without ids, and its words `oa.prompt.body` |

The names are lower case, so two names that differ differ ignoring case as
well, and every word comes from the code (an id, a row's word, a button's
word), never from a text as the player sees it, so no name changes with
the language.

## First-run pieces

`oa/ui/kit/first_run.hpp` holds the pieces a first-run flow is built from.
An extension draws them as it draws a button: a look, a rectangle and a
draw function, or an item on a display list. The words are the caller's,
already looked up. The kit does not read a preference.

Reduced motion is `Frame::reduced_motion`, off by default. It comes from a
later Settings row. A screen copies the flag onto the carousel's look and
the call to action's look, and the draw functions read the look. Off, a
carousel turns and a call to action sends out a ring. On, the carousel
slides flat and the call to action holds a steady ring.

- **Perspective blit** (`perspective_blit` and `scale_blit`, in
  `canvas.hpp`). Draws a rectangle into one reused offscreen buffer, then
  onto the canvas as a trapezoid. One vertical edge keeps its height and
  the other is shorter, so the rectangle looks turned about its vertical
  axis, at an opacity. On the crisp form each destination pixel copies the
  one nearest source pixel. On the smooth form each destination pixel
  blends the source pixels around its sample. The sample runs from the
  source's first pixel at the trapezoid's first pixel toward one past the
  last pixel at the far edge, so a sample between two pixels mixes them by
  the distance. `scale_blit` is that copy with no turn. A
  drawing that is already using the buffer draws nothing if it asks again.
- **Language card** (`draw_language_card`, `language_card_layout`). At
  Large the card is 232 by 304 points; Compact and Regular keep that
  shape. Padding, the badge and the gap under it scale from that card to
  the rectangle. The badge is centred at the top: the pack's picture, or
  the language's letters on the hint grey (accent while the card is
  active), large and centred. Under it, centred, the language's own name
  (large, and shrunk until it fits), its English name, a facts line and a
  pill centred at the bottom. A facts line wider than the card wraps onto
  the next small line, above the pill, so every word is shown. An active
  card has an accent border and a one-point accent ring outside it. The
  carousel's chosen card (`CarouselLook::chosen`, or with none the centre
  card) draws that ring once it faces the player; a side card's turn and
  opacity come from the carousel.
  `LanguageAvailability` chooses the pill: built in,
  get, downloading and checking are neutral, active uses the accent pill,
  a failed fetch uses the problem pill, and offline is neutral with the
  card dimmed. Hovered, held and disabled change the face. Progress is the
  kit's progress bar under the card, not inside it.
- **Carousel** (`draw_carousel`, `carousel_fit`, `carousel_places`,
  `carousel_go`, `carousel_tick`, `carousel_key`, `carousel_command`,
  `carousel_pointer`). Cards in a ring. The centre faces the player. Each
  step out turns 34°, falls back, shrinks and dims, and two cards show on
  each side. A move takes 0.5 s and eases out, from the time passed, so
  the place is the same however many frames drew it. The arrows are the
  caller's buttons. A pager lights the centre card's bar. Left and Right
  move one card, Enter on the centre picks it, a press on a side card
  centres it, and a swipe moves one card. Shoulders and a stick are not
  keys: the host fills a `CarouselCommand`. Each card is a control named
  `oa.<screen>.card.<tag>`, and so is each arrow.
- **Step indicator** (`draw_step_indicator`). The step's name and number in
  the small font, letter-spaced, as "LANGUAGE · 1 OF 3", and one segment a
  step, lit from the first up to the current one.
- **Setup chrome** (`draw_setup_chrome`). The kit window's header, the mark,
  "Open Annihilation", a muted page word and the step indicator at the
  right, and its footer: Skip setup, a quiet button on the left, then Back
  and an accent Next. It spans the window and is drawn from the canvas's
  origin. With `SetupChromeLook::brand` off, as in a window whose title
  bar names the game, the header leaves out the mark and the name, and the
  page word stands at its left.
- **Choice card** (`draw_choice_card`, `choice_step`). A radio row with a
  title and a hint: idle, hovered, held, chosen and disabled. Up and Left
  move to the previous row of a group, and Down and Right to the next.
- **Coach mark** (`draw_coach_mark`, `place_coach_mark`) and **spotlight**
  (`draw_spotlight`). A numbered card with an accent border and a one-point
  accent connector to a target. The card is tried on the right of the
  target, then the left, then below, then above, with a gap of 8 points,
  so it stays in the room and never covers the target. The spotlight dims
  the layer to 74 percent of the backdrop except a cut-out around the
  target, which keeps an accent ring.
- **Picture well** (`draw_picture_well`, `picture_well_layout`). A rectangle
  with a one-point border and a 6-point inset. The caller draws into the
  inset, scaled with `scale_blit`, and a caption sits under it. The kit
  ships no picture for the well. A stand-in pattern fills the inset when a
  sample shows the well alone.
- **Call to action** (`draw_call_to_action`). An accent button for a flow's
  last step. It brightens to the light accent and sends out a ring every
  1.8 s, from the time. Reduced motion holds a steady ring. Disabled, it
  is the disabled button and sends no ring.

The gallery's First run section shows every one of them, across the full
width, at the language card's real size, with the carousel turned. The
cards are tall, so a window shows the top of that section and the rest
sits below it.

## The gallery

The UI kit gallery shows every component of the kit, once working on its
own model and once in each state (idle, hovered, pressed, focused,
disabled, and locked where a component has a lock), at the scale and size
class of the window. The Chips section also shows the two maker's marks
at the state icon's size: the OA badge is the app icon when the game has
given one, filtered down, and the drawn plate when it has not, and the
bone is always the drawn bone. These pictures are
the gallery's own, as its check,
`native-kit-gallery`, takes them on the main menu with `--snapshot`. First
run spans the content width at every class. The compact picture is the
top of that section: the carousel, two rows of language cards, the choice
cards and Start playing, with CLOSE clear at the bottom right. The regular
and large pictures
show the carousel, the first row of language cards, the picture well and
the coach mark. The rest of First run, and the sections under it, are
below the window. The language-card pills are on the cards themselves. Keys is the last
section: keycaps, chord chips, an ANSI, ISO and JIS keyboard, a mouse
and hold rings. It sits under Badges and tags, below these pictures.

Compact at 1x, on a 640x480 window: one column, scrolling between the
header and the footer.

![The UI kit gallery at Compact on a 640x480 window: the header "Open Annihilation UI kit gallery" with "Compact · 1x", then First run. The carousel shows five tall cards, the centre one facing with an accent border, a large badge and a large name, and two turned cards on each side. Under it, language cards for Built in, Get · 3.4 MB, Downloading… and Active, and five "Start playing" buttons. A second row shows Offline, Checking… and Couldn't download beside the choice cards "Ridge pack" and "Not now". CLOSE is at the bottom right, clear of the cards. The setup window, the picture well and the coach mark are below the window.](../images/ui-kit/gallery-compact.png)

Regular at 2x, on a 1920x1080 window: two columns.

![The UI kit gallery at Regular, 2x, on a 1920x1080 window: the header with "Regular · 2x". First run spans the width. The carousel's centre card is tall, with a large badge, a large own name, the English name, "Built into the game" and a pill at the bottom, and an accent border with a one-point ring. Two turned cards sit on each side. Under the carousel, four language cards, then a picture well with the coach mark "1 Library" under it, wide enough for its words. CLOSE is at the bottom right. The rest of First run is below the window.](../images/ui-kit/gallery-regular.png)

Large at 1x, on a 1280x720 window: three columns.

![The UI kit gallery at Large on a 1280x720 window: the header with "Large · 1x". First run spans the width at the large card, 232 by 304. The centre card faces the player, with a large badge, a large own name, the facts on one line and a pill at the bottom, and an accent border with a one-point ring. Two turned cards sit on each side. Under it, four language cards, a picture well "Library, Active" and the coach mark "1 Library" with its spotlight. CLOSE is at the bottom right. The rest of First run is below the window.](../images/ui-kit/gallery-large.png)

To open it in the game: Settings › Developer › UI kit gallery, and SHOW. It
opens over the whole window, on the main menu or in a match, and Escape or
CLOSE goes back to Developer. Its samples are fixed: "Sample Mod" by "the
sample team" and the like, never a setting or a real mod, and it changes
no setting. Its test, `ui-kit-gallery`, fails when a component the kit
draws has no sample in it, so a component added to the kit shows up there
first. [src/ui/kit/README.md](../../src/ui/kit/README.md#gallery) describes
it in full.

The pictures are made again with the check's own command, over an
installed game, on SDL's dummy drivers so that no window opens:

```sh
SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
open-annihilation --game-dir "$OA_GAME_DIR" --skip-intro --mute \
    --check-kit-gallery --preferences-file gallery.conf \
    --user-folder gallery-user --snapshot gallery.ppm
```

which writes `gallery-kit-gallery-640x480.ppm`,
`gallery-kit-gallery-1280x720.ppm` and `gallery-kit-gallery-1920x1080.ppm`
beside the path given; each is stored here as a PNG of the same pixels.

## Limitations

- The kit is provisional ([above](#provisional-until-it-is-stable)).
- A kit screen opens over the front end only. Opened during a match, it has
  no place there, takes no input and shows once the front end does.
- The wheel does nothing on a kit screen.
- A Settings page takes four kinds of row: switches, drop-downs, sliders
  and text fields, each bound to a key. The registry refuses the other
  kinds of declared row (a level strip, a value with a button, buttons, a
  link, a text) on a page.
- Text is drawn left to right; the kit has no right-to-left layout.
- The only fractional scale is 150%, and only with the modern fonts on.
