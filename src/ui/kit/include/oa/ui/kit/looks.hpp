// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// How a kit component looks. A look owns its texts, so a display list
// outlives the words it was built from. Nothing here draws, and nothing
// here looks a word up: a caller passes the caption it wants shown.
#pragma once

#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/tag_icon.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace oa::ui::kit {

/// A rectangle in points. One point is one pixel of the 640 by 480 picture.
/// The same rectangle the settings geometry uses, so it passes straight through.
using Rect = oa::ui::frontend_renderer::SourceRect;

/// A one-bit mark the kit draws.
enum class Mark : uint8_t {
    oa_small,     ///< the OA letters, 9 by 7
    oa_large,     ///< the OA letters, 12 by 9
    padlock,      ///< the lock's padlock, 5 by 7
    arrow_closed, ///< an arrow pointing right, 5 by 5
    arrow_open,   ///< an arrow pointing down, 5 by 5
    choice_arrow, ///< a drop-down's arrow, 7 by 4
    magnifier,    ///< a search field's magnifier, 7 by 7
    key,          ///< a signing key, 7 by 7
};

/// Which of the button looks a button is drawn in. Every look has rounded
/// corners, their radius the control radius of the button's height, so a
/// button is never pill-shaped: a pill is a label (PillLook, ChipLook). One
/// hierarchy serves every OA screen: the primary button once in a view, for
/// the way forward; the secondary for every other action, at a control's
/// height in a footer or a toolbar and at a row's smaller height beside a
/// row; the danger look for a risky one; the quiet look only for a
/// low-emphasis text link.
enum class ButtonStyle : uint8_t {
    accent, ///< the primary button, lime with dark text: OK, Save changes, Close
    /// the secondary button: a raised face a step lighter than the surfaces
    /// it sits on, a clear one-point border, lighter under the pointer and
    /// pressed in while held: Add registry…, Cancel, More, Uninstall, a row's Choose…
    plain,
    /// a text link in lime, with no face at rest: Show the log, Discard,
    /// Skip this version, and a header's close and refresh marks
    quiet,
    inset, ///< the list's face at rest: a card's ground, not an action
    /// the deep green face with white text, over a picture: Mutators
    filled,
    danger, ///< outlined in the danger red, its caption red: Uninstall, Roll back…
};

/// What a button draws in place of its caption.
enum class ButtonMark : uint8_t {
    none,    ///< the caption
    refresh, ///< a circular arrow, crisp at the button's size
    close,   ///< an ×, crisp at the button's size, in the text colour
};

/// A button's caption and which of its looks to draw.
struct ButtonLook {
    std::string caption{};                 ///< the words, already looked up
    ButtonStyle style{ButtonStyle::plain}; ///< which look
    bool hovered{};                        ///< the pointer is over it, as the caller counts that
    bool held{};                           ///< a press is held on it with the pointer over it
    bool enabled{true};                    ///< false draws the plain look's disabled face
    ButtonMark mark{ButtonMark::none};     ///< a mark in place of the caption
    /// How far a refresh arrow has turned, in eighths of a turn, 0 to 7.
    /// One eighth is refresh_revolution_ms / 8 milliseconds.
    uint8_t turn{};
    /// The refresh arrow is turning: it stays in the text colour while the
    /// button is disabled, so the turn can be seen. A new turn clears the check.
    bool turning{};
    /// How long a finished refresh has shown its check, in milliseconds.
    /// Zero is the arrow. The kit eases the arrow into the check over
    /// refresh_ease_ms and holds the check until refresh_check_ms.
    uint16_t check_ms{};
    /// An accent button drawn in accent_light while it is not held: the
    /// call to action's bright face. Off leaves every button as it was.
    bool bright{false};
    /// The OA badge at the button's left and the caption after it, keeping
    /// to its left edge (button_mark_parts places them). Off centres the caption.
    bool oa_mark{false};
    /// The caption's font: the small font unless the caller picks the regular one.
    FontRole font{FontRole::small};
    /// A refresh arrow before the caption, in the caption's colour, the two
    /// centred together: a row's small Refresh. Off draws the caption alone.
    bool refresh_icon{false};
};

/// An Off/On switch. The captions are already looked up.
struct SwitchLook {
    bool on{};                 ///< the switch is On
    bool hovered{};            ///< the pointer is over it
    bool locked{};             ///< it cannot be changed: On shows without the accent
    std::string off_caption{}; ///< the Off half's words
    std::string on_caption{};  ///< the On half's words
};

