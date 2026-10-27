// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// The OA UI kit's colours, its type, spacing and shape scales, and the
// metrics of its three size classes, Compact, Regular and Large, worked out
// from those scales. One point is one pixel of the 640 by 480 picture.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace oa::ui::kit {

/// A colour: red, green, blue and opacity, each 0 to 255.
struct Colour {
    uint8_t r{};    ///< red
    uint8_t g{};    ///< green
    uint8_t b{};    ///< blue
    uint8_t a{255}; ///< opacity; 255 is opaque
    friend constexpr bool operator==(Colour, Colour) = default;
};

/// Returns a colour as the artless painter takes one, its opacity dropped.
///
/// @param colour the colour
/// @return red, green and blue
[[nodiscard]] constexpr std::array<uint8_t, 3> rgb(Colour colour) noexcept {
    return {colour.r, colour.g, colour.b};
}

/// Returns one opaque colour laid over another at a share, each channel
/// rounded to the nearest whole value: the tinted fills of badges and
/// chips, worked out once.
///
/// @param ink the colour laid over
/// @param ground the colour under it
/// @param share the ink's share, in 256ths
/// @return the mixed colour, opaque
[[nodiscard]] constexpr Colour mixed(Colour ink, Colour ground, uint32_t share) noexcept {
    const uint32_t kept = 256U - share;
    const auto channel = [share, kept](uint8_t over, uint8_t under) {
        return static_cast<uint8_t>((over * share + under * kept + 128U) / 256U);
    };
    return {channel(ink.r, ground.r), channel(ink.g, ground.g), channel(ink.b, ground.b)};
}

/// How far a badge's or a chip's colour tints its fill, in 256ths: 16 percent.
inline constexpr uint32_t badge_tint = 41;
/// How far a focused text field's soft ring carries the accent, in 256ths.
inline constexpr uint32_t field_ring_tint = 64;

