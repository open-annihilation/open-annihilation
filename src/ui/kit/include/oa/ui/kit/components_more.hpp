// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// A notice and a question: a box in the window's chrome that tells the
// player something, with a button that opens a folder and OK, or asks with
// one to three buttons of its own, and the progress bar a question can show
// under its text. A work dialog is that box with a title, one line and a
// bar or a spinner, and no buttons: it tells the player what a long moment
// is for and closes on its own. Their models, where their parts lie, their
// display lists, what a pointer, a finger and the keys do to a notice and a
// question, and their drawing. Texts arrive as they are to be shown; a
// notice's pass through the look-up its caller gives, when it gives one.
#pragma once

#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::kit {

/// A notice's or a question's width at Compact, in points. Each size class
/// has its own (Metrics::notice_width).
inline constexpr int32_t notice_width = compact_metrics.notice_width;
/// A notice's or a question's least height, in points; it grows with its text.
inline constexpr int32_t least_notice_height = compact_metrics.least_notice_height;
/// A notice's or a question's greatest height at Compact, in points: what the
/// 640 by 480 picture holds with a margin. Text below it is cut. Each size
/// class has its own (Metrics::greatest_notice_height).
inline constexpr int32_t greatest_notice_height = compact_metrics.greatest_notice_height;

/// A notice's OK, which closes it.
inline constexpr ControlId notice_ok = 0;
/// A notice's button that opens its folder.
inline constexpr ControlId notice_open = 1;
/// A notice's or a question's text as a control that takes no press, which
/// automation reads: past every button's number.
inline constexpr ControlId notice_body = 3;

/// The word a notice is named by unless it gives its own (Notice::word).
inline constexpr std::string_view notice_word = "notice";
/// The word a question is named by unless it gives its own (Question::word).
inline constexpr std::string_view question_word = "prompt";

/// The most buttons a question has. Its buttons are numbered from 0, left to right.
inline constexpr std::size_t most_question_buttons = 3;
/// A question's full progress bar (Question::progress).
inline constexpr int32_t question_progress_whole = 1000;

/// The header mark's place in a notice or a question, as the window's header
/// places it.
inline constexpr Rect notice_mark{
    compact_metrics.padding,
    compact_metrics.mark_top,
    compact_metrics.mark_side,
    compact_metrics.mark_side,
};

/// A paragraph of a notice's or a question's text.
struct Paragraph {
    /// UTF-8. A new line starts a line of its own, and the rest wraps
    /// between words. A path ignores that and wraps at its separators.
    std::string text{};
    /// A folder's path: drawn in the regular font and broken after a
    /// separator, a component too long for a line within it. Any other
    /// paragraph is drawn in the small font, a new line starting a line
    /// and the rest broken between words.
    bool path{};
};

/// One notice: it tells the player something once, with a folder's path, a
/// button that opens the folder and OK.
struct Notice {
    std::string title{};                 ///< the header's title, in capitals
    std::vector<Paragraph> paragraphs{}; ///< its text, top to bottom
    /// The caption of the button that opens the folder. Empty draws no
    /// second button and offers no control for it, so keys and Tab stay on OK.
    std::string open_caption{};
    /// Why the folder could not be opened, drawn in amber under the text;
    /// empty for none.
    std::string failure{};
    ControlId hovered{no_control}; ///< the button under the pointer
    ControlId pressed{no_control}; ///< the button a held press is on
    /// The button the keys mark and Space presses, ringed in the accent: OK
    /// at first.
    ControlId marked{notice_ok};
    /// The columns a finger's held press was moved by to reach the button it
    /// took (notice_finger_down): its moves and its release are moved as far.
    int32_t finger_shift_x{};
    int32_t finger_shift_y{}; ///< the rows a finger's held press was moved by, as finger_shift_x
    std::string ok_caption{"OK"}; ///< OK's caption
    /// The size class it is laid out at: its width and greatest height are
    /// the class's (metrics_of). Its text wraps at that width; its buttons
    /// and lines keep their sizes.
    SizeClass size_class{SizeClass::compact};
    /// Its name to automation, which its controls' names start with: a word
    /// of a-z, 0-9 and hyphens; any other is notice_word. Nothing draws it.
    std::string word{notice_word};
};