/// A radio: one of a set of choices, of which one is chosen. With no
/// caption it is the mark alone: a ring, and the accent's dot in it while it
/// is the chosen one. With a caption it is a radio row: the mark at the
/// row's left and the caption beside it, on a rounded box whose edge is the
/// accent while it is chosen. The caption is already looked up and fitted
/// to radio_parts' caption box.
struct RadioLook {
    bool on{};             ///< it is the chosen one: the dot shows
    bool hovered{};        ///< the pointer is over it
    bool held{};           ///< a press is held on it with the pointer over it
    bool disabled{};       ///< it takes nothing: drawn in the idle colours
    std::string caption{}; ///< a radio row's words; empty draws the mark alone
};

/// A strip of levels. captions are already looked up, one per level from the left.
struct LevelsLook {
    std::vector<std::string> captions{}; ///< each level's words
    int32_t level_width{};               ///< each level's columns, inside the 1-pixel border
    std::size_t chosen{};                ///< the chosen level's index
    /// How many levels from the left can be chosen. The rest are faded. The
    /// caller passes the count, never a sentinel.
    std::size_t offered{};
    bool hovered{}; ///< the pointer is over the strip
    bool locked{};  ///< it cannot be changed: the chosen level shows without the accent
};

/// A slider's knob on its track.
struct SliderLook {
    int32_t stop{};  ///< the stop the knob is on
    int32_t stops{}; ///< the stops, 2 or more
    bool locked{};   ///< the knob cannot be moved
    bool hovered{};  ///< the pointer is over it, or holds it
};

/// A drop-down's closed field.
struct ChoiceLook {
    std::string text{}; ///< the choice it shows, already looked up
    bool hovered{};     ///< the pointer is over it, or holds it
    bool open{};        ///< its menu is open
};

/// One item of a drop-down's open menu, beside the text it shows.
///
/// A menu whose items vector is empty is every item an ordinary choice: no
/// aside, choosable, and named by the control's parts. items is one entry per
/// choice, from 0, not only the ones on screen.
struct ChoiceMenuRow {
    std::string aside{};  ///< right-aligned text; empty for none
    std::string id{};     ///< the automation word; empty on a divider, which is not named
    bool divider{false};  ///< a heading, not a choice: no focus, no press, no name
    bool rule{false};     ///< a divider with no words: a line, not a heading
    bool choosable{true}; ///< a press on it chooses it; a greyed row is not
};

/// A drop-down's open menu. shown holds the items on screen, already looked up,
/// the first of them being item first of total.
struct ChoiceMenuLook {
    std::vector<std::string> shown{}; ///< the items on screen, top to bottom
    /// One entry per choice, from 0. Empty when every item is an ordinary choice.
    std::vector<ChoiceMenuRow> items{};
    int32_t first{};    ///< the index of shown's first item
    int32_t total{};    ///< how many choices the menu offers
    int32_t chosen{};   ///< the chosen choice's index
    int32_t marked{};   ///< the item the pointer or the keys mark
    bool focus_shown{}; ///< the keyboard focus outlines the marked item
    /// A choice's height. 0 keeps every row choice_item_height, as before.
    int32_t row_height{};
    /// Points above a heading that is not the menu's first item. 0 for none.
    int32_t heading_gap{};
    /// A rule's height, when row_height is set. 0 draws a rule as a choice row.
    int32_t rule_height{};
};

/// A lock's text, already looked up. The padlock is drawn with it.
struct LockLook {
    std::string text{}; ///< the words
};

/// A scroll bar's thumb, and whether the pointer is on the bar or holds it.
struct ScrollBarLook {
    Rect thumb{}; ///< the thumb
    bool hot{};   ///< the pointer is over the bar, or a press on it is held
};

/// A one-bit mark's picture and colour.
struct MarkLook {
    Mark mark{};     ///< which mark
    Colour colour{}; ///< the colour of its set pixels
};

/// How the OA button looks.
enum class OaButtonState : uint8_t {
    idle,    ///< at rest
    hovered, ///< the pointer is over it
    pressed, ///< a press on it is held
};

/// What the OA button draws in its bottom-right corner.
enum class OaButtonCorner : uint8_t {
    none,    ///< nothing there
    count,   ///< a number, "999+" above 999 (oa_button_count_text)
    spinner, ///< the number is not final yet; turn names which place is lit
};