/// The colours of OA's own screens: the settings dialog, its notices and
/// prompts, the Library, the key editor, the pickers, first run and the OA
/// button. The surfaces are one ramp from the bars (s0) to the selected
/// entry (s4).
namespace colour {

/// The bars and wells (s0): the header's and the footer's face.
inline constexpr Colour band{0x12, 0x14, 0x10};
/// Panels and lists (s1): the section list's face.
inline constexpr Colour list{0x17, 0x1a, 0x15};
/// Raised surfaces (s2): the dialog's face, cards and raised buttons.
inline constexpr Colour panel{0x1d, 0x21, 0x19};
/// Hover (s3): an entry, a row or a button under the pointer.
inline constexpr Colour hover{0x25, 0x2a, 0x21};
/// Selected (s4): the selected section's entry, a chosen chip.
inline constexpr Colour list_selected{0x2c, 0x32, 0x27};
/// The hairline rules between parts.
inline constexpr Colour rule{0x2b, 0x30, 0x27};
/// The strong edge: a raised button's border and the window's outline.
inline constexpr Colour strong_edge{0x4f, 0x57, 0x47};
/// The raised edges' light side, top and left.
inline constexpr Colour edge_light = strong_edge;
/// The raised edges' dark side, bottom and right.
inline constexpr Colour edge_dark{0x08, 0x09, 0x07};
/// Primary text: labels, values and the title.
inline constexpr Colour text{0xe7, 0xe8, 0xdf};
/// Secondary text: captions of raised buttons and entries not selected.
inline constexpr Colour text_secondary{0xb9, 0xbf, 0xae};
/// Tertiary text: hints and quiet words.
inline constexpr Colour text_tertiary{0x95, 0x9c, 0x8b};
/// Hints and the title's last word.
inline constexpr Colour hint = text_tertiary;
/// The section heading, the version and a hack's id under its title.
inline constexpr Colour quiet = text_tertiary;
/// The list's entries that are not selected.
inline constexpr Colour list_text = text_secondary;
/// The accent: what is selected, On, the slider's filled track and OK.
inline constexpr Colour accent{0x9c, 0xcc, 0x3c};
/// The accent's lighter edge, the accent under the pointer, and the focus ring.
inline constexpr Colour accent_light{0xb6, 0xe0, 0x5a};
/// The accent while it is held.
inline constexpr Colour accent_held{0x8a, 0xb8, 0x30};
/// Text on the accent.
inline constexpr Colour on_accent{0x10, 0x12, 0x0d};
/// The keyboard focus ring.
inline constexpr Colour focus = accent_light;
/// A filled button's face: the accent, deep enough to carry the white text.
inline constexpr Colour filled{0x3e, 0x52, 0x18};
/// A filled button's face under the pointer.
inline constexpr Colour filled_light{0x49, 0x60, 0x1c};
/// A filled button's face while it is held.
inline constexpr Colour filled_held{0x35, 0x45, 0x14};
/// The well of a level strip, a slider's track, a drop-down and a text field.
inline constexpr Colour well = band;
/// The border of a switch, a level strip, a slider's track and a field.
inline constexpr Colour control_border{0x3a, 0x40, 0x34};
/// That border under the pointer.
inline constexpr Colour control_hover{0x5b, 0x63, 0x52};
/// An Off switch's track.
inline constexpr Colour off_selected = list_selected;
/// A disabled control's caption, and an Off switch's knob.
inline constexpr Colour switch_idle{0x7d, 0x84, 0x74};
/// A raised button's and a level's caption.
inline constexpr Colour button_text = text_secondary;
/// Caution and locks: the padlock, the lock's text and the shared game's note.
inline constexpr Colour lock{0xe0, 0xb0, 0x4f};
/// The colour the screen under the dialog is darkened with.
inline constexpr Colour backdrop{5, 6, 4};
/// Danger: a problem, a removal, a button that cannot be undone.
inline constexpr Colour danger{0xe8, 0x80, 0x6f};
/// Information: a release offered, a note.
inline constexpr Colour information{0x7d, 0xb3, 0xde};
/// Online: a chip that says a thing is on the network.
inline constexpr Colour online = information;
/// The brand gold's light, its top: only the OA mark, the window's brand
/// word and Core Prime badges wear the gold, never an action or a warning.
inline constexpr Colour brand_light{0xf3, 0xcf, 0x76};
/// The brand gold.
inline constexpr Colour brand{0xd6, 0xa7, 0x47};
/// The brand gold's shade, its foot.
inline constexpr Colour brand_shade{0x9c, 0x6f, 0x22};
/// A badge's or a chip's fill that says all is well.
inline constexpr Colour ok_tint = mixed(accent, panel, badge_tint);
/// A badge's or a chip's fill that asks for care.
inline constexpr Colour caution_tint = mixed(lock, panel, badge_tint);
/// A badge's or a chip's fill that says something is wrong.
inline constexpr Colour danger_tint = mixed(danger, panel, badge_tint);
/// A badge's or a chip's fill that informs.
inline constexpr Colour information_tint = mixed(information, panel, badge_tint);
/// A Core Prime badge's fill.
inline constexpr Colour brand_tint = mixed(brand, panel, badge_tint);
/// The soft ring round a focused text field.
inline constexpr Colour field_ring = mixed(accent, panel, field_ring_tint);
/// A key's orders tint: the accent.
inline constexpr Colour category_orders = accent;
/// A key's selection tint.
inline constexpr Colour category_selection{0x6e, 0xa8, 0xd6};
/// A key's pointer tint: the selection tint.
inline constexpr Colour category_pointer = category_selection;
/// A key's groups tint.
inline constexpr Colour category_groups{0xc4, 0x9b, 0xe0};
/// A key's camera tint.
inline constexpr Colour category_camera{0x5f, 0xc0, 0xa8};
/// A key's building tint: the lock colour.
inline constexpr Colour category_building = lock;
/// A key's game tint.
inline constexpr Colour category_game{0xb9, 0xbf, 0xae};
/// A key's chat tint.
inline constexpr Colour category_chat{0xe5, 0x8f, 0xb0};
/// A key's window tint: the game tint.
inline constexpr Colour category_window = category_game;
/// A key's hacks tint.
inline constexpr Colour category_hacks{0xe0, 0x8a, 0x4f};
/// A key's mod tint.
inline constexpr Colour category_mod{0x8f, 0xd0, 0xe8};
/// A key's developer tint.
inline constexpr Colour category_developer{0x8a, 0x91, 0x80};

} // namespace colour

