# Text Rendering

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `ui.text-rendering` |
| Area | Interface (`ui`) |
| Scope | view: local display only; each player may differ |
| Runs on | Each machine, for its own view. |
| Parameters | 2 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

Game text can travel and be read as UTF-8, so chat in any language reaches
the other players, and the message log can be given the accessible chat's
backdrop: a black box under each line. The first is off and the second on
by default. How text is drawn is the player's own Language settings,
whatever the profile says. 3.1c sends and reads chat in its 8-bit code
page, with no backdrop.

![Chat messages in Latin with accents, Greek, Cyrillic, Japanese, Korean, Chinese and an emoji, each line on a black backdrop, above a chat input line holding typed text in several scripts.](images/ui.text-rendering-on.png)

*Chat in Latin with accents, Greek, Cyrillic, Japanese, Korean, Chinese and an emoji drawn correctly, each message line on the accessible chat's black backdrop; below, a typed line still open in the chat line.*

![The same chat drawn straight on the sand with no backdrop: the accented Latin line is intact, but the Greek, Cyrillic, Japanese, Korean, Chinese and emoji text has become question marks.](images/ui.text-rendering-off.png)

*The same chat with the hack off: 3.1c's 8-bit code page keeps the accented Latin but turns the Greek, Cyrillic, CJK and emoji into "?", and the messages have no backdrop.*

## Configuration example

```yaml
hacks:
  ui.text-rendering: true
```

```yaml
hacks:
  ui.text-rendering:
    unicode: true         # send and read game text as UTF-8
    chat-backdrop: false  # the message log's backdrop only as the player's setting asks
```

## Details

The hack has two switches, both local to each player's machine:

- **`unicode`**: game text holds UTF-8. The chat line and the whiteboard's
  text send what is typed as UTF-8, and well-formed UTF-8 in game text,
  such as chat from another player, is read as its characters. Off, game
  text is the game's 8-bit code page (Windows-1252), as 3.1c sends and
  reads it.
- **`chat-backdrop`**: the accessible chat. Each line of the in-game message
  log gets a black backdrop, whatever the player's Game text background
  setting says.

Both parameters are adjustable `install`: when the profile binds them under
`settings`, the player's own settings choose them. The accessible chat is
also the player's `ChatBackdrop` view setting, which starts from
`chat-backdrop`.

### How game text is drawn

The player's Language settings, in the engine's settings dialog, choose
how game text is drawn, with or without the hack:

- **Use modern fonts for game text** (on by default): the text the match
  writes in the game's fonts, and what players type and send, is drawn in
  the fonts that travel with the game (DejaVu Sans Bold and DejaVu Sans,
  then the endonym face, then Noto Emoji; a pack's ideograph face comes
  before the endonym face), hinted to whole pixels, at the
  Text size, in the game fonts' colours, each pixel the nearest colour of
  the palette in use. That is the message log and the chat line being
  typed, the bottom bar's and the map's labels, the whiteboard's labels,
  the clock and the frame statistics, and the kill board. Menu labels and
  text in the game's pictures keep the game's fonts.
- The **loading screen** keeps the game's fonts whatever the settings say:
  its few words are part of the game's look. A character a font lacks, such
  as in a player's name in a network game's load, is drawn in the modern
  fonts, as with modern fonts off.
- Off, the game's fonts draw it: each character of the game's code page a
  font has a glyph for is drawn with that glyph (hattfont12 holds 52
  letters past ASCII, CONSOLE.FNT all of Latin-1), and the modern fonts
  draw the characters the font lacks, at the font's own size, so no
  character shows as a stray byte. The labels of menus and dialogs do the
  same.
- **Text size** (80% by default, from 50% to 300% in steps of 10%) scales
  every size the modern fonts draw game text at together. At 100% they
  stand as tall as the game's fonts: DejaVu Sans Bold 14 px beside
  hattfont12 and COMIX.FNT, Bold 11 px beside hattfont11 and DejaVu Sans
  11 px beside CONSOLE.FNT and SMLFONT.FNT. Each is drawn at the size, to
  the nearest pixel, times the text's scale, and never under 7 px, so the
  smallest sizes stay readable; every size is hinted and drawn one bit a
  pixel. A character a game font lacks, drawn among its glyphs, keeps the
  font's size. With modern fonts off the slider is locked, as the game's
  own fonts have fixed sizes.