/// What an event on a notice asks of its host.
enum class NoticeAction : uint8_t {
    none,        ///< nothing
    redraw,      ///< only its look changed: a hover, a press or the mark
    open_folder, ///< open the notice's folder in the file manager; the notice stays
    closed,      ///< OK, Enter or Escape: the notice is done with
};

/// One of a question's buttons.
struct QuestionButton {
    std::string caption{}; ///< drawn as given
    /// Drawn as a notice's OK is, in the accent: the answer the question leads to.
    bool accent{};
    /// The button's name to automation: a word of a-z, 0-9 and hyphens, or
    /// empty for a name by its place. Nothing draws it.
    std::string id{};
};

/// One line of a question's list: a badge, a name and an aside in the hint colour.
struct QuestionItem {
    BadgeLook badge{};   ///< the line's badge, 20 points square
    std::string text{};  ///< the name, drawn in the text colour
    std::string aside{}; ///< drawn after the name, in the hint colour; empty for none
};

/// One question: a notice's look that asks with one to most_question_buttons
/// buttons of its own, and can show a progress bar under its text. Its text
/// is given finished and drawn as given. A title wider than the header is
/// cut between words, and the cut ends with an ellipsis.
struct Question {
    std::string title{};                 ///< the header's title; cut when it does not fit
    std::vector<Paragraph> paragraphs{}; ///< its text, top to bottom, as given
    /// Lines under the paragraphs, each a badge and a name. Empty leaves the
    /// question laid out as it was before the lines existed.
    std::vector<QuestionItem> items{};
    std::vector<QuestionButton> buttons{}; ///< 1 to most_question_buttons, left to right
    int32_t cancel_button{};               ///< the button Escape and N answer
    int32_t primary_button{};              ///< the button Y answers
    /// Why something it asked for failed, drawn in amber under the text;
    /// empty for none.
    std::string failure{};
    /// A bar under the text, 0 to question_progress_whole filled; -1 for none.
    int32_t progress{-1};
    ControlId hovered{no_control}; ///< the button under the pointer
    ControlId pressed{no_control}; ///< the button a held press is on
    /// The button the keys mark, which Enter and Space answer, ringed in the accent.
    ControlId marked{};
    /// The columns a finger's held press was moved by to reach the button it
    /// took (question_finger_down); its moves and its release are moved as far.
    int32_t finger_shift_x{};
    int32_t finger_shift_y{}; ///< the rows, as finger_shift_x
    /// The size class it is laid out at, as a notice's (Notice::size_class).
    SizeClass size_class{SizeClass::compact};
    /// Its name to automation, which its controls' names start with: a word
    /// of a-z, 0-9 and hyphens; any other is question_word. Nothing draws it.
    std::string word{question_word};
};

/// What an event on a question asks of its host.
enum class QuestionAction : uint8_t {
    none,     ///< nothing
    redraw,   ///< only its look changed: a hover, a press or the mark
    answered, ///< a button was pressed: QuestionAnswer::button says which
};

/// What an event on a question did.
struct QuestionAnswer {
    QuestionAction action{QuestionAction::none}; ///< what the host does
    int32_t button{-1};                          ///< the button answered, from 0; -1 when none was
};

/// One line of a notice's or a question's text, placed.
struct PlacedLine {
    std::string text{}; ///< the line, UTF-8, as its paragraph gave it
    bool path{};        ///< drawn in the regular font, as a path
    bool failure{};     ///< drawn in amber: the failure's line
    Rect rect{};        ///< the line's box, in points from the box's top left corner
};

/// A notice, placed, in points from its top left corner.
struct PlacedNotice {
    int32_t height{};                ///< the notice's height
    Rect title{};                    ///< the title's place in the header
    std::vector<PlacedLine> lines{}; ///< the lines that fit, top to bottom
    int32_t footer_rule{};           ///< the row of the line over the footer
    Rect open_button{};              ///< the button that opens the folder
    Rect ok_button{};                ///< OK, at the footer's right
};