/// The Game files screen and the folder chooser.
namespace screen_colour {

/// The screen behind everything.
inline constexpr Colour background = colour::panel;
/// Cards, rows and sheets: the hover surface, a step above the screen.
inline constexpr Colour panel = colour::hover;
/// Outlines and dividers: the dialog's control border.
inline constexpr Colour line = colour::control_border;
/// Text: the dialog's text.
inline constexpr Colour text = colour::text;
/// Secondary text.
inline constexpr Colour dim = colour::text_secondary;
/// The main button, found and done.
inline constexpr Colour green = colour::accent;
/// Warnings: the dialog's lock.
inline constexpr Colour amber = colour::lock;
/// Problems and removals: the dialog's danger.
inline constexpr Colour red = colour::danger;
/// The main button's text, dark ink on the green fill.
inline constexpr Colour ink = colour::on_accent;
/// A plain button's text.
inline constexpr Colour button_text = colour::button_text;

} // namespace screen_colour

/// The touch controls.
namespace hud_colour {

/// Panels and buttons: gunmetal at 85% opacity.
inline constexpr Colour panel{0x1b, 0x1e, 0x19, 217};
/// Sheets and the radial's ring: darker and nearly opaque.
inline constexpr Colour sheet{0x14, 0x16, 0x12, 248};
/// An empty cell of the drawer or the MORE sheet.
inline constexpr Colour cell{0x24, 0x28, 0x20, 230};
/// A control a finger rests on: a lighter gunmetal.
inline constexpr Colour pressed{0x3b, 0x42, 0x35, 240};
/// The hairline round panels and buttons.
inline constexpr Colour edge{0xff, 0xff, 0xff, 34};
/// Lit, latched and armed controls.
inline constexpr Colour lit{0x9c, 0xcc, 0x3c};
/// A lit control a finger rests on.
inline constexpr Colour lit_pressed{0xb8, 0xde, 0x66};
/// Labels and icons on a lit control.
inline constexpr Colour ink{0x1b, 0x1e, 0x19};
/// Labels and icons.
inline constexpr Colour label{0xf2, 0xf4, 0xee};
/// The quieter text of the drawer's caption and page dots.
inline constexpr Colour quiet{0xb4, 0xb9, 0xae};
/// SELF-DESTRUCT · HOLD's red: its edge, and its fill as the hold fills it.
inline constexpr Colour danger{0xc0, 0x39, 0x2b};
/// SELF-DESTRUCT · HOLD's panel before its hold fills it.
inline constexpr Colour danger_panel{0x2a, 0x15, 0x12, 235};
/// SELF-DESTRUCT · HOLD's label before its hold fills it.
inline constexpr Colour danger_label{0xf0, 0x8c, 0x80};
/// The lines between the radial's wedges.
inline constexpr Colour wedge_gap{0x08, 0x09, 0x07};
/// Greyed items show at this opacity.
inline constexpr float greyed_opacity = 0.4F;

} // namespace hud_colour

/// How far the main menu is darkened under the dialog, in 256ths.
inline constexpr uint32_t menu_backdrop_opacity = 159;
/// How far a coach mark's spotlight dims the layer, in 256ths: 74 percent
/// of the backdrop, to the nearest 256th (189 / 256).
inline constexpr uint32_t spotlight_opacity = 189;
/// How far the in-game menu's column is darkened beside the dialog, in 256ths.
inline constexpr uint32_t ingame_backdrop_opacity = 128;
/// How far a locked row is faded into the panel, in 256ths.
inline constexpr uint32_t locked_fade = 115;
/// How far the accent tints a selected list row's face, in 256ths.
inline constexpr uint32_t selected_tint = 15;

/// The levels of the type scale. The modern look draws each at its class's
/// size (Metrics::title_size and the rest); the classic look, the game's
/// bitmap fonts at their two sizes, draws a title and a label in the
/// regular font and body text and small text in the small one
/// (classic_font in oa/ui/kit/text.hpp).
enum class TextLevel : uint8_t {
    title, ///< a window's or a page's title
    label, ///< a row's label, a button's and a tab's caption
    body,  ///< a paragraph, a hint
    small, ///< an aside, a count, an eyebrow
};