/// What the OA button wears besides its face: the attention dot, and the
/// bottom-right corner. A default mark draws the face alone.
struct OaButtonMark {
    bool dot{};                                  ///< the attention dot in the top right
    OaButtonCorner corner{OaButtonCorner::none}; ///< the bottom-right corner
    std::size_t count{};                         ///< the number, when the corner is a count
    uint32_t turn{};                             ///< which spinner place is lit, from 0
};

/// The OA button's side and how it looks. It is drawn at the item's top left.
/// The corner, count and turn are the bottom-right mark (OaButtonMark); left
/// at their defaults, the button is drawn as it was with only the dot.
struct OaButtonLook {
    int32_t side{};                              ///< the square's side, in points
    OaButtonState state{OaButtonState::idle};    ///< how it looks
    bool dot{};                                  ///< it wears the attention dot (draw_oa_button)
    OaButtonCorner corner{OaButtonCorner::none}; ///< the bottom-right corner
    std::size_t count{};                         ///< the number, when the corner is a count
    uint32_t turn{};                             ///< which spinner place is lit, from 0
};

/// Where the keyboard focus outline is drawn.
struct FocusRingLook {
    bool around{true}; ///< false draws it on the control's own edge
};

/// The text a player types into a field, and where the caret stands.
///
/// The text is UTF-8. The caret counts bytes from the text's start and stands
/// on a character's first byte or at the text's end. insert_text and
/// edit_text (input.hpp) keep both so.
struct TextField {
    std::string text{};  ///< the player's own text, in UTF-8; never looked up
    std::size_t caret{}; ///< the caret, in bytes from the start, on a character boundary
};

/// What a chip says, which chooses its colours.
enum class ChipState : uint8_t {
    get,       ///< can be fetched: the hint on the hover border
    installed, ///< is in place: the accent
    update,    ///< has an update: the lock colour
    playing,   ///< is in use: filled with the accent
    online,    ///< is on the network: the online colour
    problem,   ///< something is wrong: the danger colour
    filter,    ///< a filter the player turns on and off: the list's selected face while on
};

/// A state icon: a shape the kit draws at the canvas's own pixels, never
/// from a picture or a one-bit mark, so that it is as crisp at 2x, 3x and
/// 150% as at 1x (draw_state_icon). The four action icons are rings and
/// strokes in one colour. The OA badge and the bone are filled shapes in
/// their own colours.
enum class StateIcon : uint8_t {
    circled_check,    ///< a check in a ring: installed and up to date
    circled_up_arrow, ///< an arrow pointing up in a ring: installed, with an update
    download_arrow,   ///< an arrow pointing down into a tray: not installed, to get
    key,              ///< a key, bow and shaft and teeth, with no ring: a registry's signing key
    oa,               ///< the OA badge: a rounded plate and the letters, in their own colours
    bone,             ///< a dog bone: off-white with a dark outline, in its own colours
};

/// A maker's mark drawn under a list row's state icon, or on a registry card.
enum class MakerMark : uint8_t {
    open_annihilation, ///< the OA badge
    bone,              ///< the bone
};

/// A chip: a short text in a box, in the colours of its state. The text is
/// drawn as given, already looked up and in the case the caller wants. A
/// chip with an icon draws the icon in place of its outline and text, and
/// keeps its text for automation.
struct ChipLook {
    std::string text{};              ///< the words
    ChipState state{ChipState::get}; ///< which colours
    bool selected{};                 ///< a filter that is on
    bool hovered{};                  ///< the pointer is over it, or holds it
    bool disabled{};                 ///< it cannot act: drawn in the idle colour whatever its state
    /// The state icon drawn in place of the outline and the text,
    /// state_icon_side points square; none draws the text.
    std::optional<StateIcon> icon{};
};

/// What a pill says, which chooses its colours: a status badge's. A pill is
/// a word on a fully rounded ground; a state's pill is its colour's tinted
/// fill under words in that colour. Active and inactive are the selectable
/// pair: active is drawn as playing, inactive as neutral.
enum class PillState : uint8_t {
    playing,     ///< accent ground, on_accent text
    neutral,     ///< a muted ground, text colour: built in
    installed,   ///< the accent, as a chip's installed state
    update,      ///< the lock colour, as a chip's update state
    problem,     ///< the danger colour, as a chip's problem state
    active,      ///< chosen, as playing: accent ground, on_accent text
    inactive,    ///< not chosen, as neutral: a muted ground, text colour
    information, ///< the information blue's tint and words: a release offered
    brand,       ///< the brand gold's tint and words: a Core Prime badge
};