- **Font outline**, **Font shadow** and **Game text background** (on, on and
  off by default) shape the modern text: a 1-pixel outline on all eight
  sides in the game's outline grey, 43, 43, 43, or black over pixels darker
  than it (in the Full tier, over the battlefield, the graphics card holds
  each channel to the grey at the most, which is black over black ground);
  a shadow 1 pixel down and right at half strength; and a black box
  behind each line, over the rows its letters and borders take, the message
  log's from 4 pixels left of the log to 4 past each line's text. In the
  match's bars and over the battlefield every box goes down before the
  lines drawn with it, so a box never covers the line beside it: the
  unit's status keeps clear of the metal and energy figures under it, the
  clock line, the game clock and the message log of each other, and each
  overlay's lines of their neighbours'.
- Where the match's frame is drawn larger than the game's 640 by 480, the
  modern text is drawn at the larger size, its outline and shadow as many
  pixels thick, so it stays crisp. The outline and the shadow's offset
  grow with the Text size too: the text's scale times the size, to the
  nearest pixel, a half rounded down, one pixel at the least, so 2 pixels
  from 160% at 640 by 480.

### Where larger text goes

Text over the battlefield grows with the size, and what holds it follows:

- The **message log** steps from one line to the next by COMIX's 14 rows
  at the size: 11 at 80%, 28 at 200%. A line wider than the battlefield
  leaves from where its text starts is broken into rows, at its last space
  that fits, or within a word with no space; each row has its own backdrop,
  and a sender's logo stands 0.8 of a row high. When the rows do not fit
  between the log's top and the battlefield's bottom, or a row above the
  clock while it shows, the oldest lines give way to the newest.
- The **chat line** being typed is drawn at the size in the TALK field
  while the field, with two rows above and below it, holds it (to about
  120%), and shows the end of what is typed, its cursor with it, once that
  is wider than the field. A taller line leaves the field empty and rises
  over the battlefield: a black box across the battlefield, standing on
  the bottom bar, as tall as the line and two rows more above and below
  it, drawn over the log and the clock, the line in it from the column the
  field's text starts at.
- The **clock** rises with its height, so it stays two rows above the
  bottom bar.
- The whiteboard's labels, the orders' labels shown with Shift and the
  build preview's facing letter grow from where they stand.

The fixed panels, laid out for the game's fonts, keep text to their size
at most, on the font's baseline, and draw smaller sizes smaller: the top
bar's readouts, the bottom bar's unit panel and status strip, the build
buttons' captions, the digits under and over a unit's bar, the kill
board, the resource panel and its clock, wind and tidal lines, the frame
statistics, the debug keys' line, the commander placement's prompt and
Done button, and an extension's overlay.

### Text sent and received

- A typed character the code page lacks is sent as `?` without `unicode`;
  a received byte is read in the code page. With `unicode`, typed text is
  sent as UTF-8 and well-formed UTF-8 is read as its characters; a byte
  that starts no such sequence is still read in the code page, so a 3.1c
  player's accented letters show.
- A message log line keeps 63 bytes and a chat record 64, as in 3.1c; a
  UTF-8 character the cut would split is left out whole. Bytes of the code
  page are cut as before.
- While the chat line is open, the input method's composition shows after
  the typed text until it is committed, as it will be sent.
- Backspace in the chat line and in the whiteboard's text removes the whole
  last character.

### Platforms