/// A size class's metrics, in points: its type and spacing scales, its
/// shapes' radii, the sizes of its controls, and the settings dialog's, its
/// notices' and its prompts' sizes. Each class's are worked out from its
/// spacing base (space) and its control height (button_height): the
/// padding is three spaces, rows are a space and a half apart and sections
/// three spaces. The header and the footer are the same in every class and
/// hold every class's buttons, and the sizes the type scale does not touch
/// (the game font's lines, the slider, the drop-down's items) are Compact's
/// in every class.
struct Metrics {
    /// A title's size in the modern look: a window's or a page's title.
    int32_t title_size{};
    /// A label's size in the modern look: a row's label, a button's caption.
    int32_t label_size{};
    /// Body text's size in the modern look: a paragraph, a hint.
    int32_t body_size{};
    /// Small text's size in the modern look: an aside, a count, an eyebrow.
    int32_t small_size{};
    /// The spacing base: every gap is a multiple of it.
    int32_t space{};
    /// The points between two rows of a list or a page.
    int32_t row_gap{};
    /// The points between two sections of a page.
    int32_t section_gap{};
    /// A keycap's corners' radius.
    int32_t keycap_radius{};
    /// A control's corners' radius: a button, a field, a drop-down.
    int32_t control_radius{};
    /// A card's corners' radius: a card, a hover card, an open menu.
    int32_t card_radius{};
    /// A window's corners' radius.
    int32_t window_radius{};
    /// A dialog's corners' radius: a notice, a question.
    int32_t dialog_radius{};
    /// The keyboard focus ring's thickness, drawn focus_inset clear of its control.
    int32_t focus_ring{};
    /// The raised edge's width.
    int32_t edge{};
    /// The header's height, under the top edge.
    int32_t header_height{};
    /// The footer's height, over the bottom edge.
    int32_t footer_height{};
    /// The space between a panel's edge and what it holds.
    int32_t padding{};
    /// The header mark's top row.
    int32_t mark_top{};
    /// The header mark's side.
    int32_t mark_side{};
    /// The OA mark's outlined square, which stands in for the icon.
    int32_t mark_square{};
    /// The space between the header's mark and the title.
    int32_t header_gap{};
    /// Extra columns after each glyph of a heading.
    int32_t heading_tracking{};
    /// A button's height: the control height of the class.
    int32_t button_height{};
    /// OK's and Cancel's width.
    int32_t button_width{};
    /// The columns between OK and Cancel.
    int32_t button_gap{};
    /// The columns between a control and its keyboard focus ring.
    int32_t focus_inset{};
    /// A line's height in the small font.
    int32_t small_line{};
    /// A line's height in the regular font.
    int32_t regular_line{};
    /// An Off/On switch's width: its track, a pill with a round knob.
    int32_t switch_width{};
    /// An Off/On switch's height, where its row gives it the room.
    int32_t switch_height{};
    /// A slider knob's width.
    int32_t knob_width{};
    /// A slider knob's height.
    int32_t knob_height{};
    /// A slider track's height.
    int32_t track_height{};
    /// A slider track's top row within the slider's area.
    int32_t track_offset{};
    /// A stop mark's height.
    int32_t stop_height{};
    /// A stop mark's top row within the slider's area.
    int32_t stop_offset{};
    /// Stop marks are drawn only this many columns or more apart.
    int32_t least_stop_spacing{};
    /// The columns between a drop-down field's left edge and its text.
    int32_t choice_text_inset{};
    /// The columns a drop-down field keeps at its right for its arrow.
    int32_t choice_arrow_room{};
    /// The drop-down's arrow's width.
    int32_t choice_arrow_width{};
    /// The drop-down's arrow's height.
    int32_t choice_arrow_height{};
    /// An open drop-down list's item's height.
    int32_t choice_item_height{};
    /// The most items an open drop-down list shows at once.
    int32_t most_shown_choices{};
    /// The columns between an open list item's left edge and its text.
    int32_t choice_item_text_inset{};
    /// The padlock's width.
    int32_t padlock_width{};
    /// The padlock's height.
    int32_t padlock_height{};
    /// The columns between the padlock and its text.
    int32_t padlock_gap{};
    /// A scroll bar's thumb's width.
    int32_t scroll_thumb_width{};
    /// A scroll bar's thumb's least height.
    int32_t least_thumb_height{};
    /// The columns and rows between the OA button's sides and its icon.
    int32_t button_icon_inset{};
    /// How far a held OA button's icon moves right and down.
    int32_t button_icon_press{};
    /// The OA button's outlined square, as this share of its side.
    int32_t button_square_numerator{};
    /// The denominator of the OA button's outlined square's share.
    int32_t button_square_denominator{};
    /// The least columns between the large OA mark and its square's outline.
    int32_t large_mark_margin{};
    /// A list row's height in its full form: a badge, a title and a line.
    int32_t list_row_height{};
    /// The largest side of a list row's badge.
    int32_t list_row_badge{};
    /// A list row's height in its short form: the Mods row's.
    int32_t short_list_row_height{};
    /// The columns between a list row's edges and its badge, text and chip.
    int32_t list_row_inset{};
    /// The rows from the top of a list row's text to its title's line.
    int32_t list_row_title_top{};
    /// The rows from the top of a list row's text to its first line under the title.
    int32_t list_row_line_top{};
    /// The columns between a row's title and its byline, and before its aside and its chip.
    int32_t list_row_gap{};
    /// A hairline's thickness: a blank badge's dashes.
    int32_t hairline{};
    /// A blank badge's dashes' length.
    int32_t badge_dash{};
    /// The space between a blank badge's dashes.
    int32_t badge_dash_gap{};
    /// A chip's height on a line of text: a row's chip and its tags, and a pill.
    int32_t chip_height{};
    /// The columns between a chip's edges and its text.
    int32_t chip_padding{};
    /// The columns between two chips side by side.
    int32_t chip_gap{};
    /// A filter chip's height: a pill on its own that turns a filter on and off.
    int32_t filter_chip_height{};
    /// A status badge's height: a rounded word on a tinted fill.
    int32_t badge_height{};
    /// A state icon's side: a chip that shows one is this square.
    int32_t state_icon_side{};
    /// A strip of tabs' height.
    int32_t tab_height{};
    /// The columns between a tab's edges and its caption and count.
    int32_t tab_padding{};
    /// Extra columns after each glyph of a tab's caption.
    int32_t tab_tracking{};
    /// The rows of the accent rule under the selected tab.
    int32_t tab_rule{};
    /// A tab's count's height.
    int32_t tab_count_height{};
    /// The columns between a tab's count's edges and its digits.
    int32_t tab_count_padding{};
    /// The columns between a tab's caption and its count.
    int32_t tab_count_gap{};
    /// A text field's height.
    int32_t field_height{};
    /// The columns between a text field's edges and its text, and around its magnifier.
    int32_t field_inset{};
    /// The magnifier's side, before a search field's text.
    int32_t magnifier_side{};
    /// The text caret's width.
    int32_t caret_width{};
    /// The narrowest a card may be and still add a column.
    int32_t card_least_width{};
    /// The points between two cards.
    int32_t card_gap{};
    /// The points between a card's edges and its preview and text, and its preview's edges and its chips.
    int32_t card_inset{};
    /// The points between a hover card's edges and its lines, and between its columns.
    int32_t hover_card_padding{};
    /// How far a hover card's arrow reaches out of its edge.
    int32_t hover_card_arrow{};
    /// A notice's or a question's first row of text: under the header, its
    /// rule and ten rows more.
    int32_t text_top{};
    /// The rows between two paragraphs of a notice or a question.
    int32_t paragraph_gap{};
    /// The rows between a notice's last line of text and the footer's rule.
    int32_t text_bottom_gap{};
    /// The width of a notice's button that opens its folder.
    int32_t open_width{};
    /// The columns a question button's caption keeps clear on its two sides together.
    int32_t prompt_button_padding{};
    /// A progress bar's height.
    int32_t progress_bar_height{};
    /// The rows between a settings row's rule and its label line, and under
    /// its last part. Developer's own rows keep half of it.
    int32_t row_padding{};
    /// The columns kept clear between a row's label and the control or lock
    /// beside it, and between a lock and the control it stands beside.
    int32_t label_gap{};
    /// The rows between a row's label line and its first hint line.
    int32_t hint_gap{};
    /// The rows added between two lines of a hint while the words are drawn
    /// in the modern fonts, whose ideographs stand as tall as a hint line.
    int32_t tall_hint_line_gap{};
    /// The most lines a hint takes.
    int32_t most_hint_lines{};
    /// The most lines a notice under a row's label takes.
    int32_t most_notice_lines{};
    /// The rows between a row's last hint line and its slider, drop-down
    /// field or text field.
    int32_t slider_gap{};
    /// A slider line's height: the track with its knob and stops, and the value.
    int32_t slider_line_height{};
    /// The width of a slider's value, right of its track.
    int32_t slider_value_width{};
    /// The columns between a slider's track and its value.
    int32_t slider_value_gap{};
    /// A lock's width: the padlock and its text, on a row's label line.
    int32_t lock_width{};
    /// A drop-down's field's width, and a row's text field's.
    int32_t choice_width{};
    /// A wide drop-down's field's width: room for the longest choice a
    /// settings row offers, in the regular font with the field's inset and arrow.
    int32_t wide_choice_width{};
    /// A row's own button's width, on its label line: MANAGE…'s.
    int32_t button_row_width{};
    /// The widths of a row of buttons on its label line, left to right: Your
    /// files' SAVES, SCREENSHOTS and MODS.
    std::array<int32_t, 3> folder_button_widths{};
    /// The columns between two buttons of a row.
    int32_t folder_button_gap{};
    /// The settings dialog's width.
    int32_t dialog_width{};
    /// The settings dialog's height.
    int32_t dialog_height{};
    /// The settings dialog's nav list's width, from its left edge: the
    /// content column starts right of it.
    int32_t nav_width{};
    /// The most characters a line of a host's text under a settings row
    /// holds, broken between words: what fits the content column's width in
    /// the small font.
    int32_t hint_line_characters{};
    /// A notice's or a question's width.
    int32_t notice_width{};
    /// A notice's or a question's least height; it grows with its text.
    int32_t least_notice_height{};
    /// A notice's or a question's greatest height: what the class's least
    /// window holds with a margin. Text below it is cut.
    int32_t greatest_notice_height{};
    /// A language card's width, in points. The design's card is 232 points
    /// wide at Large; Regular and Compact keep that card's proportions.
    int32_t language_card_width{};
    /// A language card's height, in points. The design's card is 304 points
    /// tall at Large.
    int32_t language_card_height{};
    /// How far a carousel's next card sits to the right of the one before
    /// it, in points. At Large this is the design's 236.
    int32_t carousel_step{};
    /// Tells whether two sets of metrics hold the same sizes.
    friend constexpr bool operator==(const Metrics&, const Metrics&) = default;
};