/// One question item, placed, in points from the question's top left corner.
struct PlacedQuestionItem {
    Rect badge{};        ///< the 20-point badge
    BadgeLook look{};    ///< what the badge draws
    Rect text{};         ///< the name and aside, the small font centred on the badge
    std::string shown{}; ///< the line as it fits, the name then the aside
    /// The bytes of shown that are the name. The rest is the aside. When the
    /// cut lands inside the name, this is the whole shown line, ellipsis included.
    std::size_t text_bytes{};
};

/// A question, placed, in points from its top left corner.
struct PlacedQuestion {
    int32_t height{};                ///< the question's height
    Rect title{};                    ///< the title's place in the header
    std::vector<PlacedLine> lines{}; ///< the lines that fit, top to bottom
    bool cut{};                      ///< a line of its text, an item, or its bar, did not fit
    Rect bar{};                      ///< the progress bar; empty for none
    int32_t footer_rule{};           ///< the row of the line over the footer
    std::vector<Rect> buttons{};     ///< its buttons, left to right
    /// The items that fit, under the paragraphs and above the failure. Empty
    /// when the question has none.
    std::vector<PlacedQuestionItem> items{};
};

/// Places a notice at its size class (Notice::size_class): its text wrapped
/// under the header at the class's width, paths in the regular font broken
/// at their separators and the rest in the small font, a new line starting a
/// line and the rest broken between words, paragraph_gap rows between
/// paragraphs and the failure last; its height from its text, from the
/// class's least to its greatest height, a line that height cuts left out;
/// and OK at the footer's right, with the open button before it when the
/// notice has an open caption.
///
/// @param notice the notice
/// @param regular a text's width in the regular font
/// @param small a text's width in the small font
/// @return the placed notice
[[nodiscard]] PlacedNotice
place_notice(const Notice& notice, const Measure& regular, const Measure& small);

/// Places a notice in its fonts, or at estimated widths without them.
///
/// @param notice the notice
/// @param fonts the fonts it is drawn in; null to estimate widths (estimated_width)
/// @return the placed notice
[[nodiscard]] PlacedNotice place_notice(const Notice& notice, const Fonts* fonts);

/// Returns a question's buttons' places, right-aligned in the footer of a box
/// of a height and of its size class's width, button_gap columns apart. Each
/// is as wide as its caption and prompt_button_padding more, at least
/// button_width. A caption counts as the wider of its estimated_width and its
/// width in the small font, so a caption the font draws wider than the
/// estimate keeps the padding, and one it draws narrower keeps the place the
/// estimate gives it. A row wider than the room between the box's paddings is
/// narrowed, its widest buttons first, until it fits. Buttons past
/// most_question_buttons are not placed.
///
/// @param question the question
/// @param height the question's height
/// @param small a caption's width in the small font
/// @return the buttons' rectangles, left to right
[[nodiscard]] std::vector<Rect>
question_button_rects(const Question& question, int32_t height, const Measure& small);

/// Returns a question's buttons' places in its fonts, or at estimated widths
/// without them. A press is tested on these places, so the pointer and finger
/// functions below take the fonts the question is drawn in, and a press lands
/// where a button is drawn.
///
/// @param question the question
/// @param height the question's height
/// @param fonts the fonts it is drawn in; null to estimate widths (estimated_width)
/// @return the buttons' rectangles, left to right
[[nodiscard]] std::vector<Rect>
question_button_rects(const Question& question, int32_t height, const Fonts* fonts = nullptr);

/// Places a question: its text as a notice's, then its items when it has
/// any (one a line, a 20-point badge, a gap and the name with the aside,
/// cut with the kit's ellipsis), the failure under them, its progress bar
/// paragraph_gap rows under that, its height from all of it, and its buttons
/// (question_button_rects). With no items the text, the bar and the height
/// are placed as before. A line, an item or a bar the height cuts is left
/// out, and found cut.
///
/// @param question the question
/// @param regular a text's width in the regular font
/// @param small a text's width in the small font
/// @return the placed question
[[nodiscard]] PlacedQuestion
place_question(const Question& question, const Measure& regular, const Measure& small);