/// A pill: a short text on a fully rounded ground about chip_height high.
/// The text is drawn as given, already looked up. Hover, focus and disabled
/// are drawn on the pill itself.
struct PillLook {
    std::string text{};                  ///< the words
    PillState state{PillState::neutral}; ///< which colours
    bool hovered{};                      ///< the pointer is over it, or holds it
    bool focused{};                      ///< the keyboard focus is on it
    bool disabled{};                     ///< it cannot act: muted ground, idle text
    /// A state icon at the pill's right, inside the capsule, beside the text.
    /// None draws the text alone. The text stays; the icon does not replace it.
    std::optional<StateIcon> icon{};
    /// A tag icon at the pill's left, inside the capsule, before the word,
    /// in the pill's text colour. None leaves the word where it was.
    std::optional<TagIcon> tag{};
};

/// A row of a list, in its full or its short form, which its rectangle's
/// height chooses. Every text is already looked up.
struct ListRowLook {
    /// The badge's letters, drawn on badge_colour when there is no picture.
    std::string badge_text{};
    Colour badge_colour{}; ///< the badge's face behind its letters
    /// The badge's picture. It refers to pixels the screen keeps, which must
    /// outlive the display list. Empty, the letters or a blank badge show.
    oa::ui::frontend_renderer::RgbaPicture badge{};
    /// A tag icon drawn on badge_colour when the picture is not drawable.
    /// It takes the place of the letters. None leaves the letters, or the
    /// dashed square when there are none.
    std::optional<TagIcon> badge_tag{};
    std::string title{}; ///< the title, in the regular font
    std::string by{};    ///< the byline after the title, in the small font
    /// The byline's colour. Unset draws it in the hint colour, as a row
    /// with no missing prerequisite does.
    std::optional<Colour> by_colour{};
    std::vector<std::string> lines{}; ///< the lines under the title, top to bottom
    std::vector<std::string> aside{}; ///< the entries at the right, one a line from the top
    std::optional<ChipLook> chip{};   ///< the chip at the right; none when unset
    /// A maker's mark in the state-icon column, under the state icon when
    /// the row has one. None when unset. The row's height does not grow.
    std::optional<MakerMark> maker{};
    std::vector<ChipLook> tags{}; ///< chips on the line after the last line
    bool selected{};              ///< the row is the chosen one: the accent tint and outline
    /// The chosen row drawn as a nav entry: the list's selected fill and the
    /// lime bar, and no accent outline. The keyboard focus keeps its own ring.
    bool selection_bar{false};
    bool hovered{}; ///< the pointer is over it, or holds it
    /// False draws no badge. The title then starts `lead` points after the
    /// left inset, so a mark column can sit there and the names still line up.
    bool show_badge{true};
    int32_t lead{0}; ///< points kept before the title when show_badge is false
};

/// A text field: the plain one, or with the magnifier the search field.
struct SearchLook {
    /// Shown in the hint colour while the text is empty, already looked up.
    std::string placeholder{};
    TextField field{};    ///< the text and the caret
    bool focused{};       ///< the field takes the keys: its caret shows
    bool hovered{};       ///< the pointer is over it
    bool magnifier{true}; ///< the magnifier before the text, as the search field has
};

/// How a tab shows its count.
enum class TabCountStyle : uint8_t {
    pill,  ///< a count above 0 in an amber pill; 0, or none given, draws no count
    words, ///< the count as digits beside the caption, 0 included; a negative count draws none
};