/// One size class's steps on the scales: its type sizes, its spacing base,
/// its radii and its control sizes.
struct ClassScale {
    int32_t title_size{};         ///< a title's size
    int32_t label_size{};         ///< a label's size
    int32_t body_size{};          ///< body text's size
    int32_t small_size{};         ///< small text's size
    int32_t space{};              ///< the spacing base
    int32_t keycap_radius{};      ///< a keycap's radius
    int32_t control_radius{};     ///< a control's radius
    int32_t card_radius{};        ///< a card's radius
    int32_t window_radius{};      ///< a window's radius
    int32_t dialog_radius{};      ///< a dialog's radius
    int32_t button_height{};      ///< the control height
    int32_t list_row_height{};    ///< a list row's height
    int32_t field_height{};       ///< a text field's height
    int32_t filter_chip_height{}; ///< a filter chip's height
    int32_t tab_height{};         ///< a strip of tabs' height
};

/// Compact's steps on the scales.
inline constexpr ClassScale compact_scale{17, 14, 13, 12, 4, 5, 6, 8, 10, 12, 28, 36, 30, 24, 32};
/// Regular's steps on the scales.
inline constexpr ClassScale regular_scale{20, 15, 14, 12, 6, 6, 7, 9, 11, 13, 34, 40, 36, 26, 38};
/// Large's steps on the scales.
inline constexpr ClassScale large_scale{24, 16, 15, 13, 8, 6, 8, 10, 12, 14, 38, 44, 40, 28, 44};