/// Places a question in its fonts, or at estimated widths without them.
///
/// @param question the question
/// @param fonts the fonts it is drawn in; null to estimate widths (estimated_width)
/// @return the placed question
[[nodiscard]] PlacedQuestion place_question(const Question& question, const Fonts* fonts);

/// Returns the word a notice is named by to automation.
///
/// @param notice the notice
/// @return its word when that is a word of a-z, 0-9 and hyphens; else notice_word
[[nodiscard]] std::string_view notice_word_of(const Notice& notice) noexcept;

/// Returns the word a question is named by to automation.
///
/// @param question the question
/// @return its word when that is a word of a-z, 0-9 and hyphens; else question_word
[[nodiscard]] std::string_view question_word_of(const Question& question) noexcept;

/// Returns a notice's text as it is shown: the caller's look-up of a text in
/// the language shown. The kit never looks a text up itself.
using LookUp = std::function<std::string_view(std::string_view)>;

/// Returns a notice's display list: its face, its header with the title, its
/// lines, its footer band, the open button and OK each followed by the focus
/// ring when the keys mark it, and its edge, in that order. Its controls are
/// OK and, when open_caption is not empty, the open button, tested in that
/// order and named <word>.ok and <word>.open (its sound word, notice_word_of),
/// then its text. Tab goes left to right, the open button then OK, or OK
/// alone when there is no open button. Its text is the control notice_body,
/// named <word>.body, of kind area, neither enabled nor focusable, so that no
/// press, finger or key reaches it: it lies over the title and the lines that
/// fit, and its text is the title, the paragraphs and the failure, when there
/// is one, one a line, as shown.
///
/// @param notice the notice
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @param look_up how the title, the lines and the captions are shown; unset shows them as given
/// @return the display list, in points from the notice's top left corner
[[nodiscard]] DisplayList
notice_list(const Notice& notice, const Fonts* fonts, const LookUp& look_up = {});

/// Returns a question's display list: its face, its header with the title,
/// its lines, its items' badges and text, its footer band, its progress bar,
/// each button left to right followed by the focus ring when the keys mark
/// it, and its edge, in that order. Its controls are its buttons, numbered,
/// tested and in Tab order left to right, each named <word>.<id> (its sound
/// word, question_word_of), or <word>.button-1 to <word>.button-3 by its
/// place from the left when its id is empty or not a word of a-z, 0-9 and
/// hyphens; then its text, as a notice's is (notice_list), named <word>.body.
/// That text lists each item's name and aside, one line, after the paragraphs
/// and before the failure. With no items the list is the one it was.
///
/// @param question the question
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return the display list, in points from the question's top left corner
[[nodiscard]] DisplayList question_list(const Question& question, const Fonts* fonts);

/// Moves the pointer over a notice: the button under it lights. While a
/// press is held, the point is moved by the finger's shift first.
///
/// @param[in,out] notice the notice
/// @param at the pointer, in points from the notice's top left corner
/// @param height the notice's height (PlacedNotice::height)
/// @return NoticeAction::redraw when the look changed, else none
[[nodiscard]] NoticeAction notice_pointer_move(Notice& notice, Point at, int32_t height);

/// Presses the pointer's button on a notice: a press on a button holds it;
/// anywhere else it does nothing.
///
/// @param[in,out] notice the notice
/// @param at the pointer, in points from the notice's top left corner
/// @param height the notice's height
/// @return NoticeAction::redraw when a button is held, else none
[[nodiscard]] NoticeAction notice_pointer_down(Notice& notice, Point at, int32_t height);

/// Presses a notice with a finger: as notice_pointer_down at the point reach
/// gives, OK tried before the open button. The press's moves and its release
/// are then moved as far as the press was, so a release where the finger
/// landed presses the button it took.
///
/// @param[in,out] notice the notice
/// @param finger the finger, in points from the notice's top left corner
/// @param height the notice's height
/// @param reach how far a button may lie from the finger, in points
/// @return NoticeAction::redraw when a button is held, else none
[[nodiscard]] NoticeAction
notice_finger_down(Notice& notice, Point finger, int32_t height, int32_t reach);