/// A strip of tabs, left to right. Captions are already looked up.
struct TabsLook {
    std::vector<std::string> captions{};            ///< each tab's words
    std::vector<int32_t> counts{};                  ///< each tab's count; see count_style
    std::size_t selected{};                         ///< the open tab's index
    std::optional<std::size_t> hovered{};           ///< the tab under the pointer; none when unset
    TabCountStyle count_style{TabCountStyle::pill}; ///< how a count is drawn
    /// 1 draws that tab's word-style digits in the lock colour when its count
    /// is above 0. Empty, the digits take the caption's colour.
    std::vector<uint8_t> count_amber{};
    /// A tab's automation text when set. Empty, the text is the caption and,
    /// for a count above 0, a space and the count.
    std::vector<std::string> readings{};
    /// How far the strip is scrolled, in points from the first tab. The
    /// layout clamps it to the strip and, while keep_selected_in_view is set,
    /// so the open tab lies wholly in the strip. 0 draws the strip from its
    /// start, and a strip whose tabs fit ignores it.
    int32_t scroll{};
    /// Keeps the open tab wholly in the strip. Off, the scroll stays where
    /// the player left it with an arrow, the wheel or a drag, anywhere from
    /// the first tab to the last, and the open tab may lie out of view.
    bool keep_selected_in_view{true};
    /// The tab drawing area's left, in window points, after an arrow. 0
    /// until the strip is placed.
    int32_t view_x{};
    /// The tab drawing area's width, in points. 0 until the strip is placed.
    int32_t view_width{};
    bool left_arrow_hovered{};  ///< the pointer is over the arrow at the left
    bool right_arrow_hovered{}; ///< the pointer is over the arrow at the right
    /// The points either side of a caption. Negative uses the theme's tab_padding.
    int32_t side_padding{-1};
    /// The points between a caption's characters. Negative uses the theme's tab_tracking.
    int32_t tracking{-1};
};

/// A card of a grid: a square preview, its chips, a title and a subtitle.
struct CardLook {
    /// The preview's picture. It refers to pixels the screen keeps, which
    /// must outlive the display list. Empty, the placeholder shows.
    oa::ui::frontend_renderer::RgbaPicture picture{};
    std::string placeholder{};     ///< the words on the empty preview, already looked up
    std::string title{};           ///< the title, already looked up
    std::string subtitle{};        ///< the line under it, already looked up
    std::vector<ChipLook> chips{}; ///< chips at the preview's bottom left, left to right
    bool selected{};               ///< the chosen card: an accent outline
    bool hovered{};                ///< the pointer is over it: a lighter outline
    /// Faded into the panel, as a locked row is. It still takes a press.
    bool greyed{};
};

/// An edge of a rectangle.
enum class Side : uint8_t {
    none,   ///< no edge
    left,   ///< the left edge
    right,  ///< the right edge
    top,    ///< the top edge
    bottom, ///< the bottom edge
};

/// One line of a hover card: a label and its value.
struct HoverCardRow {
    /// The label, already looked up. Empty, the value is drawn from the left.
    std::string label{};
    std::string value{};               ///< the value, already looked up
    Colour value_colour{colour::text}; ///< the value's colour
};

/// A hover card: a header, label and value rows, a block of lines and an
/// arrow pointing at what it describes. Every text is already looked up.
struct HoverCardLook {
    int32_t width{}; ///< the card's width, in points; 0 fits its widest line
    /// The header starts with the OA mark: the logo when the canvas carries
    /// the icon, otherwise the small one-bit mark.
    bool mark{};
    std::string title{};              ///< the header's title
    std::string aside{};              ///< the header's words at the right
    std::vector<HoverCardRow> rows{}; ///< the rows under the header
    std::vector<std::string> block{}; ///< the quiet lines under the rows
    Side arrow{Side::none};           ///< the edge the arrow leaves from; none draws no arrow
    /// The arrow's point, in points along that edge from its start.
    int32_t arrow_at{};
};

/// A link: a caption that acts when pressed.
struct LinkLook {
    std::string caption{}; ///< the words, already looked up
    bool hovered{};        ///< the pointer is over it, or holds it
    bool enabled{true};    ///< false draws it in the idle colour, and it takes no press
};

/// A progress bar: a well with the accent filled as far as it has come.
struct ProgressLook {
    int32_t done{};  ///< how far it has come, from 0 to whole
    int32_t whole{}; ///< the whole; 0 or less fills nothing
    bool paused{};   ///< the work is paused: the fill is amber, not lime
};

/// A picture of the screen's own, scaled to fill the item's rectangle, such
/// as a mod's badge.
struct PictureLook {
    /// The picture. It refers to pixels the screen keeps, which must outlive
    /// the display list.
    oa::ui::frontend_renderer::RgbaPicture picture{};
};

/// What a language card's pill says. The words are the caller's; the state
/// chooses the pill's colours and, for offline, dims the card.
enum class LanguageAvailability : uint8_t {
    built_in,    ///< built into the game: a neutral pill
    get,         ///< a pack that can be fetched: a neutral pill
    downloading, ///< a fetch in progress: a neutral pill, the bar drawn under the card
    checking,    ///< a fetch being checked: a neutral pill
    active,      ///< the language in use: an accent pill, and an accent badge
    failed,      ///< a fetch that did not finish: a problem pill
    offline,     ///< the catalogue cannot be read: a neutral pill, and the card dimmed
};