- Every system, macOS, Windows and Linux, draws the text the same way,
  through FreeType, from the fonts in the `fonts` folder beside the game
  (in the application bundle's resources on macOS).
- Without those fonts, game text keeps the game's fonts, and a character
  they lack shows as the code page's `?`.

### Baseline (3.1c)

Text is sent, read and drawn in the game's 8-bit code page and fonts, and
the message log has no backdrop.

### Network games

Each player may set the switches differently; they are not part of the
network hash. With `unicode`, a player's chat reaches another machine as
UTF-8, which a 3.1c player, or one without `unicode`, reads in the code
page. Without it, chat goes out in the code page, as 3.1c's does.

### Interactions

- [ui.options-dialog](ui.options-dialog.md): its accessible chat switch
  changes the backdrop during a match.
- [ui.whiteboard](ui.whiteboard.md): with `unicode`, a marker's text is
  sent as UTF-8.

### Implementation notes

- The profile record is `ModProfile::ui.text_rendering` (`enabled`,
  `unicode`, `chat_backdrop`); `Runtime::game_text_utf8`
  ([src/app/runtime_game_text.cpp](../../../src/app/runtime_game_text.cpp))
  reads `unicode`, and `Runtime::chat_backdrop_shown`
  ([src/app/runtime_view_rules.cpp](../../../src/app/runtime_view_rules.cpp))
  the accessible chat.
- [src/present/include/oa/present/game_text.hpp](../../../src/present/include/oa/present/game_text.hpp)
  ([game_text.cpp](../../../src/present/src/game_text.cpp)) reads and writes
  the code page (`decode_game_text`, `encode_game_text`), maps characters to
  a game font's bytes (`FontCharacters`, `split_text`), adds the outline,
  shadow and background (`build_text_layers`, `modern_text`) and lays a
  line on 8-bit or RGB pixels reduced to the palette (`lay_text`); the
  application reaches the fonts and the settings through `GameTextHooks`.
- [src/platform/text-font](../../../src/platform/text-font/README.md)
  (`oa::platform::text_font::FontStack`) opens the bundled fonts with
  FreeType and draws a line one bit a pixel.
- `Runtime::install_game_text_hooks`, `Runtime::game_text_settings` and
  `Runtime::paint_modern_text`
  ([runtime_game_text.cpp](../../../src/app/runtime_game_text.cpp)) give the
  sizes (DejaVu Sans Bold 14 px for hattfont12 and COMIX.FNT, Bold 11 px for
  hattfont11, DejaVu Sans 11 px for CONSOLE.FNT and SMLFONT.FNT, at the Text
  size and times the text's scale, `oa::present::face_pixel_size`) and keep
  up to 512 drawn lines. `TextStyle::size` carries the Text size; a run
  split from game text the modern fonts draw whole carries it
  (`TextRun::size`), and `sized_length`, `text_border`, `modern_text_rows`
  and `modern_text_tail` in `game_text.hpp` give lengths, borders, rows and
  a line's end at a size. `Runtime::PanelText` holds the text the fixed
  panels paint to the game fonts' size (`painted_text_size`,
  `painted_baseline`), and `screen_text_size`
  ([game_text.hpp](../../../src/ui/frontend-renderer/include/oa/ui/frontend_renderer/game_text.hpp))
  does on the menus' screens.
- `Runtime::message_log_step`, `message_log_rows` and
  `message_log_most_rows`
  ([runtime_messages.cpp](../../../src/app/runtime_messages.cpp)) step,
  break and fit the message log, through `MessageLogSink::rows` and
  `most_rows` of `oa::ui::hud::draw_message_log`; `Runtime::chat_line_layers`
  and `draw_risen_chat_line`
  ([runtime_hotkeys.cpp](../../../src/app/runtime_hotkeys.cpp)) draw the chat
  line, and `console_clock_pen_row`
  ([runtime_console.cpp](../../../src/app/runtime_console.cpp)) raises the
  clock. The loading screen
  ([runtime_loading.cpp](../../../src/app/runtime_loading.cpp)) and a
  network load's player bars draw their text as interface text.
- [src/ui/frontend-renderer/include/oa/ui/frontend_renderer/game_text.hpp](../../../src/ui/frontend-renderer/include/oa/ui/frontend_renderer/game_text.hpp)
  gives each game font's characters, baseline and modern face, and splits
  game text into runs (`split_game_text`); `draw_gadget_text`
  ([gadget_draw.cpp](../../../src/ui/frontend-renderer/src/gadget_draw.cpp)),
  the gadget engine's `draw_text`
  ([gadget_text.cpp](../../../src/ui/gadget-render/src/gadget_text.cpp)),
  `Runtime::paint_text`
  ([runtime_hud.cpp](../../../src/app/runtime_hud.cpp)),
  `Runtime::overlay_gui_text` and `Runtime::draw_board_text`
  ([runtime_kill_board.cpp](../../../src/app/runtime_kill_board.cpp)) draw
  through them.
- `Runtime::draw_match_message_log`
  ([src/app/runtime_messages.cpp](../../../src/app/runtime_messages.cpp))
  lays each line's backdrop (`chat_backdrop_rect` in
  [view_rules.cpp](../../../src/app/view_rules.cpp)) while the accessible
  chat or the background setting asks for it.
- `Runtime::BackedText`
  ([runtime_match_render.cpp](../../../src/app/runtime_match_render.cpp))
  groups the lines of the bars and of each overlay over the battlefield
  while a box or a backdrop shows: each line's box is laid where it is
  drawn (`oa::ui::hud::lay_text_backing`), and every line, in the modern
  fonts or the game's, once the group ends (`oa::ui::hud::TextHold` in
  [backed_text.hpp](../../../src/ui/hud/include/oa/ui/hud/backed_text.hpp)).
- `Runtime::submit_chat_line`
  ([runtime_console.cpp](../../../src/app/runtime_console.cpp)) and the
  whiteboard ([runtime_whiteboard.cpp](../../../src/app/runtime_whiteboard.cpp))
  send typed text through `Runtime::typed_game_text`; `post_message`
  ([src/sim/messages/src/messages.cpp](../../../src/sim/messages/src/messages.cpp)),
  `net_match_say` and the battle room's chat cut lines with
  `oa::base::text::whole_characters`
  ([text.hpp](../../../src/base/text/include/oa/base/text.hpp)).