/// Returns metrics with a class's steps on the scales and what follows
/// from them: the padding three spaces, rows a space and a half apart and
/// sections three spaces apart.
///
/// @param base the metrics whose other sizes are kept
/// @param scale the class's steps
/// @return the metrics
[[nodiscard]] constexpr Metrics scaled(Metrics base, const ClassScale& scale) noexcept {
    base.title_size = scale.title_size;
    base.label_size = scale.label_size;
    base.body_size = scale.body_size;
    base.small_size = scale.small_size;
    base.space = scale.space;
    base.row_gap = scale.space * 3 / 2;
    base.section_gap = scale.space * 3;
    base.padding = scale.space * 3;
    base.keycap_radius = scale.keycap_radius;
    base.control_radius = scale.control_radius;
    base.card_radius = scale.card_radius;
    base.window_radius = scale.window_radius;
    base.dialog_radius = scale.dialog_radius;
    base.button_height = scale.button_height;
    base.list_row_height = scale.list_row_height;
    base.field_height = scale.field_height;
    base.filter_chip_height = scale.filter_chip_height;
    base.tab_height = scale.tab_height;
    return base;
}

/// The sizes every class shares, before a class's steps are applied.
inline constexpr Metrics shared_metrics{
    .focus_ring = 2,
    .edge = 1,
    .header_height = 36,
    .footer_height = 48,
    .mark_top = 9,
    .mark_side = 20,
    .mark_square = 13,
    .header_gap = 6,
    .heading_tracking = 1,
    .button_width = 60,
    .button_gap = 8,
    .focus_inset = 2,
    .small_line = 12,
    .regular_line = 16,
    .switch_width = 42,
    .switch_height = 24,
    .knob_width = 7,
    .knob_height = 12,
    .track_height = 4,
    .track_offset = 4,
    .stop_height = 2,
    .stop_offset = 12,
    .least_stop_spacing = 4,
    .choice_text_inset = 6,
    .choice_arrow_room = 16,
    .choice_arrow_width = 7,
    .choice_arrow_height = 4,
    .choice_item_height = 16,
    .most_shown_choices = 8,
    .choice_item_text_inset = 12,
    .padlock_width = 5,
    .padlock_height = 7,
    .padlock_gap = 3,
    .scroll_thumb_width = 5,
    .least_thumb_height = 16,
    .button_icon_inset = 3,
    .button_icon_press = 1,
    .button_square_numerator = 20,
    .button_square_denominator = 32,
    .large_mark_margin = 2,
    .list_row_badge = 28,
    .short_list_row_height = 28,
    .list_row_inset = 4,
    .list_row_title_top = 1,
    .list_row_line_top = 15,
    .list_row_gap = 6,
    .hairline = 1,
    .badge_dash = 2,
    .badge_dash_gap = 1,
    .chip_height = 12,
    .chip_padding = 4,
    .chip_gap = 4,
    .badge_height = 22,
    .state_icon_side = 14,
    .tab_padding = 10,
    .tab_tracking = 1,
    .tab_rule = 2,
    .tab_count_height = 12,
    .tab_count_padding = 3,
    .tab_count_gap = 4,
    .field_inset = 8,
    .magnifier_side = 7,
    .caret_width = 1,
    .card_least_width = 120,
    .card_gap = 6,
    .card_inset = 4,
    .hover_card_padding = 6,
    .hover_card_arrow = 5,
    .text_top = 48,
    .paragraph_gap = 6,
    .text_bottom_gap = 10,
    .open_width = 96,
    .prompt_button_padding = 24,
    .progress_bar_height = 8,
    .row_padding = 8,
    .label_gap = 8,
    .hint_gap = 2,
    .tall_hint_line_gap = 3,
    .most_hint_lines = 2,
    .most_notice_lines = 4,
    .slider_gap = 4,
    .slider_line_height = 14,
    .slider_value_width = 110,
    .slider_value_gap = 10,
    .lock_width = 148,
    .choice_width = 200,
    .wide_choice_width = 248,
    .button_row_width = 76,
    .folder_button_widths = {46, 82, 44},
    .folder_button_gap = 4,
};