/// A language card: a badge, the language's own name, its English name, a
/// facts line and a pill. Progress is not part of the card.
struct LanguageCardLook {
    /// The badge's letters, drawn when badge has no picture.
    std::string letters{};
    /// The pack's badge. It refers to pixels the screen keeps. Empty, the
    /// letters are drawn on a grey ground, accent while the card is active.
    oa::ui::frontend_renderer::RgbaPicture badge{};
    std::string own_name{};     ///< the language's own name, in the regular font
    std::string english_name{}; ///< its English name, in the small font
    std::string facts{};        ///< the facts line, in the small font
    std::string pill{};         ///< the pill's words, already looked up
    LanguageAvailability availability{LanguageAvailability::built_in}; ///< the pill's colours
    bool hovered{};                                                    ///< the pointer is over it
    bool held{};                                                       ///< a press is held on it
    bool disabled{};                                                   ///< it takes nothing
};

/// One card of a carousel, drawn turned. The words are already looked up.
struct CarouselCard {
    std::string letters{};
    std::string own_name{};
    std::string english_name{};
    std::string facts{};
    std::string pill{};
    LanguageAvailability availability{LanguageAvailability::built_in};
    bool hovered{};
    bool held{};
    bool disabled{};
};

/// A carousel of language cards. The item's rectangle is the band the cards
/// and the pager share. The arrow buttons are the caller's own buttons.
struct CarouselLook {
    int32_t card_width{};              ///< each card's width, in points, before it shrinks
    int32_t card_height{};             ///< each card's height, in points, before it shrinks
    int32_t step{};                    ///< the points from one card's centre to the next
    int32_t centre_milli{};            ///< where the centre sits, in thousandths of a card index
    bool reduced_motion{false};        ///< slides flat: no turn, no shrink and no fall
    std::vector<CarouselCard> cards{}; ///< the cards, in order from the left of the ring
    /// The ring joins. The card before the first is the last, so two cards
    /// stand on each side of a selection at either end. Off, the row is a line.
    bool wrap{false};
    /// The chosen card, from 0: the one the ring marks once it faces the
    /// player. -1 marks the card nearest the centre.
    int32_t chosen{-1};
};

/// A step indicator: a letter-spaced line, and one segment a step.
///
/// The line is `pattern` with `{step}`, `{index}` and `{count}` filled.
/// An empty pattern is the English `{step} · {index} OF {count}`. The caller
/// looks the pattern up whole before it is stored here.
struct StepLook {
    std::string name{};    ///< the step's name, already looked up, such as "LANGUAGE"
    int32_t index{1};      ///< the current step, from 1
    int32_t count{1};      ///< how many steps
    std::string pattern{}; ///< the line's pattern, already looked up; empty is the English one
};

/// The setup window's header and footer. It spans the window the width and
/// height name, not a dialog. The words are already looked up.
struct SetupChromeLook {
    int32_t width{};         ///< the window's width, in points
    int32_t height{};        ///< the window's height, in points
    std::string page{};      ///< the muted word after the title
    std::string step_name{}; ///< the step indicator's name, already looked up
    /// The step line's pattern, already looked up. Empty is the English
    /// `{step} · {index} OF {count}`.
    std::string step_pattern{};
    int32_t step_index{1};          ///< the current step, from 1
    int32_t step_count{1};          ///< how many steps
    std::string skip{"Skip setup"}; ///< the quiet button on the left
    std::string back{"Back"};       ///< the plain button
    std::string next{"Next"};       ///< the right-hand button
    /// Pause is plain. Continue, Next and Copy the game are the accent.
    ButtonStyle next_style{ButtonStyle::accent};
    bool skip_hovered{};
    bool skip_held{};
    bool skip_enabled{true};
    bool back_hovered{};
    bool back_held{};
    bool back_enabled{true};
    bool next_hovered{};
    bool next_held{};
    bool next_enabled{true};
    /// The header shows the mark and "Open Annihilation" before the page
    /// word. Off, as in a window whose own title bar names the game, the
    /// page word stands at the header's left.
    bool brand{true};
};

/// A choice card: a radio row with a title and a hint.
struct ChoiceCardLook {
    std::string title{}; ///< the title, in the regular font
    std::string hint{};  ///< the line under it, in the small font
    bool chosen{};       ///< the radio is filled and the border is the accent
    bool hovered{};      ///< the pointer is over it
    bool held{};         ///< a press is held on it
    bool disabled{};     ///< it takes nothing, and shows no accent
};

