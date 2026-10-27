# OA UI kit

The OA UI kit (`oa/ui/kit`, `oa::ui::kit`, the library `oa-ui-kit`): one
look, one scale and one input model for Open Annihilation's own screens.
Settings and its notices and prompts, the Library, the map browser, the
download check, the Add registry window, the battle room's card, the Game
files screen and the folder chooser are built from it, and from version 15
of the extension table an extension builds its Settings pages, kit
screens, notices, prompts and match-overlay components from it too
([docs/extensions/ui-kit.md](../../../docs/extensions/ui-kit.md)). It lays
a screen out in points at a size class, as one display list of what is
drawn and which controls a press reaches; draws that list on a canvas at a
whole scale or at 150%; and turns a pointer, a finger, the keys and the
wheel into what a screen does, with every control named for automation.
Nothing in it reads or writes the preferences file, and it never looks a
word up: a caller passes the text to show. It is provisional until it is
declared stable (D31): its headers change whenever a screen needs them to
([Limitations](#limitations)).

## Entry points

Every header below is public, and each is part of the extension table's
contract from version 15 (`src/app/include/oa/app/extension.hpp`).

- `theme.hpp`: Kit 2's tokens and scales, and the metrics of the three
  size classes worked out from them.
  - The colours (`colour`): a ramp of five surfaces, the bars and wells
    (`band`, s0 121410), panels and lists (`list`, s1 171a15), raised
    surfaces such as the dialog and cards (`panel`, s2 1d2119), hover
    (`hover`, s3 252a21) and selected (`list_selected`, s4 2c3227); the
    rule, the control border and the strong edge (`strong_edge`, 4f5747);
    three text tones, primary (`text`, e7e8df), secondary
    (`text_secondary`, b9bfae) and tertiary (`text_tertiary`, 959c8b,
    which `hint` and `quiet` are); the lime accent with its light (the
    focus ring, `focus`) and held tones and its dark ink; caution
    (`lock`, e0b04f), the lifted red (`danger`, e8806f), the information
    blue (`information`, 7db3de, which `online` is) and the brand gold
    (`brand_light`, `brand`, `brand_shade`), worn only by the OA mark; the
    tinted fills of badges and chips (`ok_tint`, `caution_tint`,
    `danger_tint`, `information_tint`, `brand_tint`: each colour laid over
    the raised surface at `badge_tint`, 16 percent, by `mixed`) and a
    focused field's soft ring (`field_ring`); and the keys' category tints,
    kept. The Game files screen's and the folder chooser's tokens
    (`screen_colour`) are the dialog's: the raised surface behind, the
    hover surface for cards, and the dialog's text, tertiary text, line,
    lime, amber, red, ink and button text, each a value of its own. The touch controls keep their own (`hud_colour`).
  - The type scale (`TextLevel`: title, label, body and small) and its
    sizes in each class (`Metrics::title_size` and the rest,
    `text_size`); the classic look, the game's bitmap fonts at their two
    sizes, draws a title and a label in the regular font and body and
    small text in the small one (`classic_font`, in `text.hpp`). The
    modern look (`Fonts::modern`, below) draws each font's text at the
    steps of the class its fonts are handed for (`Fonts::size_class`):
    the regular font's as a label, Barlow SemiBold at 14, 15 and 16
    points at Compact, Regular and Large, the small font's as small text,
    Barlow Regular at 12, 12 and 13, and a tracked line in Barlow
    Condensed, the regular font's Bold at the title's 17, 20 and 24 and the
    small font's SemiBold at the small text's size (`modern_type`). The
    classic look's fonts keep one size at every class.
  - The spacing scale: a base of 4, 6 and 8 points (`space`), the window
    padding three of it (`window_padding`), rows a space and a half apart
    (`row_gap`) and sections three apart (`section_gap`).
  - The radii: keycaps, controls, cards, windows and dialogs, each class's
    own (`keycap_radius` to `dialog_radius`); `control_radius_for` gives a
    control's from its height and `keycap_radius_for` a keycap's.
  - `compact_metrics`, `regular_metrics` and `large_metrics`: each class's
    steps on Kit 2's scales (`ClassScale`, `scaled`) and Kit 2's control
    sizes (`control_height` 28, 34 and 38 points, `row_height` 36, 40 and
    44, `text_field_height` 30, 36 and 40, `filter_chip_height` 24, 26 and
    28, `tab_strip_height` 32, 38 and 44, a window's header and footer
    bands of 36 and 48 points in every class); then the sizes the screens
    are laid out with until each moves to Kit 2's, the settings dialog's,
    its notices' and its prompts': 0.7.3's at Compact, Regular and Large
    giving the dialog, its notices and its prompts more room (their sizes,
    the padding, the nav list's width and the characters a host's line
    under a row holds) and keeping every other size Compact's. The
    screens' own layouts (Settings, the Library, the key editor, the
    pickers) are built on those sizes and do not hold Kit 2's yet: each
    moves to Kit 2's sizes as its shell is rebuilt.
- `text.hpp`: the game's two text faces, their width, drawing a line and a
  boxed line, and the one wrap; `fitting_start`, where the wrap breaks a word
  wider than a line; `path_break`, a `WordBreak` that breaks a folder's path
  wider than a line after its last separator that fits (the folder
  chooser's texts break so); `fit`, a line shortened to a width with the
  `ellipsis`;
  and, for a screen drawn in the modern fonts, `TextMeasureHooks` (the app
  measures with the bundled fonts) with `measured_width` and
  `measured_line`, which estimate what a null hook leaves out. The look:
  `Fonts::modern`, the bundled font stack, set by the host while the
  player's Look is Modern and null in the classic look (`modern_look`),
  and `Fonts::size_class`, the class of the screens the fonts are handed
  to, which the host sets as it lays them out (the OA layer sets the
  class its screens take; Compact unless set). In the modern look every
  text is measured and drawn in Barlow from that stack at its role's type
  at that class (`modern_type`), what Barlow lacks in the faces
  after it (DejaVu Sans for Greek and Cyrillic, drawn whole for a line in
  those alphabets, the endonym face and a language pack's face for
  ideographs, Noto Emoji): `modern_width` is a line's width in points, each
  character's advance at 1x and the tracking; `modern_line` draws it at a
  scale, each character at its 1x pen times the scale, so a line is as wide
  at every scale as its measure; `modern_capitals` gives its capitals' rows.
  `text_width`, `tracked_width`, `capitals`, `draw_text` and
  `draw_boxed_text` take that path, the text antialiased with no outline or
  shadow, and `with_stand_ins` leaves the text as it is.
- `layout.hpp`: points, the Auto scale and the three size classes. A
  room's class is the largest whose settings dialog fits it with
  `class_margin` (24) points round it (`size_class_of`): Regular from
  `regular_from` (768 by 534 points), Large from `large_from` (1008 by
  648), Compact below, both sides reaching the class; a class the player
  chooses is held to a room by `size_class_within`, kept wherever its
  dialog fits whole. `layer_viewport` gives the scale an Interface size
  gives a canvas, held to what fits Compact's dialog: a whole step, or
  150%, `fractional_scale_percent`, where Compact fits at one and a half;
  Auto passes 0, or `auto_with_fraction_percent` while 150% is offered,
  and takes one step down from its scale where that gives the room the
  `wanted` class (Large unless the player chose a window size), so 1920
  by 1080 is Large at 1x. `metrics_of` gives a class's metrics, which a
  screen takes its window's sizes from; `to_canvas` (points as canvas pixels, each edge rounded to the
  nearest pixel on its own), rows, columns, grids, splits and scroll
  areas, and the display list, whose `Role`s are listed in `all_roles`, in
  their order, with `canvas_draws` telling which `paint` draws (every role
  but those of a screen drawn in the modern fonts). A screen drawn in the modern fonts (the Game files screen and the
  folder chooser) lays its parts out in canvas pixels at a scale that may
  be a fraction (`Typesetter`, `canvas_pixels`, `canvas_font`) with:
  - `text_part`, a text wrapped to a room in a `Lettering` (size, weight,
    colour and `TextStyle`), at most so many lines, the last ending with an
    ellipsis, as wide as the room or its widest line (`TextFit`), a word
    wider than the room breaking where a `WordBreak` such as `path_break`
    says;
  - `line_part`, one line fitted to a room and centred on a row;
  - `button_span` and `button_part`, a button of a `ButtonSpec` (its label,
    look, `Glyph` and control), and `button_row`, its left and right groups
    on one line or wrapped from the left (`ButtonRowSizes`);
  - `icon_part` and `plain_part`, a glyph in a box and a part with no text;
  - `move_parts`, parts moved down and appended, clipped or not;
  - `scroll_column`, a column of a fixed top, a body and a fixed bottom
    placed in an area: one after another, else the body scrolling between
    the others, else the whole column scrolling, as a `ScrollArea`;
  - `first_live_part` and `list_part_controls`, the controls of the parts
    above the last backdrop, the top one first, and the Tab order.

  `hit` takes a `DisabledControl` rule for a disabled control a point lies
  in: it `passes` the point on to the controls under it (the rule without
  one, the Game files screen's), or `blocks` it, and the point reaches no
  control (the folder chooser's).
- `input.hpp`: a pointer and a finger's reach (in whole points, or in a
  fraction of a point for a screen at a fractional scale, and by the
  `DisabledControl` rule, a disabled control the finger is on blocking the
  press and every search), `mark_states` (the focused and held parts of an
  `Interaction`), the keys, Tab in a declared order, the arrows by where
  controls sit (`focus_toward`; given the scroll areas, judged with them
  laid out whole), the wheel, and a text
  field's editing: `insert_text` for typed text and `edit_text` for Backspace,
  Delete, Left, Right, Home and End. For automation, `automation_entries`
  lists the named controls, and `automation_parts` lists each control
  followed by the parts of it that take a press of their own (an
  `AutomationEntry` with `part` set): a switch's halves `<name>.off` and
  `<name>.on`, a strip's levels where `level_at` finds them, a row of
  buttons' buttons, and an open drop-down's items at `choice_item`, each
  named by its control's `Control::parts` word, else its place from 1. A
  control's rectangle there is the part of it a press reaches. The
  endpoint lists them after `oa.` (docs/automation.md).
- `looks.hpp`: how a control looks. A look owns its texts. A text field's
  state, `TextField`, is its text and its caret. A picture item draws its
  `PictureLook`'s picture, such as a mod's badge, or else the canvas's icon.
  `StateIcon` names the state icons: a circled check, a circled up-arrow,
  a download arrow, a key, and the two maker's marks (the OA badge and
  the bone). `MakerMark` on a list row draws one of those marks under the
  row's state icon. `TagIcon` (`tag_icon.hpp`, which `looks.hpp` includes)
  names the eight catalogue tag icons, `statistics`, `economy`,
  `commanders`, `new-units`, `disable`, `gameplay`, `ai` and `maps`.
  `tag_icon_of` maps an id exactly, lower-case kebab-case, and any other
  id gives none; the kit holds no tag label. `BadgeLook` is a package
  badge: a picture, else a tag icon on `face`, else letters, else a dashed
  square. `GripLook` is a drag handle, `grip_width` by `grip_height`.
  `PillLook::tag` is a tag icon at the pill's left. `ListRowLook::badge_tag`
  draws that icon on the badge, and `by_colour` colours the byline.
- `canvas.hpp`: the canvas the components draw on, `Canvas`, in its two
  forms, and the primitives every draw function draws through: `fill_rect`,
  `blend_rect`, `outline_rect`, `bevel_rect`, `rule_rect`, `box_text`,
  `measure_text`, `draw_picture`, `draw_mark`, `clipped` and `placed_at`;
  and, for a shape worked out at the canvas's own pixels, `covered_pixels`
  (how many pixels a rectangle covers on either form) and `blend_coverage`
  (a colour blended through a map of one byte a pixel). The rounded
  rectangles, `fill_corner_rect`, `blend_corner_rect` and
  `ring_corner_rect`, work out each corner from 16 samples a pixel;
  `corner_coverage` gives one corner's share of each pixel, filled or as a
  ring, smooth or stepped, with which the OA layer rounds a window's
  corners. `perspective_blit`
  draws a laid-out rectangle into one reused offscreen buffer and copies it
  onto the canvas as a trapezoid: one vertical edge keeps its height and the
  other is shorter, so the rectangle looks turned about its vertical axis,
  at an opacity. On the crisp form each destination pixel copies the one
  nearest source pixel. On the smooth form each destination pixel blends
  the source pixels around its sample. The sample runs from the source's
  first pixel at the trapezoid's first pixel toward one past the last
  pixel at the far edge, so a sample between two pixels mixes them by the
  distance. `scale_blit` is that copy with no turn. The buffer is
  kept for the thread, grown and never replaced, and a drawing that is
  already using it draws nothing if it asks for the buffer again.
- `components.hpp`: the controls, drawn through the canvas's primitives on
  either form. One button, six looks, every one rounded. `draw_state_icon` draws a state
  icon, filling a square of a rectangle's shorter side. A circled check,
  a circled up-arrow, a download arrow and a key are one colour: their
  rings and strokes, from a design of 24 units (the ring 2 units wide, its
  outer edge a unit in, every stroke 2.5 units wide with round ends), are
  worked out at the canvas's own pixels, each pixel's share counted from
  16 samples in integers, and drawn through `blend_coverage`, so the icon
  keeps its edges at 1x, 2x, 3x and 150% alike and never scales a picture
  or a one-bit mark. The bone is a filled shape sampled the same way, in
  its own colours, and ignores the colour passed in: off-white with a dark
  outline. The OA badge is the app icon when `set_open_annihilation_mark`
  has been given one, drawn through `draw_picture` from the copy kept for
  the mark's size, and otherwise that same kind of shape: a rounded plate
  with the letters in the lock colour. `draw_tag_icon` draws a tag icon
  the same way, strokes 2.5 units wide on the 24-unit square, sampled at
  the canvas's own pixels. `paint` draws a display list. The
  OA button (`draw_oa_button`, `OaButtonLook`) may wear the kit's attention
  dot, as the main menu's does while updates wait: a disc
  `oa_button_dot_side` (7) points across in the warning token
  (`colour::lock`), its outer point a ring in the outline token
  (`colour::control_border`), `oa_button_dot_inset` (2) points in from the
  button's top and right edges (`oa_button_dot`), at the canvas's scale.
  It may also wear a badge in the bottom right (`OaButtonMark`,
  `oa_button_badge`): the count, "999+" above 999, in the small font on a
  dark pill, or a spinner of eight places in that pill while the count is
  not final. The spinner is worked out at the canvas's own pixels, each
  pixel's share counted from 16 samples in integers, and drawn through
  `blend_coverage`. `turn` names which place is lit. One full turn is
  `refresh_revolution_ms` (1.6 seconds), the same period as a refresh mark,
  measured in time. The badge does not
  move when the button is pressed, and a button too small to hold it draws
  none. Without the dot and without a corner mark the button is drawn
  exactly as before. `draw_menu_icon_button` is the main menu's button
  at every size: the icon itself, filtered into the square, its clear
  corners left clear, with no panel and no bevel. Hover and focus draw a
  ring in the accent that follows the icon's corner
  (`menu_icon_corner_256`, 56/256 of the side); a press draws that ring in
  the held accent and shifts the icon by one point. The dot and the badge
  are the same as on `draw_oa_button`. A radio (`RadioLook`, `draw_radio`,
  `add_radio`, role `radio`) is one of a set of choices of which one is
  chosen. Alone it is the mark, the largest square of its area: a
  one-point ring in the idle switch grey, the accent while chosen (lighter
  hovered, deeper held, the hovered border's grey disabled) with the
  accent's dot half the ring across in it. With a caption it is a radio
  row: the mark half the row's height, as far in from the left as from the
  top, the caption in the regular font beside it, on a box with the
  control radius of its height, the selected surface with the accent's
  edge while chosen and the hover face while hovered. `radio_parts` gives
  the mark and the caption's box the caller fits its caption to.
  `add_radio` adds a button whose `checked` is the radio's state, or with
  `no_control` the mark alone, for a card that is the control itself.
- `chrome.hpp`: the window's face and edge, its header and footer band,
  the nav list, and a section's heading, row, rule and locked fade. A
  header's band is `HeaderLook::height` points high, Kit 2's
  `window_header_height` (36) where a screen asks for it, and its rule, the
  mark, the title, the second word, the version and the note sit in its
  middle; 0 is Compact's `header_height` (26), drawn exactly as before.
- `controls.hpp`: the controls of the Library, the map browser and the
  battle room's card:
  - a list row, in its full form (`list_row_height`, a 28-point badge) and
    its short form (`short_list_row_height`, the Mods row's), with a badge
    picture, letters or a blank badge, a title and its byline, lines cut
    with "...", an aside, a chip and tags;
  - chips in seven states, each a pill: get (outlined), installed, update,
    online and a problem (their colour's tinted fill under words in that
    colour), playing (the accent filled) and a filter (outlined, on the
    selected surface with the accent's edge while it is on), each
    selectable, hovered and disabled, and `chip_width`. A chip with a state icon (`ChipLook::icon`) draws only
    the icon, `state_icon_side` (14) points square, in its state's colour
    (the hint for get, the accent for installed and playing, the lock
    colour for update), in `control_hover`'s grey while disabled, on the
    hover face while hovered, and keeps its text for automation. A row
    places it as it places a chip (`list_row_chip_rect`); a screen whose
    row icon takes a press adds a control of its own there, before the
    row's. A row's aside is as wide as its widest entry by the canvas's
    measure and by the game's fonts', so a version such as "4.8 · rev 3"
    is never cut on either form;
  - a pill (`PillLook`, `pill_width`, `pill_height`, `draw_pill`,
    `add_pill`): a small-font word on a fully rounded ground
    `chip_height` (12) points high. The word keeps `chip_padding` (4 points
    at 1x) clear of each semicircle, and an empty pill stays a circle.
    A pill is a status badge. Playing and active fill the accent and
    write `on_accent`; neutral and inactive fill the selected surface and
    write the secondary text, for "Built in" and for a filter that is off;
    installed, update, problem, information and brand fill their colour's
    tint and write in that colour. Hover lightens an accent ground and
    outlines any other in the hovered border's grey; focus is the focus
    ring round the pill; disabled is the hover surface under the idle
    colour and ignores hover and focus. The ends are stepped at 1x on the
    crisp form and smooth at 150%. A tag icon at the left adds the same
    square and gap as the state icon at the right, and a pill with neither
    is unchanged;
  - a text field, and with the magnifier the search field, its text moved
    so its caret shows;
  - tabs with a count, `tab_rects`;
  - cards with a square preview or a placeholder, chips on the preview, a
    title and a subtitle, greyed or not, `card_height` and `card_cells`, a
    grid of as many columns as fit or of a fixed number;
  - a hover card with the OA mark (the logo when the canvas carries the icon,
    otherwise the one-bit mark), a title and an aside, label and value
    rows, a quiet block and an arrow from any edge, `hover_card_size`,
    `hover_card_rect` and `hover_card_due`, which says when the pointer has
    rested on one control for `hover_card_delay_ms` (400);
  - a link, `link_width`;
  - a package badge (`BadgeLook`, `draw_badge`, `add_badge`, role
    `package_badge`): the picture with a `control_border` outline, else
    the tag icon in `on_accent` on `face` at 70 percent of the side, else
    the letters cut to whole letters, else a dashed square. A list row's
    badge is this drawing;
  - a drag handle (`GripLook`, `draw_grip`, `add_grip`, role `grip`):
    two columns of four discs, 3 points apart and 1 point in radius,
    `grip_width` (6) by `grip_height` (12). Rest is `quiet`, hovered the
    text colour, held the accent, disabled `control_hover`;
  - a keycap (`KeycapLook`, `draw_keycap`, `add_keycap`, role `keycap`,
    `keycap_width`): 16 points high and at least 16 wide, a label, an
    optional short name, a category tint and its letter, and a mark.
    Changed is a filled diamond, clash a triangle with a dashed danger
    border, unbound a hollow ring, fixed an open square with a dashed
    control border. A held key fills with the accent. `small` is the
    9-point badge, its label in the accent on `on_accent`. The tint's
    face, darker bottom edge and ink are `tint_face`, `tint_edge` and
    `tint_ink` (`colour::category_orders` through `category_developer`);
  - a chord chip (`ChordChipLook`, `draw_chord_chip`, `add_chord_chip`,
    role `chord_chip`, `chord_chip_width`): keycaps joined by "+" and a
    gesture word, or the dashed "+ Add key" form, or quiet unbound text;
  - a keyboard picture (`KeyboardLook`, `draw_keyboard`, `add_keyboard`,
    role `keyboard`): ANSI, ISO or JIS from `keyboard_places`, 91 by 25
    quarter units, one control a key named by `code_word`. With
    `KeyboardLook::main_block` it draws the main block alone, the rows from
    Tab, Caps Lock and Shift to Enter and Shift (`keyboard_main_block`, 60
    by 12 quarter units), scaled to its rectangle on their own, so a narrow
    picture keeps keys large enough for their legends. No legend is cut,
    in either picture: one too wide for the regular font is drawn in the
    small font when that fits it whole, a point nearer the key's sides
    where it needs that, and a named key whose legend still does not fit
    takes its geometry's own label (Ent for a long Enter), then a shorter
    one (Cap for Caps, PU for PgUp, PS for Prt), or none.
    `keyboard_size(look)` gives either picture's size and `keyboard_draws`
    tells whether it draws a key; each key keeps its control;
  - a mouse picture (`MouseLook`, `draw_mouse`, `add_mouse`, role
    `mouse_picture`): the body, the buttons and the wheel, and the extra
    buttons when the look asks;
  - a hold ring (`HoldRingLook`, `draw_hold_ring`, `add_hold_ring`, role
    `hold_ring`): filled clockwise from the top, in quarters while
    `reduced_motion` is set.

  Each has a draw function and an `add_*` that puts it in a display list
  with its name, kind and Tab place; `paint` draws them.
- `components_more.hpp`: the notice and the question (the settings
  dialog's notice and prompt): their models, placement at their size class
  (`Notice::size_class`, `Question::size_class`), display lists, pointer,
  finger and key events and drawing, and the progress bar. A
  notice's and a question's `word` (`notice` and `prompt` unless the code
  that raises it gives its own, as the missing-language question's
  `language-notice`) starts its controls' names: a notice's `<word>.ok` and
  `<word>.open`, a question's buttons `<word>.<id>` by their
  `QuestionButton::id`, or `<word>.button-<n>` from 1 without one, and
  both's text `<word>.body`, a control of kind `area` that is neither
  enabled nor focusable, so that no press, finger or key reaches it, whose
  text is the title, the paragraphs and the failure, one a line. A question's
  `items` (`QuestionItem`) are lines under the paragraphs: a 20-point badge,
  the name and an aside in the hint colour, cut with the kit's ellipsis.
  Each item's name and aside is a line of `<word>.body` after the paragraphs.
  With no items the question is laid out and drawn as before. The work
  dialog is the same panel with no buttons: a title, one line and either a
  bar (`progress` of 0 to `question_progress_whole`) or a spinner
  (`progress` of -1, `work_spinner_side`, `draw_spinner`). Its word is
  `progress` and its one control is `<progress>.body`, the same kind of
  area. The spinner is drawn directly and is not a role of the display list.
  A question's `parts` (`QuestionPart`) follow its paragraphs and items in
  order: a paragraph in the small font in the secondary colour, a callout
  tinted in its tone (`CalloutTone`: ok, caution, danger, information)
  with its mark, or one box across or two side by side (`QuestionBox`), a
  heading over lines each marked done, failed, a bullet or nothing
  (`LineMark`); a part the height leaves no room for is left out whole.
  With no parts the question is laid out and drawn as before. A
  `QuestionButton` may be `quiet`, a caption with no face at the footer's
  left, or `danger`, outlined in red, and `Question::progress_paused`
  draws the bar's fill in the lock colour. The update pill (`UpdatePill`,
  `place_update_pill`, `update_pill_list`, `draw_update_pill`,
  `update_pill_pointer`) is the update centre's state in short: a mark
  (`UpdatePillIcon`), the words in the small font, a thin bar under them
  while a download is under way, amber while paused, and one small button,
  on a raised face with the strong edge; automation names it
  `update-pill.open` and its button `update-pill.<id>`.
- `rows.hpp`: declared rows. A settings page is a table of `RowSpec`s,
  each made by a factory that takes the row's id first: `toggle`, `choice`,
  `slider`, `levels`, `value_and_button`, `buttons`, `text_field`, `link`
  and `text`, bound to a model's fields or to its get and set functions:

  ```cpp
  constexpr std::array<kit::RowSpec<Downloads>, 3> kDownloadsRows{
      kit::choice("check-updates", "Check for content updates", &Downloads::check, kCheckCaptions),
      kit::toggle("install-id-on", "Install ID", &Downloads::install_id_on, {"A random number."}),
      kit::value_and_button("install-id", "", install_id_text, "RESET", "reset", kResetId),
  };
  ```

  A row bound to a preference is a `PreferenceRow` (`RowSpec<PreferenceModel>`).
  `preference_toggle`, `preference_choice`, `preference_slider` and
  `preference_text_field` take the row's id, its label, its key, its default
  text and its hint; a choice also takes its choices' ids and captions, a
  slider its range and step. The model holds an
  `oa::platform::preferences::Values`. A missing or malformed stored value
  reads as the default. A toggle stores `1` or `0`, a choice one of its
  choices' ids, a slider a decimal on its step, and a text field the player's
  text up to 256 bytes. A key is dotted words of a to z, 0 to 9 and hyphens,
  at least two words, at most 64 bytes, and it does not start with
  `open-annihilation.`. `preference_key_problem` names a key that is refused.

  `view_of` reads a row's `RowView` from its spec and the model through the
  caller's text function; `place_rows` places a column of views,
  `rows_scroll` and `scroll` scroll it, and `add_rows` lists what the rows
  draw and their controls, named `<prefix>.<id>`. A drop-down's items and a
  strip's levels have ids too, beside their captions: an index field's
  `choice_ids` (the `choice` factory that takes them) and a stepper's `id`
  function (`row_choice_id` reads either). `add_rows` gives a control its
  levels' or items' ids, or a row of buttons' `button_ids`, as its parts'
  words, which automation names its parts by. `step`, `activate`,
  `press`, `drag`, `choose`, `type` and `edit` change the model as a row's
  control is used.

- `first_run.hpp`: the pieces a first-run flow is built from. The words
  arrive already looked up. Reduced motion is `Frame::reduced_motion`, off
  until a later Settings row sets it. The kit does not read preferences: a
  screen copies the flag onto the look, and the draw functions read the look.
  - A language card (`draw_language_card`, `language_card_layout`,
    `add_language_card`): a tall card. The badge is centred at the top
    (the pack's picture, or the language's letters on the hint grey,
    accent while the card is active). Under it, centred, the language's
    own name, large and shrunk until it fits, its English name, a facts
    line and a pill centred at the bottom. A facts line wider than the
    card wraps onto the next small line, above the pill, so the whole
    line is shown. An active card, and the carousel's chosen card once it
    faces the player (`CarouselLook::chosen`; with none, the centre card),
    have an accent border and a one-point accent ring outside it. The
    pill follows `LanguageAvailability`. Built in, get, downloading and
    checking are neutral; active uses the accent pill and an accent badge;
    a failed fetch uses the problem pill; offline is neutral and the card
    is dimmed. Hovered, held and disabled change the face. A progress bar,
    when the layout leaves room for one, sits under the card and is the
    kit's own progress bar, not part of the card.
  - A carousel (`draw_carousel`, `carousel_fit`, `carousel_places`,
    `carousel_go`, `carousel_tick`, `carousel_key`, `carousel_command`,
    `carousel_pointer`): cards in a ring. The centre card faces the player.
    Each step out turns 18°, falls back and shrinks. A card stays fully
    opaque through two steps, so its name can be read and a floor drawn
    behind the carousel does not show through the face, and fades out over
    the third step. Two cards show on each side. A move takes 0.5 s and
    eases out. The place at a time is the same however many frames drew it.
    `carousel_settle` puts the centre on the selected card with no move, for
    the moment the carousel is first shown. Reduced motion slides the cards
    flat. The arrows are the caller's
    buttons. A pager of one bar a card lights the bar nearest the centre.
    Left and Right move one card, Enter on the centre picks it, a press on
    a side card centres it, and a swipe of 24 points or more moves one
    card. Shoulders and a stick are not keys: the host fills a
    `CarouselCommand` and `carousel_command` applies it. Each card and each
    arrow is a control.
  - A step indicator (`draw_step_indicator`, `step_indicator_width`): the
    step's name and "N OF M" in the small font, letter-spaced, and one
    segment a step, lit from the first up to the current one.
  - The setup window (`draw_setup_chrome`, `setup_buttons`): the kit
    window's header (the mark, "Open Annihilation", a muted page word and
    the step indicator at the right; without `SetupChromeLook::brand`, the
    page word at the left and the step indicator) and its footer (Skip
    setup, a quiet button on the left, then Back and an accent Next). It
    spans the window and is drawn from the canvas's origin.
  - A choice card (`draw_choice_card`, `choice_step`): a radio row with a
    title and a hint, idle, hovered, held, chosen and disabled. Up and Left
    move to the previous row of a group and Down and Right to the next,
    held inside the group.
  - A coach mark (`draw_coach_mark`, `place_coach_mark`) and its spotlight
    (`draw_spotlight`): a numbered card with an accent border and a
    one-point accent connector to a target rectangle. The card is placed to
    the right of the target, then the left, then below, then above, with a
    gap of 8 points, so it stays in the room and never covers the target.
    The spotlight dims the layer to 74 percent of the backdrop except a
    cut-out around the target, which keeps an accent ring.
  - A picture well (`draw_picture_well`, `picture_well_layout`): a one-point
    border, a 6-point inset the caller draws into, scaled with `scale_blit`,
    and a caption under it. The kit ships no picture for it. A sample may
    fill the inset with a stand-in pattern.
  - A call to action (`draw_call_to_action`): an accent button for a flow's
    last step. It brightens to the light accent and sends out a ring every
    1.8 s, from the time. Reduced motion holds a steady ring. Disabled, it
    is the disabled button and sends no ring.

- `gallery.hpp`: the UI kit gallery (see [Gallery](#gallery)):
  `GalleryModel`, `gallery_layout`, `draw_gallery`, `gallery_act` and
  `gallery_text_field`, and its fixed samples' `gallery_version` and
  `gallery_languages`.

### The parts of a screen drawn in the modern fonts

The display list's `Item` has what such a screen draws besides the crisp
canvas's fields: `lines` (its text, already wrapped), `pixel_size` and
`bold`, `style` (a `TextStyle`: header, title, subtitle, lead, body, small,
row title, row detail, button or mark text), `glyph` (a `Glyph`: folder,
device, disk, clock, warning, check, cross, dash, info, stop, play, trash,
refresh, plus, file or the OA mark) and `glyph_size`, and `fraction` (a
progress fill's share). Its colour is one of the screen's tokens
(`screen_colour`), and its clip the scrolled view. The roles such a screen
draws besides `text`, `button` and `toggle`, from those fields rather than a
look, are `header_bar`, `badge`, `header_text`, `version`, `title`, `panel`
(a card or a panel of rows; the kit's `card` is a grid's card, drawn from its
`CardLook`), `icon`, `banner`, `row`, `divider`, `button_main`,
`button_danger`, `progress_track`, `progress_fill` (the kit's `progress` is a
whole bar drawn from its `ProgressLook`), `progress_busy`, `backdrop` and
`sheet`. Their painter is the screen's own; `paint` draws nothing for them on either
form of the canvas.

## The canvas's two forms

A `Canvas` draws in one of two forms, and the components keep one set of
draw functions for both: each draws through the canvas's primitives, and
only the primitives know the form.

- The crisp form, a canvas with a `surface`, draws with the artless painter
  at a whole scale (`placement.scale`) in the game's own fonts, the modern
  faces drawing only the characters they lack: the classic look; or, in the
  modern look, in Barlow (`modern_line`). The OA layer draws with it at
  100%, 200%, 300% and 400%.
- The smooth form, a canvas whose `smooth` has a painter, draws with the
  painter of `oa/ui/paint` into an RGBA canvas at a scale in percent
  (`SmoothCanvas::scale_percent`), in the bundled modern fonts
  (`SmoothCanvas::fonts`). The OA layer draws with it at 150%, which the
  game's bitmap fonts cannot take: they are never scaled by a fraction.

Places stay in points on both. On the smooth form a point's edge is
`lround(point * scale)` (`to_canvas`, a half away from zero), each edge of
a rectangle rounded on its own, so a fill covers whole pixels, two fills that
share an edge in points share it in pixels, and a 10 by 10 point fill at
1.5x covers 15 by 15 pixels. An outline, a raised edge and a rule are
`max(1, lround(points * scale))` whole pixels thick, two at 1.5x, from
their first edge. A one-bit mark is a fill of each set point. A coverage
map is one byte a pixel the form draws at: on the crisp form a rectangle's
points times the whole scale, on the smooth form the pixels its rounded
edges cover; so a state icon is worked out at 2x or 1.5x as finely as the
pixels allow, never scaled up from 1x. Text is drawn
antialiased at `lround(smooth_text_points * scale)` pixels per em: 12 points
for the regular font, drawn bold as the game's heavy title, label and value
font is, and 10 for the small one, so 18 and 15 pixels at 1.5x; tracking is
`lround(tracking * scale)` pixels. Its capitals are centred on its box's
height as the game font's are. A text wider than its box at that size keeps
its size; the kit records it (text_fit_records) so a check can name the
screen. Recording is off in a release build.
`measure_text` gives the smooth width in points, rounded up. A picture,
such as the Open Annihilation icon, is drawn filtered, never by the nearest
pixel. A clip is the clipped rectangle's pixels, and `placed_at` moves the
origin in points.

The layout, the hit tests and the display list are the same on both forms:
the look's fonts measure the texts in points whichever form draws them, the
game's fonts in the classic look and Barlow at 1x in the modern one. In the
modern look the smooth form draws Barlow at the type's size times the
scale, each character at its 1x pen times the scale, and `measure_text`
gives the 1x measure.

## Gallery

The gallery shows every component of the kit at the scale and size class of
its frame, once working on its own model and once in each fixed state, so
that a change to a shared component shows up there, and in its one golden
frame for each class, before it shows up across the screens. The extension
chapter shows it at Compact, Regular and Large, in the pictures its check
takes ([docs/extensions/ui-kit.md](../../../docs/extensions/ui-kit.md#the-gallery)).

- **What it shows.** One window over the whole frame: the face, the header
  (the OA mark, "Open Annihilation", "UI kit gallery" and the class and
  scale, such as "Large · 1x", in the lock colour), the footer band with
  CLOSE, and the edge. Between them the sections, each under its heading
  and a rule: First run (a carousel of language cards, turned, with an
  arrow on each side and a pager; language cards in each pill state; choice
  cards; the setup window with Skip setup, Back and Next; a coach mark and
  the spotlight under it; a picture well; a call to action; and a step
  indicator), Window chrome (an OA window over a darkened screen, its
  header saying `v1.2.3`, never the build's version), Tabs, Buttons (the
  six looks at Kit 2's control height, a link), Toggle (a switch, then radios not chosen and chosen in each state and a pair of radio rows), Level strip, Drop-down (one shown open with
  an aside), Slider, Search field (and a plain text field), Chips (every
  state), Pills (playing, neutral, installed, update, problem, information
  and brand, each a word on a rounded ground and none taking a press),
  Progress (waiting, part done, paused and done), Nav list,
  List rows, Scroll list, Card grid, Hover card,
  Section, label and hint, Lock, Notice, Question, OA mark (the OA
  button with and without its dot, the mark alone, the icon and every
  one-bit mark), Badges and tags (the eight tag icons, a picture,
  letters and a blank badge, a pill and a nav entry with a tag, a drag
  handle at rest, hovered, held and disabled, and a question listing
  Gatehouse) and Keys (a working keycap, every state and category tint
  with its letter, a modifier, two small badges, chord chips, an ANSI,
  ISO and JIS keyboard, a mouse, and hold rings at 0, 1/3, 2/3 and whole,
  steady and in quarters). They flow into the shortest column: one at Compact, two
  at Regular, three at Large, and scroll between the header and the footer
  where they are taller than it. A notice, a question and the window
  chrome's sample are drawn from their own origin (`GalleryPanel`), as the
  kit draws their header and footer from a canvas's origin.
- **Working and fixed samples.** Each interactive component appears once
  working on `GalleryModel`, named `gallery.<section>.<n>` from 1: tabs
  switch, the button, the link and the OA button count presses, the switch
  flips, the strip steps, the drop-down opens and chooses a language, the
  slider moves and drags, the search field takes typing, filter chips turn
  on and off, nav entries, rows and cards are chosen, and the scroll list
  scrolls, the carousel's arrows and cards
  (`gallery.carousel.previous`, `gallery.carousel.next` and
  `gallery.carousel.card.<tag>`) move which card is centred, a choice card
  is chosen, and the language card, Skip setup, Back and Next
  (`gallery.setup.skip`, `gallery.setup.back`, `gallery.setup.next`) and
  the call to action take a press. CLOSE is `gallery.close`, last in Tab's
  order. Each appears again in each fixed state, which takes no input:
  idle, hovered, pressed, focused (with the focus ring), disabled (its
  disabled look, or the locked fade over it), and locked where it has a
  lock. The sample texts are fixed: "Sample Mod" by "the sample team",
  "Example Maps", "Another Mod", "Third Mod", English, Deutsch, Español,
  Français and 日本語, a Ridge card ("From Ridge · Coverage 100%"), choice
  cards "Ridge pack" and "Not now", a coach mark "Library", a picture well
  captioned "Library, Active" and a call to action "Start playing". Nothing
  in the gallery reads or changes a setting.
- **How its test guards the kit.** `ui-kit-gallery` lays it out at Compact
  (640 by 480 points at 1x), Large (1280 by 720 at 1x) and Regular (960 by
  540 points at 2x), and fails when a role of `all_roles` that `paint`
  draws has no part in it, naming the role: a component added to the kit
  without a sample in the gallery breaks it. Every component role whose
  parts draw a control shows idle, hovered, pressed, focused and disabled;
  every part lies in the frame or its scroll area; no two controls
  overlap; Tab visits every focusable control once in the declared order;
  Down goes to the nearest control below; the names are sound and start
  with `gallery.`; no text holds the build's version; and toggling,
  choosing and typing change the model alone.
- **How to open it.** Settings › Developer's UI kit gallery row: SHOW
  (`oa.settings.kit-gallery.show`) opens it on the OA layer, modal, over
  the whole window, on the main menu or in a match; Escape or CLOSE goes
  back to Developer. The automation endpoint names it `oa.gallery`. The
  native check `native-kit-gallery` and a stage's `gallery` line open it
  too ([src/app/README.md](../../app/README.md)).

## State

None: every model is the caller's. The kit keeps nothing between calls and
writes no file and no setting. A screen holds its own model (a settings
page's values, the gallery's `GalleryModel`, a notice's or a question's
marked button), its `Interaction` (the hovered, held and focused control),
its fields' text and caret, its scroll offsets and its hover card's timer,
and passes them in; the kit's functions read them and change only what they
are handed. A preference row reads and writes the `PreferenceModel` its
caller fills from the preferences and saves. `load_game_fonts` reads the
game's font files, and the canvas draws on the surface or the painter its
caller gives it. Reduced motion is `Frame::reduced_motion`. It comes from
a later Settings row, and the kit does not read that row: a screen copies
the flag onto a carousel's look and a call to action's look. Off, the
default, the carousel turns and the call to action sends out a ring. On,
the carousel slides flat and the call to action holds a steady ring.

## Invariants

- Kit 2's look: every OA screen draws through the kit's tokens, so a token
  changes every screen at once. On the crisp form at 1x at Compact, the
  settings dialog, its notices and prompts and the OA button draw the
  pixels `ui-engine-settings-pixels` and `ui-engine-settings-pixels-data`
  hold in fixed states in `src/ui/engine-settings/tests/pixels.txt`, which
  changes only when a change to their look is accepted.
- Each class's metrics hold its steps on Kit 2's scales and what follows
  from them, and the screens' sizes: Compact's are 0.7.3's exactly, for
  the controls 0.7.3 has, and Regular's and Large's differ from them only
  in the sizes the design's table gives the classes, and in the first-run
  language card and the carousel's step (`ui-kit-theme`).
- One focus ring: focus_ring (1) point of the light accent, its outside
  focus_inset (2) points out from its control, so a point clear of the
  control stays between them, its corners rounded as the control's are
  and that much rounder; a control drawn with the ring on its own edge (a
  nav entry, an open menu's item, a keycap) keeps the ring inside it.
  Kit 2's design draws the ring two points thick and two points clear;
  the screens keep two points round a control, and clip a row's ring
  there, so the kit's ring keeps to them until a screen keeps four.
- Shapes: buttons, fields, drop-downs, level strips, list rows, nav
  entries and the focus ring take the control radius of their height;
  cards and hover cards the card radius; chips, pills, switches, the
  slider and the progress bar are pills; keycaps keep their darker bottom
  edge under rounded corners. A window keeps its square edge: what lies
  round a rounded window's corners is the layer's to draw, not the
  window's. A switch is a pill track, switch_width by switch_height held
  to its area and at the area's right (`switch_track`; the track is
  `switch_track_width` wide, the area the screens give a switch
  `switch_width`), with a round knob
  two points in: lime with the knob at the right in the dark ink while
  On, the selected surface with the knob at the left while Off. Its Off
  and On words name its halves to automation and are not drawn.
- Every token's value is pinned by `ui-kit-theme`.
- One wrap. Its rules are the only differences between its callers.
- The kit never looks text up in the interface catalogue, and it forces no
  case on a word: callers pass what to show, and the words decide whether
  a caption is in capitals. A button's caption, a switch's Off and On, a strip's captions, a
  menu's items, a header's words, a nav entry, a row's label and hints, a
  chip's, a tab's, a card's, a hover card's and a link's words arrive
  already looked up. A text field holds the player's own text. A notice's
  title, lines and captions pass through the look-up its caller hands it,
  when one is handed: the notice wraps the words it was given and shows
  each line as the look-up returns it, as the settings dialog's notice
  always has. A question's texts are drawn as given.
- The controls use only kit tokens, and their sizes are `Metrics`
  members. Kit 2's design is drawn in web colours; the kit draws them so:

  | Design | Kit token |
  |---|---|
  | s0, bars and wells | `colour::band`, `colour::well` |
  | s1, panels and lists | `colour::list` |
  | s2, cards and dialogs | `colour::panel` |
  | s3, hover | `colour::hover` |
  | s4, selected, tracks, off | `colour::list_selected`, `colour::off_selected` |
  | lime | `colour::accent` |
  | lime light, the focus ring | `colour::accent_light`, `colour::focus` |
  | lime ink | `colour::on_accent` |
  | amber | `colour::lock` |
  | red | `colour::danger` (e8806f) |
  | blue | `colour::information` (7db3de), which `colour::online` is |
  | brand gold | `colour::brand_light`, `colour::brand`, `colour::brand_shade` |
  | primary, secondary, tertiary text | `colour::text`, `colour::text_secondary`, `colour::text_tertiary` |
  | line | `colour::control_border` |
  | line2 | `colour::control_hover` |
  | strong edge | `colour::strong_edge` |
  | a badge's tinted fill | `colour::ok_tint` and the other tints |
  | the hover card's ground | `colour::band` |
  | a selected row's tint | `colour::accent` blended at `selected_tint` (15/256) over the face |

- A text field's text is well-formed UTF-8 without control characters, and
  its caret stands on a character boundary: `insert_text` refuses anything
  else whole, and `edit_text` deletes and steps over whole characters.
- `kit::Key` keeps the settings dialog's keys' values up to No; the editing
  keys, Backspace and Delete, follow it, and then the information key,
  Info, which the OA layer gives F1 outside a match. `key` does nothing
  with Info, as with Yes and No: a screen that has more to tell about what
  the pointer or the focus is on answers it before it calls `key`.
- A hover card is due only once the pointer has rested on one control for
  `hover_card_delay_ms`. The timer is the screen's: the kit keeps none.
- A notice and a question place their parts from the Compact metrics, but
  for their width and greatest height, which are their size class's: their
  text wraps at the wider width, and their buttons keep their sizes. A
  question's button is as wide as its caption and the padding, the caption
  counted at the wider of the estimated width and the small font's
  measure: a caption the fonts draw wider than the estimate, as the modern
  fonts draw Cyrillic capitals, widens its button, and one they draw
  narrower keeps the estimate's place. A row wider than the room between
  the box's paddings is narrowed from its widest buttons. The pointer and
  finger functions take the fonts the question is drawn in, so a press
  lands where a button is drawn. Their
  events hit and reach through `hit` and `reach` over the same controls
  their display lists hold: a notice's OK before its open button, a
  question's buttons left to right.
- One button, six looks, and the disabled look: accent, the primary
  button, lime under the dark ink; plain, the secondary button, the hover
  surface inside the strong edge, the selected surface under the pointer;
  quiet, a lime caption alone at rest and the hover surface under the
  pointer; inset, the list's face; danger, outlined and written in the
  red, its tint under the pointer; and filled. A button that is not
  enabled draws no face, the control border and the idle caption. Every
  look but filled has the control radius of its height
  (`control_radius_for`). Filled is a deep green face (`colour::filled`, 3e5218,
  7:1 against the white text; `filled_light` under the pointer and
  `filled_held` while held both keep at least 5.7:1) under white text
  (`colour::text`), with rounded corners a quarter of its shorter side, at
  least 2 points (`filled_radius`), drawn as two rectangles with rounded
  corners (`fill_corner_rect`, the canvas's capsule fill with quarter-circle
  corners and straight edges), the edge and the face one point in. It is for a button that
  stands over a 3.1c picture:
  Mutators. The kit chooses every colour. A caller passes the style and
  whether the pointer is over the button or holds it. A button may wear the
  OA badge (`ButtonLook::oa_mark`): the badge at its left and the caption
  after it, keeping to the left, in the look's font (`ButtonLook::font`,
  small unless the caller picks regular). `button_mark_parts` places them:
  a fifth of the button's height pads the left, the gap after the badge
  and the right, and the badge is a square as tall as the button less that
  padding above and below. The caller fits the caption to that box. The
  gallery's filled sample wears the badge.
- `paint` draws a display list's items in the list's order. A control is
  added with its automation name.
- Two backends, one set of components. The crisp form of the canvas draws
  every whole scale in the game's fonts, each scale's pixels its own; the smooth
  form, for 150%, puts every rectangle's edge on a whole pixel, draws its
  text in the modern fonts and never scales a game font. Both draw through
  the same primitives, from the same draw functions, and lay out the same
  ([The canvas's two forms](#the-canvass-two-forms)).
- The display list is the only record of what a screen drew and where its
  controls are. Hit testing reads `controls`. Drawing reads `items`.
- Every control of a kit screen carries a name unique on its screen,
  ignoring case: words of a-z, 0-9 and hyphens joined by dots, at most 100
  bytes. Names are lower case, so two that differ differ ignoring case too;
  `name_problem` reports the first fault. The automation endpoint adds its
  own prefix; the name here has none. A part of a control is named after
  it with one word more, from the code's own words (a row's, a choice's or a
  button's id), never from a text as shown.
- A column of rows is placed exactly as the settings dialog places a
  section's rows: `ui-kit-rows` holds rows shaped as Controls', Graphics' and
  Developer's to the dialog's own placement, and U08 proves it on every page.
- A row's kind is its spec's `kind`, and nothing else decides it. A spec
  has an id, one word of a-z, 0-9 and hyphens; a table built at compile time
  with a malformed id does not compile.
- A `RowView` and the display list own their texts: they keep nothing of
  the spec or the model.

The arrows move by where controls sit. A control's own scroll area is searched
first, including rows the area does not show. The focus leaves that area only
when nothing in it lies that way, and a move from outside never lands on a
row the area hides. A control in no scroll area, such as a footer button or
an entry of a section list, has no area of its own: every control that shows
is searched at once. Given the scroll areas (`focus_toward` with `areas`),
the arrows judge every control where it lies with the areas laid out whole:
a row moved back by its area's offset, a control under an area's view moved
down by its limit, every row a candidate whether its area shows it or not,
so that from a button under a list Up reaches the list's last row, as the
folder chooser's arrows do. Tab follows the declared order and wraps. Up and Down
never follow that order. Left and Right go to a focused text field, which
moves its caret, as do Home, End, Backspace and Delete. Tabs and cards do not
take Left and Right: the arrows move the focus between them. Enter goes to a
focused button, link, list row or tab.

## Tests

`ui-kit-theme` checks every token against Kit 2's value (the screens'
tokens against the dialog's, the category tints against the values they
keep, and the tinted fills against their mix), the sizes every class
shares (Kit 2's focus ring, window bands, switch and badge, and the
screens' sizes, 0.7.3's), each class's steps on Kit 2's type, spacing and
shape scales and what follows from them, the design's table of dialog
sizes, that Regular and Large differ from Compact in nothing else, and
that each class's dialog and notices fit the least window of their class. `ui-kit-text` checks the UTF-8
helpers, the estimated width, the stand-ins, the wrap against copies of
the six wraps it replaces, `fit` and `fitting_start` against the Game files
screen's copies, `path_break` at its separators and against the folder
chooser's copy, the modern fonts' measure through its hooks and
without them, and the modern look: each role's type, Barlow's measure the
same at 1x and 2x with each character at twice its pen, a tracked line in
the condensed type, Cyrillic and Chinese still drawn, no stand-ins, a
boxed line drawn inside its box and its clip at 2x, and each class's steps
on the type scale (`Fonts::size_class`): the measure, a tracked line and a
drawn line growing from Compact to Large, the classic look's alike at every
class. `ui-kit-layout` checks the Auto scale, the size classes'
edges and the resolution table they make (Compact only where nothing
larger fits), a chosen class held to a room, the design's
window sizes, the Interface size's examples and Auto at each window, 150%
where Compact fits at one and a half and 1x where it does not, that
`metrics_of` gives each class its metrics, `to_canvas`'s rounding at 1.5x,
arrangements, the scroll arithmetic and hit testing (a disabled control
passing a point on or blocking it), and each helper of a screen drawn in the
modern fonts with a made-up measure: points to canvas pixels, a text part's
wrap, line limit, fit and path break, a line part, a button's span, label and
look, a row of buttons on one line and wrapped, icons, plain parts and moved
parts, the three ways a column is placed, and the controls listed from parts
under and above a backdrop.
`ui-kit-smooth` checks the smooth form at 1.5x: a coverage map landing on
the pixels a rectangle covers, a state icon inside its square's pixels
with smoothed edges, a 10 by 10 point fill
covering exactly 15 by 15 pixels from `lround(x * 1.5)`, at even and odd
points, fills sharing their edges, outlines, raised edges and rules of whole
pixels, text measured wider than at 1x and narrower than at 2x, drawn
antialiased with its capitals centred, the regular font bold, a text too
wide for its box drawn at full size and recorded, tracking, a clip that keeps what
lies outside it, a picture drawn filtered, a mark, a moved origin, and a
button and a display list drawn through the same functions as on the crisp
form. Its text checks read the bundled fonts beside the test.
`ui-kit-input` checks reach (with a reach of a fraction of a point too, and
a disabled control blocking a finger), `mark_states`, the pointer, Tab, the
arrows (with a scroll area laid out whole too), the keys and
their values, the wheel's fractions, the names, the parts automation lists
(a switch's halves splitting it, a strip's levels where `level_at` finds
them, an open drop-down's items at `choice_item` and a screen's own item
control listed once, a menu with a divider naming the other items by their
ids and naming the divider not at all, a row of buttons' buttons and a
clipped control's press area), and a text field's typing and editing over
characters of one to four bytes. `ui-kit-components` checks each control's
pixels (the six button looks and the disabled one with their rounded
corners, the pill switch and its knob, the radio's ring and dot in each state
and a radio row's box and its parts, the rounded strip, slider,
drop-down, menu and scroll bar, and the OA mark in the brand gold), the OA button's dot at 24 and 32 points in every look, with and
without the icon, at 1× and 2× (the button drawn without it unchanged, and
with it differing only inside the dot's square), that `paint` matches a
direct draw, and the controls' geometry,
including a menu row that is a divider, one with a right-aligned aside and
one that cannot be chosen.
`ui-kit-chrome` checks the header, the footer band, the nav list, a row's
frame and the locked fade, and that `paint` matches those direct draws; a
header given its height draws its band, rule, mark and title in the
middle of it, and a height of 0 draws exactly what a look that names none
draws.
`ui-kit-controls` checks each new control's pixels in the tokens above with
a synthetic font: list rows in both forms, every chip state as a pill,
every pill state, a pill's focus ring, the three
state icons at 1x and 2x (inside their square, worked out at 2x rather
than doubled, whole and smoothed pixels, the ring and the download
arrow's shaft where the design puts them), a chip with a state icon in
each colour and its place in a row, a row's aside keeping a whole
version, the field's
placeholder, caret, scrolling and soft ring while focused, tabs with and without a count, cards and
their grid at three widths and at 3, 4 and 6 columns, the arrows through a
grid of cards, the hover card with an arrow on each side, its place in a
frame and its delay, and links; and that every `add_*` names its controls
and `paint` matches the direct draws.
`ui-kit-notices` checks where six notices and six questions place their
parts against the settings dialog's own placement, their display lists'
names, kinds and Tab order, a word of their own (`language-notice`) and
their text's control, which changes what no press or finger finds at any
point of the box, where a finger lands, every key (the editing
keys and Info, which do nothing, among them), that `paint` matches `draw_notice` and
`draw_question`, and the progress bar's rounded fill, amber while paused. It fits a question's buttons
to a measure and to fonts wider than the estimate, where the display list,
the placement and the presses agree, and narrows a row too wide for its box.
A question's parts place in order (the callout and its mark, a box's
heading, a failed step that wraps with its mark on its first line only,
the line under them, a quiet button at the left and the others at the
right), two boxes side by side are as tall as each other, a danger button
has its look, and a part the height cuts is left out whole; the update
pill's parts, its two controls and their names, and a press and release
on its button, on the pill and across, with no button and with a paused
bar.
`ui-kit-rows` lays out, draws, focuses and changes a page of every row
kind from a table: the placement of rows shaped as the settings dialog's
Controls, Graphics and Developer sections, with the game's fonts and the
modern fonts' taller hints, locked and not, against values computed with
the dialog's own placement, and the scroll against the dialog's; the names,
kinds, Tab order and drawing order of the display list; the ids of a
drop-down's items and a strip's levels, and the parts they name; rows of two models
in one column; every event on every kind; and the page painted.

`ui-kit-gallery` checks the gallery at Compact, Large and Regular, as
[Gallery](#gallery) says: every drawn role of `all_roles` present, five
states of every component role that takes input, every part in the frame
or its scroll area, no two controls overlapping, Tab and Down, the names,
no text holding the build's version (which the test is given), and the
working samples changing the model alone; and that it draws. Each case
runs in both looks, the modern one with the fonts beside the test, where
the header's title is Barlow Condensed's width and the pixels differ from
the classic look's.
`ui-kit-first-run` checks the perspective blit (a turned edge shorter than
the near one, an opacity, a clip, a nested blit that draws nothing, and a
smooth copy that is filtered), that a carousel's place at a time is the
same however the time was sliced, where a choice group steps and where a
coach mark sits, and that every first-run component draws at Compact,
Regular and Large, on the crisp form and at 150%, with its states differing.

`ui-kit-tag-icons` checks the eight tag ids, each icon at 8, 12, 20, 28
and 40 points at 1x, 2x, 3x and 150%, a badge's picture, tag, letters and
blank, a row, pill, nav entry and question with nothing new pinned to the
pixels from before the change, a byline's colour, a drag handle in each
state, and a question that grows by two item lines and names them.
`ui-kit-keys` checks each keyboard's geometry, `code_word` and the names
`add_keyboard` and `add_mouse` give, that every part stays inside its
rectangle at 1x, 2x, 3x and 150% and that 2x is not 1x doubled, that the
marks differ in shape, that each tint's face reaches 4.5:1 against its
ink, and that a reduced-motion ring snaps to quarters; a main-block
picture draws its keys alone, each keeping its control, and keeps every
one-letter key's legend, in the small font where the regular one is too
wide, as the whole keyboard at the same size does, where no named key's
legend is cut: each is drawn whole, shorter, or not at all.

The module's ctests are those fourteen: `ui-kit-theme`, `ui-kit-text`,
`ui-kit-layout`, `ui-kit-input`, `ui-kit-components`, `ui-kit-chrome`,
`ui-kit-controls`, `ui-kit-tag-icons`, `ui-kit-keys`, `ui-kit-notices`,
`ui-kit-smooth`, `ui-kit-rows`, `ui-kit-gallery` and `ui-kit-first-run`,
none of which needs the game's
data. Others guard the
kit from its users: `ui-engine-settings-pixels` and
`ui-engine-settings-pixels-data` pin Compact's pixels
([Invariants](#invariants));
`native-kit-gallery` opens the gallery from Settings › Developer on
640x480, 1280x720 and 1920x1080 windows (Compact at 1x, then Large at 1x on
both, 1920x1080's Auto taking its step down to Large) and checks its class,
scale and pixels, with game data; `screen-registry` checks the registrations an
extension makes; `native-registered-kit` checks a page, a kit screen, a
notice and a prompt registered from inside the engine; and, in a build
configured with `-DOA_RECORD_EXTENSION_HOOKS=ON`, `extension-kit` checks
the Kit fixture, a test extension that uses the kit through the extension
table alone (`tests/extension/kit_fixture.cpp`).

## Limitations

The kit is provisional until it is declared stable (D31), a decision of its
own that is not tied to a release: until then it changes whenever OA's
screens need it to, and the rework that follows is expected, in the
engine's screens and in any extension that uses it. Each header says so in
its first comment. From version 15 its public headers are part of the
extension table's contract (`OA_EXTENSION_API_VERSION`, in
`src/app/include/oa/app/extension.hpp`): a change to one before 0.8.0, the
first release that carries version 15, keeps 15 and updates the engine's
own followers and test extensions in the same change; from 0.8.0 on, any
change to a kit header raises the version.

The compact picture is the first-run section, which spans that window at
the cards' real size, so the sections under it sit further down. Its
choice-card states sit over the footer, clear of CLOSE. The regular picture
shows First run and the headings of the next sections. The large picture
shows First run and the tops of Window chrome, Tabs and Buttons. A facts
line wider than its language card wraps onto the next small line above the
pill. The Chips section shows the two maker's marks, the OA badge and
the bone, as chips with no control. The other state icons are reviewed in
the Library's pictures (`oa-ui-library-draw-test --frames`).

Text is laid out and drawn left to right, one character after another, in
the order it is stored: the kit has no right-to-left or bidirectional
layout, and no screen is mirrored. 150% is the only fractional size, and
only on the smooth form in the modern fonts; every other scale is a whole
step.

The metrics hold the settings dialog's, its notices' and its prompts' sizes
for each class (`regular_metrics`, `large_metrics`, `metrics_of`); the
Library's and the map browser's are theirs to add. A drop-down's open menu
keeps above the footer line it is given (`choice_menu`), the Compact
settings dialog's unless a larger dialog gives its own. A menu item may be
a divider, drawn as a section heading and skipped by the focus, with no
automation name; an item may carry a right-aligned aside, clipped before it
meets the label; and an item may be greyed so a press does not choose it
(`ChoiceMenuRow`). The smooth form lays
the screens out in the game's fonts' widths and draws them in the modern
fonts, so a text that would not fit its box is drawn a little smaller; the
only fractional scale the layer offers is 150%. The arrows' rule is one function, its weight named, and is to be
tuned after controller playtests (D30). The look of the list rows, chips, fields, tabs, cards,
hover cards and links follows the design's components mockup in the kit's
tokens; it is reviewed in the gallery ([Gallery](#gallery)) and pinned by
golden frames (U17). A field takes typed text and the editing keys; it has no selection,
no clipboard and no press that places the caret.
A row of buttons takes up to three, with Your files' widths. A link is as
wide as its line unless its spec gives a width. The rows do not draw a
drop-down's open menu, a scroll bar or the rule a scrolled view keeps at
its top: the screen draws them, as the settings dialog does. The gallery
shows the parts of a screen drawn in the modern fonts (`header_bar` to
`sheet`) nowhere, since `paint` draws nothing for them: the Game files
screen and the folder chooser show them. Its notice and question are
Compact's at every class, the width a column holds, and its fixed samples
keep Compact's sizes, as every component does.