/// Compact metrics: a settings dialog of 480 by 324 points, a nav list 144
/// points wide, 50 characters to a host's line under a row, and notices and
/// prompts 400 points wide and at most 440 tall; controls 28 points high,
/// list rows 36, text fields 30, filter chips 24 and tab strips 32.
inline constexpr Metrics compact_metrics = [] {
    Metrics sized = scaled(shared_metrics, compact_scale);
    sized.dialog_width = 480;
    sized.dialog_height = 324;
    sized.nav_width = 144;
    sized.hint_line_characters = 50;
    sized.notice_width = 400;
    sized.least_notice_height = 150;
    sized.greatest_notice_height = 440;
    sized.language_card_width = 139;
    sized.language_card_height = 182;
    sized.carousel_step = 132;
    return sized;
}();

/// Regular metrics: a settings dialog of 720 by 486 points, a nav list 176
/// points wide, 82 characters to a host's line under a row, and notices and
/// prompts 520 points wide and at most 500 tall; controls 34 points high,
/// list rows 40, text fields 36, filter chips 26 and tab strips 38. The
/// language card is 186 by 244, the design's card at four fifths, and the
/// carousel steps 177 points.
inline constexpr Metrics regular_metrics = [] {
    Metrics sized = scaled(compact_metrics, regular_scale);
    sized.dialog_width = 720;
    sized.dialog_height = 486;
    sized.nav_width = 176;
    sized.hint_line_characters = 82;
    sized.notice_width = 520;
    sized.greatest_notice_height = 500;
    sized.language_card_width = 186;
    sized.language_card_height = 244;
    sized.carousel_step = 177;
    return sized;
}();