/// A coach mark: a numbered card with an accent border. Its rectangle is the
/// card. The connector runs from the card to target, which may lie outside it.
struct CoachMarkLook {
    int32_t number{1};   ///< the mark's number, drawn before the title
    std::string title{}; ///< the title, in the regular font
    std::string body{};  ///< the lines under the title, in the small font
    Rect target{};       ///< the rectangle the connector runs to, in the same points as the card
};

/// A spotlight under a coach mark. The item's rectangle is the layer that
/// dims; cutout is the rectangle left clear, with an accent ring.
struct SpotlightLook {
    Rect cutout{}; ///< the clear rectangle, in the same points as the layer
};

/// A picture well: a one-point border, an inset, and a caption under it.
/// The picture is not part of the look: the caller draws it into the inset.
struct PictureWellLook {
    std::string caption{}; ///< the line under the well, in the small font
    /// A stand-in pattern in the inset, so a sample can show the well with
    /// no picture of its own. Off, the inset is left for the caller.
    bool stand_in{false};
};

/// A call to action: an accent button that brightens and sends out a ring.
/// The item's rectangle is the button. The ring is drawn a few points outside it.
struct CallToActionLook {
    std::string caption{};      ///< the button's words, already looked up
    bool hovered{};             ///< the pointer is over it
    bool held{};                ///< a press is held on it
    bool enabled{true};         ///< false draws the disabled button and no ring
    uint32_t time_ms{};         ///< how long the pulse has been running, in milliseconds
    bool reduced_motion{false}; ///< holds a steady ring instead of sending one out
};

/// A package badge: a square picture, a tag icon, letters, or a blank.
/// Drawn in that order. The picture, when drawable, has a control_border
/// outline. Otherwise a tag's icon is drawn in on_accent on face, its
/// square 70 percent of the badge's side and centred. Otherwise the letters
/// are drawn on face, cut to the whole letters that fit, as a list row cuts
/// them, with no ellipsis. Otherwise the badge is a dashed blank square.
struct BadgeLook {
    oa::ui::frontend_renderer::RgbaPicture picture{}; ///< the picture; empty draws none
    std::optional<TagIcon> tag{};                     ///< the tag's icon, when there is no picture
    std::string letters{};     ///< the letters, when there is no picture or tag
    Colour face{colour::hint}; ///< the ground behind a tag icon or the letters
};

/// A drag handle: two columns of four dots. The rectangle it is drawn in
/// is grip_width by grip_height. Disabled wins over held, and held over hovered.
struct GripLook {
    bool hovered{};  ///< the pointer is over it
    bool held{};     ///< a press is held on it
    bool disabled{}; ///< it cannot be dragged
};

/// A drag handle's width, in points, at every size class.
inline constexpr int32_t grip_width = 6;

/// A drag handle's height, in points, at every size class.
inline constexpr int32_t grip_height = 12;

/// A key's category tint. none is an untinted key. The letter on a tinted
/// key is the caller's.
enum class KeyTint : uint8_t {
    none,      ///< no category
    orders,    ///< orders
    pointer,   ///< the pointer
    selection, ///< selection
    groups,    ///< groups
    camera,    ///< the camera
    building,  ///< building
    game,      ///< the game
    chat,      ///< chat
    window,    ///< the window
    hacks,     ///< a mod's hacks
    mod,       ///< a mod's data
    developer, ///< developer
};

/// A mark on a key. Each has its own shape.
enum class KeyMark : uint8_t {
    none,    ///< no mark
    changed, ///< a filled diamond
    clash,   ///< a triangle, and a dashed border
    unbound, ///< a hollow ring
    fixed,   ///< an open square, and a dashed border
};

/// A keycap. The label is what the key prints, the short name sits along
/// the bottom, and the letter sits in a corner square. small is the 9-point
/// badge: its label alone, in the accent on on_accent.
struct KeycapLook {
    std::string label{};         ///< what the key prints
    std::string sub{};           ///< the short name along the bottom
    KeyTint tint{KeyTint::none}; ///< the category; none is untinted
    std::string letter{};        ///< the category letter in the corner
    KeyMark mark{KeyMark::none}; ///< the corner mark
    bool modifier{false};        ///< a modifier, in the button text colour when untinted
    bool held{false};            ///< a press is held on it
    bool hovered{false};         ///< the pointer is over it
    bool focused{false};         ///< the keyboard focus is on it
    bool disabled{false};        ///< it takes nothing
    bool small{false};           ///< the 9-point corner badge
    /// The mark alone, centred, with no key face. none draws nothing, so a
    /// column of marks stays blank where a row has none.
    bool mark_only{false};
};