/// Releases the pointer's button on a notice: a release over the button the
/// press held presses it, OK closing the notice and the other asking for the
/// folder. A release elsewhere only redraws.
///
/// @param[in,out] notice the notice
/// @param at the pointer, in points from the notice's top left corner
/// @param height the notice's height
/// @return what the release asks of the host
[[nodiscard]] NoticeAction notice_pointer_up(Notice& notice, Point at, int32_t height);

/// Takes a key on a notice: Enter and Escape close it, Space presses the
/// marked button, and Left, Right, Up, Down, Tab and Shift+Tab move the mark
/// to the other button. With no open caption the mark stays on OK, and those
/// keys do nothing. Any other key does nothing.
///
/// @param[in,out] notice the notice
/// @param key the key
/// @return what the key asks of the host
[[nodiscard]] NoticeAction notice_key(Notice& notice, Key key);

/// Moves the pointer over a question: the button under it lights. While a
/// press is held, the point is moved by the finger's shift first.
///
/// @param[in,out] question the question
/// @param at the pointer, in points from the question's top left corner
/// @param height the question's height (PlacedQuestion::height)
/// @param fonts the fonts it is drawn in, which place its buttons
///        (question_button_rects); null to estimate widths
/// @return a redraw when the look changed
[[nodiscard]] QuestionAnswer
question_pointer_move(Question& question, Point at, int32_t height, const Fonts* fonts = nullptr);

/// Presses the pointer's button on a question: a press on a button holds it.
///
/// @param[in,out] question the question
/// @param at the pointer, in points from the question's top left corner
/// @param height the question's height
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return a redraw when a button is held
[[nodiscard]] QuestionAnswer
question_pointer_down(Question& question, Point at, int32_t height, const Fonts* fonts = nullptr);

/// Presses a question with a finger: as question_pointer_down at the point
/// reach gives, the buttons tried left to right. The press's moves and its
/// release are moved as far as the press was.
///
/// @param[in,out] question the question
/// @param finger the finger, in points from the question's top left corner
/// @param height the question's height
/// @param reach how far a button may lie from the finger, in points
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return a redraw when a button is held
[[nodiscard]] QuestionAnswer question_finger_down(
    Question& question, Point finger, int32_t height, int32_t reach, const Fonts* fonts = nullptr
);

/// Releases the pointer's button on a question: a release over the button
/// the press held answers it.
///
/// @param[in,out] question the question
/// @param at the pointer, in points from the question's top left corner
/// @param height the question's height
/// @param fonts the fonts it is drawn in; null to estimate widths
/// @return the answer, or a redraw
[[nodiscard]] QuestionAnswer
question_pointer_up(Question& question, Point at, int32_t height, const Fonts* fonts = nullptr);

/// Takes a key on a question: Enter and Space answer the marked button,
/// Escape and No the cancel button, Yes the primary button; Left, Up and
/// Shift+Tab mark the button before, Right, Down and Tab the one after,
/// round to the other end. Any other key does nothing, and so does every key
/// on a question without buttons.
///
/// @param[in,out] question the question
/// @param key the key
/// @return the answer, a redraw, or nothing for a key it does not take
[[nodiscard]] QuestionAnswer question_key(Question& question, Key key);

/// Draws a notice at the canvas's origin: the window's face, its header with
/// the header mark and the title in the regular font, the text in the text
/// colour with paths in the regular font, the failure in amber, the footer
/// band with the open button as a plain button and OK as an accent one, the
/// marked one ringed, and the window's edge. It is placed in the canvas's
/// fonts, or at estimated widths without them.
///
/// @param canvas where it is drawn; its icon is the header mark's
/// @param notice the notice
/// @param look_up how the title, the lines and the captions are shown; unset shows them as given
void draw_notice(const Canvas& canvas, const Notice& notice, const LookUp& look_up = {});

/// Draws a question at the canvas's origin in a notice's look: its header
/// and title, its text, the failure in amber, its progress bar, and its
/// buttons right-aligned in the footer, an accent one as OK looks and the
/// others plain, the marked one ringed. Its texts are drawn as given.
///
/// @param canvas where it is drawn; its icon is the header mark's
/// @param question the question
void draw_question(const Canvas& canvas, const Question& question);