/// Large metrics: a settings dialog of 960 by 600 points, a nav list 208
/// points wide, 114 characters to a host's line under a row, and notices
/// and prompts 600 points wide and at most 640 tall; controls 38 points
/// high, list rows 44, text fields 40, filter chips 28 and tab strips 44.
/// The language card is 232 by 304, the design's card, and the carousel
/// steps 236 points, the design's step.
inline constexpr Metrics large_metrics = [] {
    Metrics sized = scaled(compact_metrics, large_scale);
    sized.dialog_width = 960;
    sized.dialog_height = 600;
    sized.nav_width = 208;
    sized.hint_line_characters = 114;
    sized.notice_width = 600;
    sized.greatest_notice_height = 640;
    sized.language_card_width = 232;
    sized.language_card_height = 304;
    sized.carousel_step = 236;
    return sized;
}();

/// Returns the radius of a control's corners from its height: the radius
/// of the class whose controls are that tall, and for a control shorter
/// than Compact's, Compact's radius in proportion, at least 2 points.
///
/// @param height the control's height, in points
/// @return the radius, in points
[[nodiscard]] constexpr int32_t control_radius_for(int32_t height) noexcept {
    if (height >= large_metrics.button_height)
        return large_metrics.control_radius;
    if (height >= regular_metrics.button_height)
        return regular_metrics.control_radius;
    if (height >= compact_metrics.button_height)
        return compact_metrics.control_radius;
    return std::max(
        height * compact_metrics.control_radius / compact_metrics.button_height, int32_t{2}
    );
}

/// Returns the radius of a keycap's corners from its height: Compact's
/// keycap radius, held to a third of the keycap's height.
///
/// @param height the keycap's height, in points
/// @return the radius, in points
[[nodiscard]] constexpr int32_t keycap_radius_for(int32_t height) noexcept {
    return std::clamp(height / 3, int32_t{1}, compact_metrics.keycap_radius);
}

} // namespace oa::ui::kit