- Tests:
  - `present-game-text` (`src/present/tests/game_text_test.cpp`): the code
    page both ways, the fonts' characters, runs, borders at scales 1 and 2,
    lines laid on 8-bit and RGB pixels, the hooks, and the Text size: its
    range, lengths, the faces' pixel sizes, the borders and the message
    log's steps from 50% to 300%, and lines broken into rows or showing
    their end.
  - `ui-hud-chat-panel` (`test_message_log_rows`): a line of several rows
    takes a step for each, and the oldest lines give way to the most rows.
  - `ui-hud-backed-text` (`src/ui/hud/tests/backed_text_test.cpp`): a box
    laid apart from its line makes the pixels of the whole line; lines
    whose boxes reach over each other, as the unit's status over the rate
    figures, cover each other's letters laid in turn and keep every letter
    and every box when held; groups join, pause and close in order.
    `native-pointer-interfaces` draws the bottom bar with the background on
    and finds every line of the bars whole on its box.
  - `base-text` (`cuts_between_characters`) and `sim-messages`: lines cut
    between UTF-8 characters, code-page bytes cut as before.
  - `frontend-gadget-draw` (`test_gadget_text_game_runs`) and
    `gadget-render` (`test_text_game_runs`): a missing character drawn in
    the modern fonts on the font's baseline, the text after it, a width
    limit, game text drawn whole and labels kept in the font; game text on
    the screens drawn at the Text size and held to the game fonts' size,
    and a missing character kept at the font's size.
  - `ui-engine-settings` and `ui-engine-settings-dialog`: the Text size
    setting, its default, range, stops, keys and lock (see the settings'
    [README](../../../src/ui/engine-settings/README.md));
    `native-engine-settings` sets it through the dialog in the game.
  - `native-navigation-screens` draws the loading screen with modern fonts on at
    300% and holds it to the frame with them off.
  - `platform-text-font` and `platform-text-font-pixels`
    (`src/platform/text-font/tests/text_font_test.cpp`): the font stack's
    sizes, fallback chain, limits and a mixed line pixel for pixel.
  - `app-view-rules` (`view_settings_round_trip`, `chat_helpers` and
    `options_dialog_round_trip` in `src/app/view_rules_test.cpp`): the
    backdrop's default from the profile, its box, and the options dialog's
    switch.
  - `native-render-tiers-visual-rules`, which needs the game's data
    (`Runtime::check_visual_rule_overlays`): where the fonts open, a
    chat line in them over its backdrop is held to the
    processor's composition at zoom 1, and in the accelerated tier at zoom
    0.5, 1 and 2.5; in the Full tier what of a line lies over the
    battlefield, its shadow, outline and letter edges, is the card's,
    drawn as the line asks. `native-render-tiers-modern-fonts` runs it
    with the modern fonts on for every line of game text.
- **Known limits:**
  - Text boxes on the menu and lobby screens, the battle room's chat among
    them, take only ASCII characters as they are typed, and remove one byte
    per Backspace. Only the in-game chat line and the whiteboard's text take
    typed characters past ASCII.
  - Text in the fixed panels and on the menus' screens grows no larger
    than the game's fonts; only text over the battlefield and the chat line
    take the larger sizes.
  - With Game text background on, labels the bottom bar stacks closer
    than a modern line's height, such as a unit's rates, draw their boxes
    over part of the label above.
  - A modern run is not kerned or shaped: scripts that join or reorder
    their letters, such as Arabic and the Indic scripts, are drawn one
    character after another.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `ui.text-rendering`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `ui.text-rendering: true` | On, every parameter at its default. |
| `ui.text-rendering: {unicode: false}` | On, the parameters named set and the rest at their defaults. |
| `ui.text-rendering: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `ui.text-rendering: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `unicode` | `bool` | - | `true`, `false` | - | `false` | `false` | `install` | `view` | - |
| `chat-backdrop` | `bool` | - | `true`, `false` | - | `false` | `true` | `install` | `view` | - |

Adjustable: `install`: the player's settings may set it when the profile binds it under `settings`.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{unicode: false, chat-backdrop: false}` |

`baseline` sets every parameter to its 3.1c value: the hack is on and plays as 3.1c.

### Shorthand

This hack takes no shorthand: write `true`, `false` or a parameter map.

### Every parameter at its default

```yaml
hacks:
  ui.text-rendering:
    unicode: false
    chat-backdrop: true
```
<!-- END GENERATED: schema -->