/// A chord chip: keycaps joined by "+", then a gesture word. add draws the
/// dashed add form, with keys empty. Its caption is add_caption, or
/// "+ Add key" when that is empty. none_text after that caption is a quiet
/// note. none_text alone, with add clear, is the unbound text.
struct ChordChipLook {
    std::vector<std::string> keys{}; ///< the keycaps' labels, in order
    std::string gesture{};           ///< the gesture word after the keycaps
    KeyMark mark{KeyMark::none};     ///< the mark on the last keycap
    bool selected{false};            ///< the chip is chosen
    bool hovered{false};             ///< the pointer is over it
    bool add{false};                 ///< the dashed add form
    std::string add_caption{};       ///< the add form's words; empty draws "+ Add key"
    std::string none_text{};         ///< a quiet note on the add form, or the unbound text
};

/// Which keyboard a picture draws.
enum class KeyboardShape : uint8_t {
    ansi, ///< ANSI
    iso,  ///< ISO, with IntlBackslash and a tall Enter
    jis,  ///< JIS, with IntlYen, IntlRo and a shorter space
};

/// One key of a keyboard picture that is not left free.
struct KeyboardKey {
    std::string code{}; ///< the W3C code
    KeycapLook cap{};   ///< what that key shows; an empty label keeps the geometry's
};

/// A keyboard picture. A code with no entry is a free key with the
/// geometry's own label.
struct KeyboardLook {
    KeyboardShape shape{KeyboardShape::ansi}; ///< which keyboard
    std::vector<KeyboardKey> keys{};          ///< the keys that are not free
    /// Only the main block: the rows from Tab to Enter, Caps Lock to Enter
    /// and Shift to Shift, scaled to the rectangle on their own, so a narrow
    /// picture keeps keys large enough for their legends. Off draws the
    /// whole keyboard.
    bool main_block{false};
};

/// A mouse button, in the order MouseLook::buttons stores their looks.
enum class MouseButton : uint8_t {
    left,       ///< the left button
    right,      ///< the right button
    middle,     ///< the middle button
    wheel_up,   ///< the wheel turned up
    wheel_down, ///< the wheel turned down
    back,       ///< the back side button
    forward,    ///< the forward side button
    button6,    ///< an extra button
    button7,    ///< an extra button
    button8,    ///< an extra button
};

/// A mouse picture. buttons is one look per MouseButton. extra_buttons
/// draws buttons 6, 7 and 8.
struct MouseLook {
    std::array<KeycapLook, 10> buttons{}; ///< one look per MouseButton
    bool extra_buttons{false};            ///< draws the three extra buttons
};

/// A hold ring filled clockwise from the top to done/whole. reduced_motion
/// snaps the fill down to a quarter of the ring.
struct HoldRingLook {
    int32_t done{0};            ///< how far the ring has filled
    int32_t whole{1};           ///< the full ring
    bool reduced_motion{false}; ///< snap the fill down to quarters
};

/// What a component item draws. Empty for a generic role. Later components
/// append looks. A locked fade has no look of its own: its rectangle is the
/// item's.
using Look = std::variant<
    std::monostate,
    ButtonLook,
    SwitchLook,
    LevelsLook,
    SliderLook,
    ChoiceLook,
    ChoiceMenuLook,
    LockLook,
    ScrollBarLook,
    MarkLook,
    OaButtonLook,
    FocusRingLook,
    HeaderLook,
    FooterBandLook,
    NavLook,
    HeadingLook,
    RowFrame,
    ListRowLook,
    ChipLook,
    SearchLook,
    TabsLook,
    CardLook,
    HoverCardLook,
    LinkLook,
    ProgressLook,
    PictureLook,
    PillLook,
    LanguageCardLook,
    CarouselLook,
    StepLook,
    SetupChromeLook,
    ChoiceCardLook,
    CoachMarkLook,
    SpotlightLook,
    PictureWellLook,
    CallToActionLook,
    BadgeLook,
    GripLook,
    KeycapLook,
    ChordChipLook,
    KeyboardLook,
    MouseLook,
    HoldRingLook,
    RadioLook>;

} // namespace oa::ui::kit