/// Draws a progress bar: a rounded track of the selected surface, filled
/// in the accent, or in amber while the work is paused, from its left as
/// far as done is of whole, rounded down, the fill rounded as the track is.
/// done is held to 0 to whole. A whole of 0 or less, or an empty rectangle,
/// fills nothing; an empty rectangle draws nothing at all.
///
/// @param canvas where it is drawn
/// @param bar the bar
/// @param done how far it has come
/// @param whole the whole
/// @param paused true while the work is paused
void draw_progress(
    const Canvas& canvas, const Rect& bar, int32_t done, int32_t whole, bool paused = false
);

/// Appends a progress bar. It adds no control.
///
/// @param[in,out] list the display list
/// @param bar the bar
/// @param done how far it has come
/// @param whole the whole
/// @param paused true while the work is paused
void add_progress(
    DisplayList& list, const Rect& bar, int32_t done, int32_t whole, bool paused = false
);

/// The word a work dialog is named by. Its text's control is `<word>.body`.
inline constexpr std::string_view work_dialog_word = "progress";

/// The spinner's side, in points, when a work dialog does not know how far
/// the work has come.
inline constexpr int32_t work_spinner_side = 28;

/// One work dialog: a notice's panel and header, one line, and either a
/// progress bar or a spinner. It has no buttons. Its texts are given
/// finished and drawn as given.
struct WorkDialog {
    std::string title{}; ///< the header's title, as given
    std::string line{};  ///< the one line under the header, as given
    /// A bar under the line, 0 to question_progress_whole filled. -1 draws
    /// the spinner instead (work_spinner_side).
    int32_t progress{-1};
    /// Which of the spinner's eight places is brightest (draw_spinner).
    /// Ignored while a bar is shown.
    uint32_t turn{};
    /// The size class it is laid out at, as a notice's (Notice::size_class).
    SizeClass size_class{SizeClass::compact};
};

/// A work dialog, placed, in points from its top left corner.
struct PlacedWorkDialog {
    int32_t height{};  ///< the dialog's height
    Rect title{};      ///< the title's place in the header
    PlacedLine line{}; ///< the one line
    Rect bar{};        ///< the progress bar; empty when the spinner is shown
    Rect spinner{};    ///< the spinner; empty when the bar is shown
};

/// Places a work dialog at its size class: the title in the header, the line
/// fitted to one row under it, and the bar or the spinner just under that
/// line. Its height is that content plus the gap under the text and the
/// edge, and no taller than the class's greatest notice height.
///
/// @param dialog the dialog
/// @param measure a text's width in the small font
/// @return the placed dialog
[[nodiscard]] PlacedWorkDialog place_work_dialog(const WorkDialog& dialog, const Measure& measure);

/// Places a work dialog in its fonts, or at estimated widths without them.
///
/// @param dialog the dialog
/// @param fonts the fonts it is drawn in; null to estimate widths (estimated_width)
/// @return the placed dialog
[[nodiscard]] PlacedWorkDialog place_work_dialog(const WorkDialog& dialog, const Fonts* fonts);

/// Returns a work dialog's display list: the window's face, its header, the
/// line, the bar when the amount is known, and the window's edge. The text
/// is one control, `<progress>.body` unless the word changes, of kind area,
/// neither enabled nor focusable. The Tab order is empty. The spinner is
/// not a role of the list: draw_work_dialog draws it. No button is added.
///
/// @param dialog the dialog
/// @param fonts the fonts it is placed in; null to estimate widths
/// @return the display list
[[nodiscard]] DisplayList work_dialog_list(const WorkDialog& dialog, const Fonts* fonts);

/// Draws a work dialog at the canvas's origin: the window's face, its header
/// with the header mark and the title, the line in the small font, the
/// progress bar or the spinner, and the window's edge. It has no footer and
/// no buttons. The spinner is drawn directly (draw_spinner), not as a role.
///
/// @param canvas where it is drawn; its icon is the header mark's
/// @param dialog the dialog
void draw_work_dialog(const Canvas& canvas, const WorkDialog& dialog);

} // namespace oa::ui::kit
