// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kit's notice, question and work dialog: where six notices and six
// questions place their parts, their display lists' names, kinds and Tab
// order, their words and their text's control, which no press reaches,
// where a finger's press lands, every key (the editing keys among them),
// paint against a direct draw, the progress bar's fill, and the work
// dialog, which has no buttons. A question's buttons are also fitted to a
// measure and to fonts wider than the estimate, and a row too wide for its
// box is narrowed to the room.
//
// The placements and the finger's landings below are literal values,
// computed once with the settings dialog's own notice and prompt placement
// (place_notice and place_prompt) and its notice_finger_down and
// prompt_finger_down, at the commit that moved them into the kit.

#include "oa/formats/fnt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace renderer = oa::ui::frontend_renderer;

// --- Synthetic drawing -------------------------------------------------------

renderer::Surface black(int32_t width, int32_t height) {
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(width);
    surface.height = static_cast<uint32_t>(height);
    surface.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    return surface;
}

/// The synthetic font from the settings pixel test: glyph b is 2 + b % 4
/// columns by 8 rows, set where (x + y + byte) % 3 is not 0.
///
/// @param columns every glyph's width instead; 0 for 2 + b % 4
/// @return the font as both the regular and the small one
kit::Fonts block_fonts(uint16_t columns = 0) {
    renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = columns != 0 ? columns : static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 0);
        for (uint16_t y = 0; y < height; ++y) {
            for (uint16_t x = 0; x < width; ++x) {
                if ((static_cast<int>(x) + y + byte) % 3 != 0)
                    coverage[static_cast<std::size_t>(y) * width + x] = 1;
            }
        }
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

const kit::Fonts& fonts() {
    static const kit::Fonts loaded = block_fonts();
    return loaded;
}

renderer::RgbaPicture red_icon() {
    static const std::array<uint8_t, 16> pixels{
        255,
        0,
        0,
        255,
        255,
        0,
        0,
        255,
        255,
        0,
        0,
        255,
        255,
        0,
        0,
        255,
    };
    return {2, 2, pixels};
}

/// Checks two surfaces hold the same bytes, naming the first pixel that differs.
///
/// @param left a surface
/// @param right another
/// @param what what was drawn, for the report
void same(const renderer::Surface& left, const renderer::Surface& right, const char* what) {
    if (left.width == right.width && left.height == right.height && left.rgb == right.rgb)
        return;
    std::fprintf(stderr, "%s: the surfaces differ\n", what);
    for (std::size_t at = 0; at < left.rgb.size() && at < right.rgb.size(); ++at)
        if (left.rgb[at] != right.rgb[at]) {
            const std::size_t pixel = at / 3;
            std::fprintf(stderr, "  first at %zu,%zu\n", pixel % left.width, pixel / left.width);
            break;
        }
    OA_CHECK(false);
}

/// Returns a pixel's colour.
///
/// @param surface the surface
/// @param x its column
/// @param y its row
/// @return its colour, opaque
kit::Colour pixel(const renderer::Surface& surface, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
    return {surface.rgb[at], surface.rgb[at + 1], surface.rgb[at + 2]};
}

bool same_rect(const kit::Rect& a, const kit::Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// --- The notices and questions --------------------------------------------

/// Five points a character in the small font and six in the regular one.
int32_t narrow_small(std::string_view text) {
    return static_cast<int32_t>(kit::character_count(text)) * 5;
}

int32_t narrow_regular(std::string_view text) {
    return static_cast<int32_t>(kit::character_count(text)) * 6;
}

/// Nine points a character: wider than the estimate, as the modern fonts
/// draw Cyrillic capitals in the small font.
int32_t wide_small(std::string_view text) {
    return static_cast<int32_t>(kit::character_count(text)) * 9;
}

kit::Notice short_notice() {
    kit::Notice notice;
    notice.title = "SAVED";
    notice.paragraphs.push_back({"The game was saved.", false});
    notice.open_caption = "Open folder";
    return notice;
}

kit::Notice moved_notice() {
    kit::Notice notice;
    notice.title = "SAVED GAMES MOVED";
    notice.paragraphs = {
        {"3 saved games moved to:", false},
        {"/home/player/Documents/Open Annihilation/Saves", true},
        {"Screenshots, films and mods now go in the same Open Annihilation folder.", false},
    };
    notice.open_caption = "OPEN FOLDER";
    return notice;
}

kit::Notice long_notice() {
    kit::Notice notice;
    notice.title = "YOUR FILES";
    notice.open_caption = "Open folder";
    const std::string sentence =
        "Screenshots, films and mods now go in your own folder, and the game keeps them there.";
    notice.paragraphs.push_back({sentence + " " + sentence + " " + sentence, false});
    notice.paragraphs.push_back(
        {"Open the folder to see the saves, the screenshots and the mods.", false}
    );
    notice.paragraphs.push_back(
        {"The game makes the folder when it is missing, then shows it.", false}
    );
    notice.paragraphs.push_back(
        {"C:\\Users\\player\\Documents\\My Games\\Total Annihilation\\Saves", true}
    );
    notice.failure = "The folder could not be opened.";
    return notice;
}

kit::Notice wide_notice() {
    kit::Notice notice;
    notice.title = "建造";
    std::string text;
    for (int repeat = 0; repeat < 6; ++repeat)
        text += "建造完成，单位已就绪。";
    notice.paragraphs.push_back({text, false});
    notice.paragraphs.push_back(
        {"C:\\Documents and Settings\\A Rather Long User Name\\My Documents\\Open "
         "Annihilation\\Saves",
         true}
    );
    notice.open_caption = "打开";
    return notice;
}

kit::Notice cut_notice() {
    kit::Notice notice;
    notice.title = "A LONG NOTE";
    for (int paragraph = 0; paragraph < 40; ++paragraph)
        notice.paragraphs.push_back({"A line of the notice's text that takes a row.", false});
    notice.open_caption = "OPEN";
    return notice;
}

kit::Question question_of(const std::vector<std::string>& captions) {
    kit::Question question{};
    question.title = "UPDATE MOD";
    question.paragraphs.push_back({"Example Mod 1.0, revision 1, is installed.", false});
    question.paragraphs.push_back(
        {"/Users/someone/Documents/Open Annihilation/Mods/example-mod", true}
    );
    for (const auto& caption : captions)
        question.buttons.push_back({caption, false});
    question.buttons.back().accent = true;
    question.cancel_button = 0;
    question.primary_button = static_cast<int32_t>(captions.size()) - 1;
    question.marked = question.primary_button;
    return question;
}

kit::Question progress_question() {
    kit::Question question = question_of({"CANCEL"});
    question.title = "INSTALL MOD";
    question.progress = 500;
    return question;
}

kit::Question bar_cut_question() {
    kit::Question question{};
    question.title = "A ROW EACH";
    for (int paragraph = 0; paragraph < 20; ++paragraph)
        question.paragraphs.push_back({"A row.", false});
    question.buttons.push_back({"OK", true});
    question.progress = 250;
    return question;
}

kit::Question four_question() {
    kit::Question question{};
    for (int paragraph = 0; paragraph < 40; ++paragraph)
        question.paragraphs.push_back({"A line of the prompt's text that takes a row.", false});
    question.failure = "The mod could not be read.";
    question.buttons = {
        {"LATER", false},
        {"CANCEL", false},
        {"A VERY LONG CAPTION FOR A BUTTON", false},
        {"OK", true},
    };
    question.progress = 0;
    return question;
}

// --- Placement -----------------------------------------------------------

/// A line where the settings dialog placed it.
struct LineAt {
    std::string_view text;
    bool path{};
    bool failure{};
    kit::Rect rect{};
};

/// A notice as the settings dialog placed it.
struct NoticeAt {
    int32_t height{};
    kit::Rect title{};
    int32_t footer_rule{};
    kit::Rect open{};
    kit::Rect ok{};
    std::vector<LineAt> lines{};
};

/// A question as the settings dialog placed its prompt.
struct QuestionAt {
    int32_t height{};
    kit::Rect title{};
    bool cut{};
    kit::Rect bar{};
    int32_t footer_rule{};
    std::vector<kit::Rect> buttons{};
    std::vector<LineAt> lines{};
};

/// The same one-line paragraph, a row of the small font each, eighteen rows
/// apart from the text's first row.
///
/// @param text the paragraph
/// @param count how many lines
/// @return the lines
std::vector<LineAt> small_rows(std::string_view text, int32_t count) {
    std::vector<LineAt> lines;
    for (int32_t index = 0; index < count; ++index)
        lines.push_back({text, false, false, {12, 38 + 18 * index, 376, 12}});
    return lines;
}

void check_lines(const std::vector<kit::PlacedLine>& got, const std::vector<LineAt>& want) {
    OA_CHECK(got.size() == want.size());
    for (std::size_t index = 0; index < got.size() && index < want.size(); ++index) {
        OA_CHECK(got[index].text == want[index].text);
        OA_CHECK(got[index].path == want[index].path);
        OA_CHECK(got[index].failure == want[index].failure);
        OA_CHECK(same_rect(got[index].rect, want[index].rect));
    }
}

void check_notice(const kit::PlacedNotice& got, const NoticeAt& want) {
    OA_CHECK(got.height == want.height);
    OA_CHECK(same_rect(got.title, want.title));
    OA_CHECK(got.footer_rule == want.footer_rule);
    OA_CHECK(same_rect(got.open_button, want.open));
    OA_CHECK(same_rect(got.ok_button, want.ok));
    check_lines(got.lines, want.lines);
}

void check_question(const kit::PlacedQuestion& got, const QuestionAt& want) {
    OA_CHECK(got.height == want.height);
    OA_CHECK(same_rect(got.title, want.title));
    OA_CHECK(got.cut == want.cut);
    OA_CHECK(same_rect(got.bar, want.bar));
    OA_CHECK(got.footer_rule == want.footer_rule);
    OA_CHECK(got.buttons.size() == want.buttons.size());
    for (std::size_t index = 0; index < got.buttons.size() && index < want.buttons.size(); ++index)
        OA_CHECK(same_rect(got.buttons[index], want.buttons[index]));
    check_lines(got.lines, want.lines);
}

void notices_place_as_before() {
    const kit::Measure estimated = kit::estimated_width;
    check_notice(
        kit::place_notice(short_notice(), estimated, estimated),
        {150,
         {38, 1, 350, 26},
         116,
         {235, 124, 96, 17},
         {336, 124, 52, 17},
         {{"The game was saved.", false, false, {12, 38, 376, 12}}}}
    );
    check_notice(
        kit::place_notice(moved_notice(), estimated, estimated),
        {150,
         {38, 1, 350, 26},
         116,
         {235, 124, 96, 17},
         {336, 124, 52, 17},
         {
             {"3 saved games moved to:", false, false, {12, 38, 376, 12}},
             {"/home/player/Documents/Open Annihilation/Saves", true, false, {12, 56, 376, 16}},
             {"Screenshots, films and mods now go in the same Open",
              false,
              false,
              {12, 78, 376, 12}},
             {"Annihilation folder.", false, false, {12, 90, 376, 12}},
         }}
    );
    check_notice(
        kit::place_notice(long_notice(), narrow_regular, narrow_small),
        {206,
         {38, 1, 350, 26},
         172,
         {235, 180, 96, 17},
         {336, 180, 52, 17},
         {
             {"Screenshots, films and mods now go in your own folder, and the game keeps",
              false,
              false,
              {12, 38, 376, 12}},
             {"them there. Screenshots, films and mods now go in your own folder, and the",
              false,
              false,
              {12, 50, 376, 12}},
             {"game keeps them there. Screenshots, films and mods now go in your own",
              false,
              false,
              {12, 62, 376, 12}},
             {"folder, and the game keeps them there.", false, false, {12, 74, 376, 12}},
             {"Open the folder to see the saves, the screenshots and the mods.",
              false,
              false,
              {12, 92, 376, 12}},
             {"The game makes the folder when it is missing, then shows it.",
              false,
              false,
              {12, 110, 376, 12}},
             {"C:\\Users\\player\\Documents\\My Games\\Total Annihilation\\Saves",
              true,
              false,
              {12, 128, 376, 16}},
             {"The folder could not be opened.", false, true, {12, 150, 376, 12}},
         }}
    );
    check_notice(
        kit::place_notice(wide_notice(), estimated, estimated),
        {156,
         {38, 1, 350, 26},
         122,
         {235, 130, 96, 17},
         {336, 130, 52, 17},
         {
             {"建造完成，单位已就绪。建造完成，单位已就绪。建造完",
              false,
              false,
              {12, 38, 376, 12}},
             {"成，单位已就绪。建造完成，单位已就绪。建造完成，单位",
              false,
              false,
              {12, 50, 376, 12}},
             {"已就绪。建造完成，单位已就绪。", false, false, {12, 62, 376, 12}},
             {"C:\\Documents and Settings\\A Rather Long User Name\\",
              true,
              false,
              {12, 80, 376, 16}},
             {"My Documents\\Open Annihilation\\Saves", true, false, {12, 96, 376, 16}},
         }}
    );
    // Forty one-line paragraphs: the greatest height, and the lines under
    // the twentieth cut.
    check_notice(
        kit::place_notice(cut_notice(), estimated, estimated),
        {440,
         {38, 1, 350, 26},
         406,
         {235, 414, 96, 17},
         {336, 414, 52, 17},
         small_rows("A line of the notice's text that takes a row.", 20)}
    );
    // No open caption, so no open button.
    check_notice(
        kit::place_notice(kit::Notice{}, estimated, estimated),
        {150, {38, 1, 350, 26}, 116, {0, 0, 0, 0}, {336, 124, 52, 17}, {}}
    );
    // Without fonts, the estimated widths.
    check_notice(
        kit::place_notice(moved_notice(), nullptr),
        {150,
         {38, 1, 350, 26},
         116,
         {235, 124, 96, 17},
         {336, 124, 52, 17},
         {
             {"3 saved games moved to:", false, false, {12, 38, 376, 12}},
             {"/home/player/Documents/Open Annihilation/Saves", true, false, {12, 56, 376, 16}},
             {"Screenshots, films and mods now go in the same Open",
              false,
              false,
              {12, 78, 376, 12}},
             {"Annihilation folder.", false, false, {12, 90, 376, 12}},
         }}
    );
}

void questions_place_as_before() {
    const kit::Measure estimated = kit::estimated_width;
    const std::vector<LineAt> example{
        {"Example Mod 1.0, revision 1, is installed.", false, false, {12, 38, 376, 12}},
        {"/Users/someone/Documents/Open Annihilation/Mods/", true, false, {12, 56, 376, 16}},
        {"example-mod", true, false, {12, 72, 376, 16}},
    };
    check_question(
        kit::place_question(question_of({"OK"}), estimated, estimated),
        {150, {38, 1, 350, 26}, false, {}, 116, {{336, 124, 52, 17}}, example}
    );
    check_question(
        kit::place_question(question_of({"CANCEL", "REPLACE"}), estimated, estimated),
        {150, {38, 1, 350, 26}, false, {}, 116, {{260, 124, 58, 17}, {323, 124, 65, 17}}, example}
    );
    // The narrow measure keeps the path on one line; the buttons are placed
    // at the estimated width whatever the measure.
    check_question(
        kit::place_question(
            question_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"}), narrow_regular, narrow_small
        ),
        {150,
         {38, 1, 350, 26},
         false,
         {},
         116,
         {{120, 124, 58, 17}, {183, 124, 135, 17}, {323, 124, 65, 17}},
         {
             {"Example Mod 1.0, revision 1, is installed.", false, false, {12, 38, 376, 12}},
             {"/Users/someone/Documents/Open Annihilation/Mods/example-mod",
              true,
              false,
              {12, 56, 376, 16}},
         }}
    );
    check_question(
        kit::place_question(progress_question(), estimated, estimated),
        {150, {38, 1, 350, 26}, false, {12, 94, 376, 8}, 116, {{330, 124, 58, 17}}, example}
    );
    // Twenty rows fit; the bar under them does not, and is left out.
    check_question(
        kit::place_question(bar_cut_question(), estimated, estimated),
        {440, {38, 1, 350, 26}, true, {}, 406, {{336, 414, 52, 17}}, small_rows("A row.", 20)}
    );
    // Four buttons: the first three are placed.
    check_question(
        kit::place_question(four_question(), estimated, estimated),
        {440,
         {38, 1, 350, 26},
         true,
         {},
         406,
         {{28, 414, 52, 17}, {85, 414, 58, 17}, {148, 414, 240, 17}},
         small_rows("A line of the prompt's text that takes a row.", 20)}
    );
    // The buttons alone, at a height of 200.
    const auto rects = kit::question_button_rects(question_of({"CANCEL", "REPLACE"}), 200);
    OA_CHECK(rects.size() == 2);
    if (rects.size() == 2) {
        OA_CHECK(same_rect(rects[0], {260, 174, 58, 17}));
        OA_CHECK(same_rect(rects[1], {323, 174, 65, 17}));
    }
}

void question_buttons_fit_their_captions() {
    // A measure wider than the estimate widens a button to its caption and
    // the padding: the eleven characters of ЗАВАНТАЖИТИ are 99 points, and
    // BACK keeps the least width. At the estimate, 77 points, it was 93.
    const kit::Question load = question_of({"BACK", "ЗАВАНТАЖИТИ"});
    const auto measured = kit::question_button_rects(load, 200, wide_small);
    OA_CHECK(measured.size() == 2);
    if (measured.size() == 2) {
        OA_CHECK(same_rect(measured[0], {216, 174, 52, 17}));
        OA_CHECK(same_rect(measured[1], {273, 174, 115, 17}));
    }
    const auto estimated = kit::question_button_rects(load, 200);
    OA_CHECK(estimated.size() == 2);
    if (estimated.size() == 2) {
        OA_CHECK(same_rect(estimated[0], {238, 174, 52, 17}));
        OA_CHECK(same_rect(estimated[1], {295, 174, 93, 17}));
    }
    // place_question places them with its small measure.
    const kit::PlacedQuestion placed = kit::place_question(load, kit::estimated_width, wide_small);
    OA_CHECK(placed.buttons.size() == 2);
    if (placed.buttons.size() == 2)
        OA_CHECK(same_rect(placed.buttons[1], {273, 124, 115, 17}));

    // A row wider than the room between the paddings, 376 points at Compact,
    // is narrowed from its widest buttons: 268 and 205 come to 148 each, and
    // the row starts at the left padding.
    const kit::Question wide_row =
        question_of({"CANCEL", "INSTALL BESIDE THE ONE THERE", "REPLACE THE ONE THERE"});
    const auto narrowed = kit::question_button_rects(wide_row, 200, wide_small);
    OA_CHECK(narrowed.size() == 3);
    if (narrowed.size() == 3) {
        OA_CHECK(same_rect(narrowed[0], {12, 174, 70, 17}));
        OA_CHECK(same_rect(narrowed[1], {87, 174, 148, 17}));
        OA_CHECK(same_rect(narrowed[2], {240, 174, 148, 17}));
    }

    // In fonts wider than the estimate the display list's buttons, the
    // placement's and the presses agree: a press on the part of REPLACE
    // the estimate leaves out takes it only when the fonts are given; at
    // the estimate that point is CANCEL's.
    const kit::Fonts wide = block_fonts(10);
    kit::Question question = question_of({"CANCEL", "REPLACE"});
    const int32_t height = kit::place_question(question, &wide).height;
    const auto in_fonts = kit::question_button_rects(question, height, &wide);
    OA_CHECK(in_fonts.size() == 2);
    if (in_fonts.size() != 2)
        return;
    OA_CHECK(
        in_fonts[1].width == kit::text_width(wide, kit::FontRole::small, "REPLACE") +
                                 kit::compact_metrics.prompt_button_padding
    );
    OA_CHECK(in_fonts[1].width > kit::question_button_rects(question, height)[1].width);
    const kit::PlacedQuestion placed_in_fonts = kit::place_question(question, &wide);
    const kit::DisplayList list = kit::question_list(question, &wide);
    OA_CHECK(placed_in_fonts.buttons.size() == 2 && list.controls.size() >= 2);
    if (placed_in_fonts.buttons.size() == 2 && list.controls.size() >= 2) {
        OA_CHECK(same_rect(placed_in_fonts.buttons[1], in_fonts[1]));
        OA_CHECK(same_rect(list.controls[1].rect, in_fonts[1]));
        OA_CHECK(same_rect(list.controls[0].rect, in_fonts[0]));
    }
    const kit::Point left_part{in_fonts[1].x + 2, in_fonts[1].y + 2};
    kit::Question at_estimate = question;
    std::ignore = kit::question_pointer_down(at_estimate, left_part, height);
    OA_CHECK(at_estimate.hovered == 0);
    OA_CHECK(
        kit::question_pointer_down(question, left_part, height, &wide).action ==
        kit::QuestionAction::redraw
    );
    const kit::QuestionAnswer answered =
        kit::question_pointer_up(question, left_part, height, &wide);
    OA_CHECK(answered.action == kit::QuestionAction::answered && answered.button == 1);
    OA_CHECK(
        kit::question_pointer_move(question, left_part, height, &wide).action ==
        kit::QuestionAction::none
    );
    OA_CHECK(question.hovered == 1);
    const kit::Point under{left_part.x, in_fonts[1].y + in_fonts[1].height + 3};
    OA_CHECK(
        kit::question_finger_down(question, under, height, 6, &wide).action ==
        kit::QuestionAction::redraw
    );
    const kit::QuestionAnswer tapped = kit::question_pointer_up(question, under, height, &wide);
    OA_CHECK(tapped.action == kit::QuestionAction::answered && tapped.button == 1);
}

// --- Display lists -------------------------------------------------------

void check_control(
    const kit::DisplayList& list,
    std::size_t index,
    kit::ControlId id,
    std::string_view name,
    std::string_view text
) {
    OA_CHECK(index < list.controls.size());
    if (index >= list.controls.size())
        return;
    const kit::Control& control = list.controls[index];
    OA_CHECK(control.id == id);
    OA_CHECK(control.name == name);
    OA_CHECK(control.kind == kit::ControlKind::button);
    OA_CHECK(control.enabled);
    OA_CHECK(control.focusable);
    OA_CHECK(!control.steps);
    OA_CHECK(control.text == text);
}

/// Checks a list's text control: its number, name and kind, that no press
/// reaches it, that it lies over every line of the text, and its text.
///
/// @param list the list
/// @param index the control's place among the list's controls
/// @param name its name
/// @param text its text, one line each
void check_body(
    const kit::DisplayList& list, std::size_t index, std::string_view name, std::string_view text
) {
    OA_CHECK(index < list.controls.size());
    if (index >= list.controls.size())
        return;
    const kit::Control& body = list.controls[index];
    OA_CHECK(body.id == kit::notice_body);
    OA_CHECK(body.name == name);
    OA_CHECK(body.kind == kit::ControlKind::area);
    OA_CHECK(!body.enabled && !body.focusable && !body.steps);
    OA_CHECK(body.text == text);
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::text)
            OA_CHECK(kit::wholly_in(item.rect, body.rect));
    for (const kit::ControlId stop : list.tab_order)
        OA_CHECK(stop != kit::notice_body);
}

void lists_name_their_buttons() {
    // A notice: OK tested first, the open button second, Tab left to right,
    // and its text last.
    const kit::DisplayList notice = kit::notice_list(moved_notice(), nullptr);
    OA_CHECK(notice.controls.size() == 3);
    check_control(notice, 0, kit::notice_ok, "notice.ok", "OK");
    check_control(notice, 1, kit::notice_open, "notice.open", "OPEN FOLDER");
    check_body(
        notice,
        2,
        "notice.body",
        "SAVED GAMES MOVED\n3 saved games moved to:\n"
        "/home/player/Documents/Open Annihilation/Saves\n"
        "Screenshots, films and mods now go in the same Open Annihilation folder."
    );
    OA_CHECK((notice.tab_order == std::vector<kit::ControlId>{kit::notice_open, kit::notice_ok}));
    OA_CHECK(kit::name_problem(notice).empty());
    if (notice.controls.size() == 3) {
        OA_CHECK(same_rect(notice.controls[0].rect, {336, 124, 52, 17}));
        OA_CHECK(same_rect(notice.controls[1].rect, {235, 124, 96, 17}));
    }
    // A failure is the text's last line.
    const kit::DisplayList failed = kit::notice_list(long_notice(), nullptr);
    OA_CHECK(failed.controls.size() == 3);
    if (failed.controls.size() == 3) {
        const std::string& text = failed.controls[2].text;
        const std::string_view last = "\nThe folder could not be opened.";
        OA_CHECK(text.size() > last.size() && text.substr(text.size() - last.size()) == last);
    }

    // A question whose buttons have ids, and one whose buttons have none.
    kit::Question named = question_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"});
    named.buttons[0].id = "cancel";
    named.buttons[1].id = "alongside";
    named.buttons[2].id = "replace";
    const kit::DisplayList with_ids = kit::question_list(named, nullptr);
    OA_CHECK(with_ids.controls.size() == 4);
    check_control(with_ids, 0, 0, "prompt.cancel", "CANCEL");
    check_control(with_ids, 1, 1, "prompt.alongside", "INSTALL ALONGSIDE");
    check_control(with_ids, 2, 2, "prompt.replace", "REPLACE");
    const std::string question_text = "UPDATE MOD\nExample Mod 1.0, revision 1, is installed.\n"
                                      "/Users/someone/Documents/Open Annihilation/Mods/example-mod";
    check_body(with_ids, 3, "prompt.body", question_text);
    OA_CHECK((with_ids.tab_order == std::vector<kit::ControlId>{0, 1, 2}));
    OA_CHECK(kit::name_problem(with_ids).empty());

    const kit::DisplayList without =
        kit::question_list(question_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"}), nullptr);
    check_control(without, 0, 0, "prompt.button-1", "CANCEL");
    check_control(without, 1, 1, "prompt.button-2", "INSTALL ALONGSIDE");
    check_control(without, 2, 2, "prompt.button-3", "REPLACE");
    check_body(without, 3, "prompt.body", question_text);
    OA_CHECK((without.tab_order == std::vector<kit::ControlId>{0, 1, 2}));
    OA_CHECK(kit::name_problem(without).empty());

    // An id that is no such word is named by its place; a fourth button has
    // no control.
    kit::Question four = four_question();
    four.buttons[0].id = "Later!";
    four.buttons[1].id = "cancel";
    const kit::DisplayList mixed = kit::question_list(four, nullptr);
    OA_CHECK(mixed.controls.size() == 4);
    check_control(mixed, 0, 0, "prompt.button-1", "LATER");
    check_control(mixed, 1, 1, "prompt.cancel", "CANCEL");
    check_control(mixed, 2, 2, "prompt.button-3", "A VERY LONG CAPTION FOR A BUTTON");
    OA_CHECK(kit::name_problem(mixed).empty());
}

void a_word_of_their_own_names_them() {
    // The missing-language question gives its own word, which starts every
    // name, its buttons' with ids and without.
    kit::Question asked = question_of({"LATER", "DOWNLOAD"});
    asked.word = "language-notice";
    asked.buttons[1].id = "download";
    OA_CHECK(kit::question_word_of(asked) == "language-notice");
    const kit::DisplayList question = kit::question_list(asked, nullptr);
    OA_CHECK(question.controls.size() == 3);
    check_control(question, 0, 0, "language-notice.button-1", "LATER");
    check_control(question, 1, 1, "language-notice.download", "DOWNLOAD");
    if (question.controls.size() == 3)
        OA_CHECK(question.controls[2].name == "language-notice.body");
    OA_CHECK(kit::name_problem(question).empty());

    kit::Notice told = short_notice();
    told.word = "language-notice";
    OA_CHECK(kit::notice_word_of(told) == "language-notice");
    const kit::DisplayList notice = kit::notice_list(told, nullptr);
    check_control(notice, 0, kit::notice_ok, "language-notice.ok", "OK");
    check_control(notice, 1, kit::notice_open, "language-notice.open", "Open folder");
    check_body(notice, 2, "language-notice.body", "SAVED\nThe game was saved.");

    // A word that is not one word of a-z, 0-9 and hyphens gives way to the
    // kind's own.
    told.word = "Language Notice";
    OA_CHECK(kit::notice_word_of(told) == kit::notice_word);
    OA_CHECK(kit::notice_list(told, nullptr).controls[0].name == "notice.ok");
    asked.word = "";
    OA_CHECK(kit::question_word_of(asked) == kit::question_word);
    OA_CHECK(kit::question_list(asked, nullptr).controls[1].name == "prompt.download");
}

/// Returns a list without its text control.
///
/// @param list the list
/// @return the list, its controls but notice_body
kit::DisplayList without_body(kit::DisplayList list) {
    std::erase_if(list.controls, [](const kit::Control& control) {
        return control.id == kit::notice_body;
    });
    return list;
}

void the_text_control_takes_no_press() {
    // At every point of the box and round it, a press and a finger find what
    // they found without the text's control.
    const std::vector<kit::DisplayList> lists{
        kit::notice_list(short_notice(), nullptr),
        kit::notice_list(moved_notice(), nullptr),
        kit::question_list(question_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"}), nullptr),
        kit::question_list(progress_question(), nullptr),
    };
    for (const kit::DisplayList& list : lists) {
        const kit::DisplayList bare = without_body(list);
        OA_CHECK(bare.controls.size() + 1 == list.controls.size());
        for (int32_t y = -10; y < 200; y += 3)
            for (int32_t x = -10; x < kit::notice_width + 10; x += 3) {
                OA_CHECK(kit::hit(list, {x, y}) == kit::hit(bare, {x, y}));
                for (const int32_t within : {6, 22}) {
                    const kit::Reached reached = kit::reach(list, {x, y}, within);
                    const kit::Reached bare_reached = kit::reach(bare, {x, y}, within);
                    OA_CHECK(reached.control == bare_reached.control);
                    OA_CHECK(
                        reached.at.x == bare_reached.at.x && reached.at.y == bare_reached.at.y
                    );
                }
            }
    }
}

void lists_draw_in_order() {
    using kit::Role;
    kit::Notice notice = short_notice();
    notice.marked = kit::notice_open;
    std::vector<Role> roles;
    for (const kit::Item& item : kit::notice_list(notice, nullptr).items)
        roles.push_back(item.role);
    OA_CHECK(
        (roles == std::vector<Role>{
                      Role::fill,
                      Role::header,
                      Role::text,
                      Role::footer_band,
                      Role::button,
                      Role::focus_ring,
                      Role::button,
                      Role::bevel,
                  })
    );

    roles.clear();
    for (const kit::Item& item : kit::question_list(progress_question(), nullptr).items)
        roles.push_back(item.role);
    OA_CHECK(
        (roles == std::vector<Role>{
                      Role::fill,
                      Role::header,
                      Role::text,
                      Role::text,
                      Role::text,
                      Role::footer_band,
                      Role::progress,
                      Role::button,
                      Role::focus_ring,
                      Role::bevel,
                  })
    );
}

// --- A finger's reach ------------------------------------------------------

/// Where the settings dialog's finger press landed.
struct Landing {
    kit::Point finger{};
    int32_t reach{};
    kit::ControlId control{kit::no_control};
    kit::Point at{};
};

void check_landings(
    const kit::DisplayList& list,
    const std::vector<Landing>& landings,
    const auto& press_with_finger
) {
    for (const Landing& landing : landings) {
        const kit::Reached reached = kit::reach(list, landing.finger, landing.reach);
        OA_CHECK(reached.control == landing.control);
        OA_CHECK(reached.at.x == landing.at.x && reached.at.y == landing.at.y);
        // The model's own finger press holds the same button, shifted as far.
        const auto [pressed, shift] = press_with_finger(landing);
        OA_CHECK(pressed == landing.control);
        OA_CHECK(shift.x == landing.at.x - landing.finger.x);
        OA_CHECK(shift.y == landing.at.y - landing.finger.y);
    }
}

struct Press {
    kit::ControlId pressed{};
    kit::Point shift{};
};

void a_finger_lands_as_before() {
    // The short notice at the estimated widths: 150 high, OK at 336, 124 and
    // the open button at 235, 124. OK is tried first: from the gap three
    // columns from each, the finger takes OK.
    const kit::Notice notice = short_notice();
    check_landings(
        kit::notice_list(notice, nullptr),
        {
            {{360, 132}, 6, kit::notice_ok, {360, 132}},
            {{360, 144}, 6, kit::notice_ok, {360, 140}},
            {{333, 132}, 6, kit::notice_ok, {336, 132}},
            {{332, 132}, 6, kit::notice_open, {330, 132}},
            {{231, 121}, 6, kit::notice_open, {235, 124}},
            {{231, 121}, 4, kit::no_control, {231, 121}},
            {{300, 162}, 22, kit::notice_open, {300, 140}},
            {{5, 5}, 22, kit::no_control, {5, 5}},
            {{360, 144}, 0, kit::no_control, {360, 144}},
        },
        [&notice](const Landing& landing) {
            kit::Notice pressed = notice;
            static_cast<void>(kit::notice_finger_down(pressed, landing.finger, 150, landing.reach));
            return Press{pressed.pressed, {pressed.finger_shift_x, pressed.finger_shift_y}};
        }
    );

    // Two buttons, tried left to right: from the gap two columns from
    // CANCEL and three from REPLACE, CANCEL; a row further from REPLACE than
    // its reach, nothing.
    const kit::Question two = question_of({"CANCEL", "REPLACE"});
    const auto press_question = [](const kit::Question& question) {
        return [question](const Landing& landing) {
            kit::Question pressed = question;
            static_cast<void>(
                kit::question_finger_down(pressed, landing.finger, 150, landing.reach)
            );
            return Press{pressed.pressed, {pressed.finger_shift_x, pressed.finger_shift_y}};
        };
    };
    check_landings(
        kit::question_list(two, nullptr),
        {
            {{333, 132}, 6, 1, {333, 132}},
            {{320, 132}, 6, 0, {317, 132}},
            {{321, 132}, 6, 1, {323, 132}},
            {{333, 146}, 6, 1, {333, 140}},
            {{333, 147}, 6, kit::no_control, {333, 147}},
            {{333, 148}, 6, kit::no_control, {333, 148}},
            {{256, 120}, 6, 0, {260, 124}},
        },
        press_question(two)
    );

    // Three buttons: from the gaps three columns from each side, the left one.
    const kit::Question three = question_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"});
    check_landings(
        kit::question_list(three, nullptr),
        {
            {{203, 145}, 22, 1, {203, 140}},
            {{320, 132}, 6, 1, {317, 132}},
            {{180, 121}, 6, 0, {177, 124}},
            {{5, 5}, 22, kit::no_control, {5, 5}},
        },
        press_question(three)
    );
}

// --- Pointer and keys --------------------------------------------------------

void the_pointer_presses_buttons() {
    // A notice: a press on the open button released on it asks for the
    // folder; on OK, it closes; released elsewhere, it only redraws.
    kit::Notice notice = short_notice();
    OA_CHECK(kit::notice_pointer_move(notice, {240, 130}, 150) == kit::NoticeAction::redraw);
    OA_CHECK(notice.hovered == kit::notice_open);
    OA_CHECK(kit::notice_pointer_move(notice, {241, 130}, 150) == kit::NoticeAction::none);
    OA_CHECK(kit::notice_pointer_down(notice, {240, 130}, 150) == kit::NoticeAction::redraw);
    OA_CHECK(notice.pressed == kit::notice_open);
    OA_CHECK(kit::notice_pointer_up(notice, {240, 130}, 150) == kit::NoticeAction::open_folder);
    OA_CHECK(notice.pressed == kit::no_control);
    OA_CHECK(kit::notice_pointer_down(notice, {340, 130}, 150) == kit::NoticeAction::redraw);
    OA_CHECK(kit::notice_pointer_up(notice, {340, 130}, 150) == kit::NoticeAction::closed);
    OA_CHECK(kit::notice_pointer_down(notice, {340, 130}, 150) == kit::NoticeAction::redraw);
    OA_CHECK(kit::notice_pointer_up(notice, {240, 130}, 150) == kit::NoticeAction::redraw);
    OA_CHECK(kit::notice_pointer_down(notice, {5, 5}, 150) == kit::NoticeAction::none);
    OA_CHECK(kit::notice_pointer_up(notice, {5, 5}, 150) == kit::NoticeAction::none);
    // A finger three rows under OK takes it; its release where it landed
    // closes the notice, and a move there keeps OK lit.
    OA_CHECK(kit::notice_finger_down(notice, {360, 144}, 150, 6) == kit::NoticeAction::redraw);
    OA_CHECK(kit::notice_pointer_move(notice, {360, 144}, 150) == kit::NoticeAction::none);
    OA_CHECK(notice.hovered == kit::notice_ok);
    OA_CHECK(kit::notice_pointer_up(notice, {360, 144}, 150) == kit::NoticeAction::closed);
    OA_CHECK(notice.finger_shift_x == 0 && notice.finger_shift_y == 0);

    // A question answers the button a release lands on.
    kit::Question question = question_of({"CANCEL", "REPLACE"});
    OA_CHECK(
        kit::question_pointer_down(question, {330, 130}, 150).action == kit::QuestionAction::redraw
    );
    const kit::QuestionAnswer answered = kit::question_pointer_up(question, {330, 130}, 150);
    OA_CHECK(answered.action == kit::QuestionAction::answered && answered.button == 1);
    OA_CHECK(
        kit::question_pointer_down(question, {265, 130}, 150).action == kit::QuestionAction::redraw
    );
    OA_CHECK(
        kit::question_pointer_up(question, {330, 130}, 150).action == kit::QuestionAction::redraw
    );
    OA_CHECK(
        kit::question_pointer_move(question, {265, 130}, 150).action == kit::QuestionAction::redraw
    );
    OA_CHECK(question.hovered == 0);
    OA_CHECK(kit::question_pointer_down(question, {5, 5}, 150).action == kit::QuestionAction::none);
    OA_CHECK(
        kit::question_finger_down(question, {333, 146}, 150, 6).action ==
        kit::QuestionAction::redraw
    );
    const kit::QuestionAnswer tapped = kit::question_pointer_up(question, {333, 146}, 150);
    OA_CHECK(tapped.action == kit::QuestionAction::answered && tapped.button == 1);
}

/// Every key the kit names, in order.
constexpr std::array<kit::Key, 15> every_key{
    kit::Key::enter,
    kit::Key::escape,
    kit::Key::up,
    kit::Key::down,
    kit::Key::left,
    kit::Key::right,
    kit::Key::space,
    kit::Key::tab,
    kit::Key::back_tab,
    kit::Key::page_up,
    kit::Key::page_down,
    kit::Key::home,
    kit::Key::end,
    kit::Key::yes,
    kit::Key::no,
};

/// The keys a notice and a question do nothing with: the editing keys a
/// text field takes, and the information key.
constexpr std::array<kit::Key, 3> ignored_keys{
    kit::Key::backspace, kit::Key::delete_forward, kit::Key::info
};

void every_key_on_a_notice() {
    using kit::NoticeAction;

    // What each key does from OK marked, and the mark after it; then from the
    // open button marked.
    struct Expected {
        NoticeAction action;
        kit::ControlId marked;
    };

    const std::array<Expected, 15> from_ok{{
        {NoticeAction::closed, kit::notice_ok},   // enter
        {NoticeAction::closed, kit::notice_ok},   // escape
        {NoticeAction::redraw, kit::notice_open}, // up
        {NoticeAction::redraw, kit::notice_open}, // down
        {NoticeAction::redraw, kit::notice_open}, // left
        {NoticeAction::redraw, kit::notice_open}, // right
        {NoticeAction::closed, kit::notice_ok},   // space
        {NoticeAction::redraw, kit::notice_open}, // tab
        {NoticeAction::redraw, kit::notice_open}, // back tab
        {NoticeAction::none, kit::notice_ok},     // page up
        {NoticeAction::none, kit::notice_ok},     // page down
        {NoticeAction::none, kit::notice_ok},     // home
        {NoticeAction::none, kit::notice_ok},     // end
        {NoticeAction::none, kit::notice_ok},     // yes
        {NoticeAction::none, kit::notice_ok},     // no
    }};
    const std::array<Expected, 15> from_open{{
        {NoticeAction::closed, kit::notice_open},      // enter
        {NoticeAction::closed, kit::notice_open},      // escape
        {NoticeAction::redraw, kit::notice_ok},        // up
        {NoticeAction::redraw, kit::notice_ok},        // down
        {NoticeAction::redraw, kit::notice_ok},        // left
        {NoticeAction::redraw, kit::notice_ok},        // right
        {NoticeAction::open_folder, kit::notice_open}, // space
        {NoticeAction::redraw, kit::notice_ok},        // tab
        {NoticeAction::redraw, kit::notice_ok},        // back tab
        {NoticeAction::none, kit::notice_open},        // page up
        {NoticeAction::none, kit::notice_open},        // page down
        {NoticeAction::none, kit::notice_open},        // home
        {NoticeAction::none, kit::notice_open},        // end
        {NoticeAction::none, kit::notice_open},        // yes
        {NoticeAction::none, kit::notice_open},        // no
    }};
    for (std::size_t index = 0; index < every_key.size(); ++index) {
        kit::Notice on_ok = short_notice();
        OA_CHECK(kit::notice_key(on_ok, every_key[index]) == from_ok[index].action);
        OA_CHECK(on_ok.marked == from_ok[index].marked);
        kit::Notice on_open = short_notice();
        on_open.marked = kit::notice_open;
        OA_CHECK(kit::notice_key(on_open, every_key[index]) == from_open[index].action);
        OA_CHECK(on_open.marked == from_open[index].marked);
    }
    // The editing keys and the information key do nothing, from either mark.
    for (const kit::Key key : ignored_keys)
        for (const kit::ControlId mark : {kit::notice_ok, kit::notice_open}) {
            kit::Notice notice = short_notice();
            notice.marked = mark;
            OA_CHECK(kit::notice_key(notice, key) == NoticeAction::none);
            OA_CHECK(notice.marked == mark);
        }
}

void a_notice_with_no_open_caption_offers_ok_alone() {
    kit::Notice folder = short_notice();
    const kit::PlacedNotice with_open = kit::place_notice(folder, nullptr);
    OA_CHECK(with_open.open_button.width > 0 && with_open.open_button.height > 0);
    const kit::DisplayList folder_list = kit::notice_list(folder, nullptr);
    OA_CHECK(folder_list.controls.size() == 3);
    check_control(folder_list, 1, kit::notice_open, "notice.open", "Open folder");

    kit::Notice told = short_notice();
    told.open_caption.clear();
    told.word = "updated-notice";
    const kit::PlacedNotice placed = kit::place_notice(told, nullptr);
    OA_CHECK(placed.open_button.width == 0 && placed.open_button.height == 0);
    OA_CHECK(same_rect(placed.ok_button, with_open.ok_button));
    const kit::DisplayList list = kit::notice_list(told, nullptr);
    OA_CHECK(list.controls.size() == 2);
    check_control(list, 0, kit::notice_ok, "updated-notice.ok", "OK");
    check_body(list, 1, "updated-notice.body", "SAVED\nThe game was saved.");
    OA_CHECK((list.tab_order == std::vector<kit::ControlId>{kit::notice_ok}));
    for (const kit::Control& control : list.controls)
        OA_CHECK(control.name.find(".open") == std::string::npos);
    int32_t buttons = 0;
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::button)
            ++buttons;
    OA_CHECK(buttons == 1);

    const kit::Point on_open{
        with_open.open_button.x + with_open.open_button.width / 2,
        with_open.open_button.y + with_open.open_button.height / 2,
    };
    OA_CHECK(kit::notice_pointer_down(told, on_open, placed.height) == kit::NoticeAction::none);
    OA_CHECK(told.pressed == kit::no_control);
    OA_CHECK(kit::notice_pointer_up(told, on_open, placed.height) == kit::NoticeAction::none);

    for (const kit::Key key : every_key) {
        kit::Notice keyed = told;
        keyed.marked = kit::notice_open;
        const kit::NoticeAction action = kit::notice_key(keyed, key);
        OA_CHECK(keyed.marked == kit::notice_ok);
        if (key == kit::Key::enter || key == kit::Key::escape || key == kit::Key::space)
            OA_CHECK(action == kit::NoticeAction::closed);
        else
            OA_CHECK(action == kit::NoticeAction::none);
    }
}

void notes_keep_a_line_each() {
    const std::array<std::string, 3> lines{
        "New in 9.9.9:",
        "• Wider bars",
        "• A second English line",
    };
    const std::string notes = lines[0] + "\n" + lines[1] + "\n" + lines[2];
    const int32_t width = kit::compact_metrics.notice_width - 2 * kit::compact_metrics.padding;
    kit::Notice notice;
    notice.open_caption.clear();
    notice.paragraphs.push_back({notes, false});
    const kit::PlacedNotice placed = kit::place_notice(notice, nullptr);
    OA_CHECK(placed.lines.size() == 3);
    for (int32_t index = 0; index < 3; ++index) {
        OA_CHECK(
            placed.lines[static_cast<std::size_t>(index)].text ==
            lines[static_cast<std::size_t>(index)]
        );
        OA_CHECK(same_rect(
            placed.lines[static_cast<std::size_t>(index)].rect,
            {kit::compact_metrics.padding,
             kit::compact_metrics.text_top + kit::compact_metrics.small_line * index,
             width,
             kit::compact_metrics.small_line}
        ));
    }

    kit::Question question = question_of({"LATER"});
    question.paragraphs = {{notes, false}};
    const kit::PlacedQuestion asked = kit::place_question(question, nullptr);
    OA_CHECK(asked.lines.size() == 3);
    for (std::size_t index = 0; index < lines.size(); ++index)
        OA_CHECK(asked.lines[index].text == lines[index]);
}

void every_key_on_a_question() {
    using kit::QuestionAction;

    struct Expected {
        QuestionAction action;
        int32_t button;
        kit::ControlId marked;
    };

    const auto check = [](kit::Question question, const std::array<Expected, 15>& expected) {
        const kit::Question start = question;
        for (std::size_t index = 0; index < every_key.size(); ++index) {
            question = start;
            const kit::QuestionAnswer answer = kit::question_key(question, every_key[index]);
            OA_CHECK(answer.action == expected[index].action);
            OA_CHECK(answer.button == expected[index].button);
            OA_CHECK(question.marked == expected[index].marked);
        }
    };
    // One button: every answer is that one, and the arrows change nothing.
    check(
        question_of({"OK"}),
        {{
            {QuestionAction::answered, 0, 0}, // enter
            {QuestionAction::answered, 0, 0}, // escape
            {QuestionAction::none, -1, 0},    // up
            {QuestionAction::none, -1, 0},    // down
            {QuestionAction::none, -1, 0},    // left
            {QuestionAction::none, -1, 0},    // right
            {QuestionAction::answered, 0, 0}, // space
            {QuestionAction::none, -1, 0},    // tab
            {QuestionAction::none, -1, 0},    // back tab
            {QuestionAction::none, -1, 0},    // page up
            {QuestionAction::none, -1, 0},    // page down
            {QuestionAction::none, -1, 0},    // home
            {QuestionAction::none, -1, 0},    // end
            {QuestionAction::answered, 0, 0}, // yes
            {QuestionAction::answered, 0, 0}, // no
        }}
    );
    // Two buttons, the second marked: the mark goes round.
    check(
        question_of({"CANCEL", "REPLACE"}),
        {{
            {QuestionAction::answered, 1, 1}, // enter
            {QuestionAction::answered, 0, 1}, // escape
            {QuestionAction::redraw, -1, 0},  // up
            {QuestionAction::redraw, -1, 0},  // down
            {QuestionAction::redraw, -1, 0},  // left
            {QuestionAction::redraw, -1, 0},  // right
            {QuestionAction::answered, 1, 1}, // space
            {QuestionAction::redraw, -1, 0},  // tab
            {QuestionAction::redraw, -1, 0},  // back tab
            {QuestionAction::none, -1, 1},    // page up
            {QuestionAction::none, -1, 1},    // page down
            {QuestionAction::none, -1, 1},    // home
            {QuestionAction::none, -1, 1},    // end
            {QuestionAction::answered, 1, 1}, // yes
            {QuestionAction::answered, 0, 1}, // no
        }}
    );
    // Three buttons, the middle one marked.
    kit::Question three = question_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"});
    three.marked = 1;
    check(
        three,
        {{
            {QuestionAction::answered, 1, 1}, // enter
            {QuestionAction::answered, 0, 1}, // escape
            {QuestionAction::redraw, -1, 0},  // up
            {QuestionAction::redraw, -1, 2},  // down
            {QuestionAction::redraw, -1, 0},  // left
            {QuestionAction::redraw, -1, 2},  // right
            {QuestionAction::answered, 1, 1}, // space
            {QuestionAction::redraw, -1, 2},  // tab
            {QuestionAction::redraw, -1, 0},  // back tab
            {QuestionAction::none, -1, 1},    // page up
            {QuestionAction::none, -1, 1},    // page down
            {QuestionAction::none, -1, 1},    // home
            {QuestionAction::none, -1, 1},    // end
            {QuestionAction::answered, 2, 1}, // yes
            {QuestionAction::answered, 0, 1}, // no
        }}
    );
    // Three buttons, the last marked: Right and Tab go round to the first.
    three.marked = 2;
    kit::Question last = three;
    OA_CHECK(kit::question_key(last, kit::Key::right).action == QuestionAction::redraw);
    OA_CHECK(last.marked == 0);
    OA_CHECK(kit::question_key(last, kit::Key::back_tab).action == QuestionAction::redraw);
    OA_CHECK(last.marked == 2);
    // No buttons: no key does anything.
    for (const kit::Key key : every_key) {
        kit::Question none{};
        OA_CHECK(kit::question_key(none, key).action == QuestionAction::none);
    }
    // The editing keys and the information key do nothing on one, two or
    // three buttons.
    for (const kit::Key key : ignored_keys)
        for (const kit::Question& start :
             {question_of({"OK"}),
              question_of({"CANCEL", "REPLACE"}),
              question_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"})}) {
            kit::Question question = start;
            const kit::QuestionAnswer answer = kit::question_key(question, key);
            OA_CHECK(answer.action == QuestionAction::none && answer.button == -1);
            OA_CHECK(question.marked == start.marked);
        }
}

// --- Drawing -------------------------------------------------------------

/// Shows a few of the moved notice's words in other words, as a language's
/// look-up would.
std::string_view other_words(std::string_view text) {
    if (text == "SAVED GAMES MOVED")
        return "MOVED";
    if (text == "OK")
        return "YES";
    if (text == "OPEN FOLDER")
        return "OPEN IT";
    if (text == "3 saved games moved to:")
        return "Three went to:";
    return text;
}

void paint_draws_what_draw_notice_draws() {
    std::vector<kit::Notice> notices{
        short_notice(), moved_notice(), long_notice(), wide_notice(), cut_notice(), {}
    };
    kit::Notice marked = moved_notice();
    marked.marked = kit::notice_open;
    marked.hovered = kit::notice_ok;
    marked.pressed = kit::notice_open;
    notices.push_back(marked);
    marked.hovered = kit::notice_open;
    marked.failure = "It could not be opened.";
    notices.push_back(marked);
    for (const kit::LookUp& look_up : {kit::LookUp{}, kit::LookUp{other_words}})
        for (const kit::Notice& notice : notices)
            for (const int32_t scale : {1, 2}) {
                const int32_t height = kit::place_notice(notice, &fonts()).height;
                const int32_t width = kit::notice_width * scale + 6;
                const int32_t rows = height * scale + 6;
                renderer::Surface drawn = black(width, rows);
                renderer::Surface painted = black(width, rows);
                const kit::Canvas draw_on{&drawn, {3, 3, scale, {}}, &fonts(), red_icon()};
                const kit::Canvas paint_on{&painted, {3, 3, scale, {}}, &fonts(), red_icon()};
                kit::draw_notice(draw_on, notice, look_up);
                kit::paint(paint_on, kit::notice_list(notice, &fonts(), look_up));
                same(drawn, painted, notice.title.c_str());
            }
    // The look-up shows the words the notice was given in its own words.
    const kit::DisplayList shown =
        kit::notice_list(moved_notice(), &fonts(), kit::LookUp{other_words});
    OA_CHECK(shown.items.size() > 2 && shown.items[1].text == "MOVED");
    OA_CHECK(shown.items.size() > 2 && shown.items[2].text == "Three went to:");
    OA_CHECK(!shown.controls.empty() && shown.controls[0].text == "YES");
    // A notice's OK caption is its own to set.
    kit::Notice other_ok = short_notice();
    other_ok.ok_caption = "CLOSE";
    OA_CHECK(kit::notice_list(other_ok, nullptr).controls[0].text == "CLOSE");
}

/// The header item's words.
///
/// @param list the display list
/// @return the header's text; empty when it has none
std::string header_text(const kit::DisplayList& list) {
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::header)
            return item.text;
    return {};
}

/// The columns kept for a question's title at Compact.
///
/// @return the title rectangle's width
int32_t compact_title_room() {
    const int32_t box = kit::metrics_of(kit::SizeClass::compact).notice_width;
    const int32_t title_left =
        kit::notice_mark.x + kit::notice_mark.width + kit::compact_metrics.header_gap;
    return box - kit::compact_metrics.padding - title_left;
}

void a_question_title_is_cut_to_its_header() {
    kit::Question plain = question_of({"OK"});
    plain.title = "Start";
    OA_CHECK(header_text(kit::question_list(plain, &fonts())) == "Start");

    kit::Question wide = question_of({"OK"});
    wide.title = std::string(12, 'W') + " " + std::string(12, 'W') + " " + std::string(12, 'W') +
                 " " + std::string(12, 'W') + " " + std::string(12, 'W');
    const int32_t room = compact_title_room();
    OA_CHECK(
        kit::tracked_width(
            fonts(), kit::FontRole::regular, wide.title, kit::compact_metrics.heading_tracking
        ) > room
    );
    const std::string shown = header_text(kit::question_list(wide, &fonts()));
    OA_CHECK(shown != wide.title);
    OA_CHECK(shown.size() >= kit::ellipsis.size() && shown.ends_with(kit::ellipsis));
    const std::string prefix = shown.substr(0, shown.size() - kit::ellipsis.size());
    OA_CHECK(wide.title.starts_with(prefix));
    OA_CHECK(
        prefix.size() == wide.title.size() ||
        (prefix.size() < wide.title.size() && wide.title[prefix.size()] == ' ')
    );
    // The header records the line after stand-ins. A font that draws the
    // ellipsis as three stops is wider than the ellipsis counted as one.
    const std::string drawn = kit::with_stand_ins(fonts(), kit::FontRole::regular, shown);
    OA_CHECK(
        kit::tracked_width(
            fonts(), kit::FontRole::regular, drawn, kit::compact_metrics.heading_tracking
        ) <= room
    );
    OA_CHECK(header_text(kit::question_list(wide, nullptr)) == wide.title);
}

void paint_draws_what_draw_question_draws() {
    std::vector<kit::Question> questions{
        question_of({"OK"}),
        question_of({"CANCEL", "REPLACE"}),
        question_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"}),
        progress_question(),
        bar_cut_question(),
        four_question(),
        kit::Question{},
    };
    kit::Question held = question_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"});
    held.hovered = 1;
    held.pressed = 1;
    held.marked = 0;
    held.progress = 1;
    held.failure = "No.";
    questions.push_back(held);
    held.progress = kit::question_progress_whole;
    held.hovered = 2;
    questions.push_back(held);
    kit::Question wide = question_of({"OK"});
    wide.title = std::string(12, 'W') + " " + std::string(12, 'W') + " " + std::string(12, 'W') +
                 " " + std::string(12, 'W') + " " + std::string(12, 'W');
    questions.push_back(wide);
    for (const kit::Question& question : questions)
        for (const int32_t scale : {1, 2}) {
            const int32_t height = kit::place_question(question, &fonts()).height;
            const int32_t width = kit::notice_width * scale + 6;
            const int32_t rows = height * scale + 6;
            renderer::Surface drawn = black(width, rows);
            renderer::Surface painted = black(width, rows);
            const kit::Canvas draw_on{&drawn, {3, 3, scale, {}}, &fonts(), red_icon()};
            const kit::Canvas paint_on{&painted, {3, 3, scale, {}}, &fonts(), red_icon()};
            kit::draw_question(draw_on, question);
            kit::paint(paint_on, kit::question_list(question, &fonts()));
            same(drawn, painted, question.title.c_str());
        }
}

/// Returns how many columns of a bar's inner row are filled in the accent.
///
/// @param surface the surface the bar was drawn on
/// @param bar the bar
/// @return the columns
int32_t filled_columns(const renderer::Surface& surface, const kit::Rect& bar) {
    int32_t filled = 0;
    for (int32_t x = bar.x + 1; x < bar.x + bar.width - 1; ++x)
        if (pixel(surface, x, bar.y + 1) == kit::colour::accent)
            ++filled;
    return filled;
}

void the_progress_bar_fills_as_far_as_it_came() {
    // The prompt's bar: 376 by 8, so 374 columns inside its border.
    const kit::Rect bar{2, 2, 376, 8};
    const std::array<std::pair<int32_t, int32_t>, 4> fills{{
        {0, 0},
        {1, 0},
        {500, 187},
        {1000, 374},
    }};
    for (const auto& [done, columns] : fills) {
        renderer::Surface drawn = black(380, 12);
        const kit::Canvas canvas{&drawn, {0, 0, 1, {}}, &fonts(), {}};
        kit::draw_progress(canvas, bar, done, kit::question_progress_whole);
        OA_CHECK(filled_columns(drawn, bar) == columns);
        // The border, the well past the fill, and the fill from the left.
        OA_CHECK(pixel(drawn, bar.x, bar.y) == kit::colour::control_border);
        OA_CHECK(pixel(drawn, bar.x + bar.width - 1, bar.y + 4) == kit::colour::control_border);
        if (columns < 374)
            OA_CHECK(pixel(drawn, bar.x + 1 + columns, bar.y + 4) == kit::colour::well);
        if (columns > 0) {
            OA_CHECK(pixel(drawn, bar.x + 1, bar.y + 6) == kit::colour::accent);
            OA_CHECK(pixel(drawn, bar.x + columns, bar.y + 6) == kit::colour::accent);
        }
        OA_CHECK(pixel(drawn, 0, 0) == (kit::Colour{0, 0, 0}));
        // A progress item paints the same.
        renderer::Surface painted = black(380, 12);
        kit::DisplayList list;
        kit::add_progress(list, bar, done, kit::question_progress_whole);
        kit::paint({&painted, {0, 0, 1, {}}, &fonts(), {}}, list);
        same(drawn, painted, "progress");
    }
    // Past the whole is full, under 0 is empty, and a whole of 0 fills nothing.
    renderer::Surface over = black(380, 12);
    kit::draw_progress({&over, {0, 0, 1, {}}, &fonts(), {}}, bar, 1500, 1000);
    OA_CHECK(filled_columns(over, bar) == 374);
    renderer::Surface under = black(380, 12);
    kit::draw_progress({&under, {0, 0, 1, {}}, &fonts(), {}}, bar, -5, 1000);
    OA_CHECK(filled_columns(under, bar) == 0);
    renderer::Surface no_whole = black(380, 12);
    kit::draw_progress({&no_whole, {0, 0, 1, {}}, &fonts(), {}}, bar, 5, 0);
    OA_CHECK(filled_columns(no_whole, bar) == 0);
    OA_CHECK(pixel(no_whole, bar.x + 4, bar.y + 4) == kit::colour::well);
    // An empty bar draws nothing.
    renderer::Surface empty = black(380, 12);
    kit::draw_progress({&empty, {0, 0, 1, {}}, &fonts(), {}}, {2, 2, 0, 8}, 500, 1000);
    OA_CHECK(empty.rgb == black(380, 12).rgb);
}

bool button_role(kit::Role role) {
    return role == kit::Role::button || role == kit::Role::oa_button ||
           role == kit::Role::button_main || role == kit::Role::button_danger ||
           role == kit::Role::call_to_action;
}

bool button_kind(kit::ControlKind kind) {
    return kind == kit::ControlKind::button || kind == kit::ControlKind::buttons;
}

void the_work_dialog_has_no_buttons() {
    const std::array<int32_t, 2> amounts{-1, 400};
    for (const int32_t amount : amounts) {
        kit::WorkDialog dialog;
        dialog.title = "Open Annihilation";
        dialog.line = "Loading the game's archives";
        dialog.progress = amount;
        dialog.turn = 2;
        const kit::DisplayList list = kit::work_dialog_list(dialog, &fonts());
        const kit::PlacedWorkDialog placed = kit::place_work_dialog(dialog, &fonts());
        OA_CHECK(list.tab_order.empty());
        OA_CHECK(list.controls.size() == 1);
        OA_CHECK(list.controls[0].kind == kit::ControlKind::area);
        OA_CHECK(!list.controls[0].enabled);
        OA_CHECK(!list.controls[0].focusable);
        OA_CHECK(list.controls[0].name == "progress.body");
        for (const kit::Item& item : list.items)
            OA_CHECK(!button_role(item.role));
        for (const kit::Control& control : list.controls)
            OA_CHECK(!button_kind(control.kind));
        bool progress = false;
        for (const kit::Item& item : list.items)
            if (item.role == kit::Role::progress)
                progress = true;
        const kit::Metrics& compact = kit::metrics_of(kit::SizeClass::compact);
        const int32_t under_the_line =
            compact.text_top + compact.small_line + compact.paragraph_gap;
        if (amount >= 0) {
            OA_CHECK(progress);
            OA_CHECK(placed.bar.width > 0);
            OA_CHECK(placed.spinner.width == 0);
            OA_CHECK(placed.bar.y == under_the_line);
            OA_CHECK(
                placed.height == under_the_line + compact.progress_bar_height +
                                     compact.text_bottom_gap + compact.edge
            );
        } else {
            OA_CHECK(!progress);
            OA_CHECK(placed.bar.width == 0);
            OA_CHECK(placed.spinner.width > 0);
            OA_CHECK(placed.spinner.y == under_the_line);
            OA_CHECK(
                placed.height ==
                under_the_line + kit::work_spinner_side + compact.text_bottom_gap + compact.edge
            );
        }
    }

    kit::WorkDialog bar;
    bar.title = "Open Annihilation";
    bar.line = "Checking your copy of Total Annihilation";
    bar.progress = 400;
    const int32_t height = kit::place_work_dialog(bar, &fonts()).height;
    const int32_t width = kit::notice_width + 6;
    const int32_t rows = height + 6;
    renderer::Surface drawn = black(width, rows);
    renderer::Surface painted = black(width, rows);
    const kit::Canvas draw_on{&drawn, {3, 3, 1, {}}, &fonts(), red_icon()};
    const kit::Canvas paint_on{&painted, {3, 3, 1, {}}, &fonts(), red_icon()};
    kit::draw_work_dialog(draw_on, bar);
    kit::paint(paint_on, kit::work_dialog_list(bar, &fonts()));
    same(drawn, painted, "work dialog");

    kit::WorkDialog spinning;
    spinning.title = "Open Annihilation";
    spinning.line = "Loading the game's archives";
    spinning.progress = -1;
    const kit::PlacedWorkDialog spinning_placed = kit::place_work_dialog(spinning, &fonts());
    renderer::Surface first = black(width, rows);
    renderer::Surface later = black(width, rows);
    spinning.turn = 0;
    kit::draw_work_dialog({&later, {3, 3, 1, {}}, &fonts(), red_icon()}, spinning);
    spinning.turn = 2;
    kit::draw_work_dialog({&first, {3, 3, 1, {}}, &fonts(), red_icon()}, spinning);
    bool turned = false;
    for (int32_t y = spinning_placed.spinner.y;
         y < spinning_placed.spinner.y + spinning_placed.spinner.height && !turned;
         ++y) {
        for (int32_t x = spinning_placed.spinner.x;
             x < spinning_placed.spinner.x + spinning_placed.spinner.width;
             ++x) {
            if (pixel(first, x + 3, y + 3) != pixel(later, x + 3, y + 3))
                turned = true;
        }
    }
    OA_CHECK(turned);
}

} // namespace

int main() {
    notices_place_as_before();
    questions_place_as_before();
    question_buttons_fit_their_captions();
    lists_name_their_buttons();
    a_word_of_their_own_names_them();
    the_text_control_takes_no_press();
    lists_draw_in_order();
    a_finger_lands_as_before();
    the_pointer_presses_buttons();
    every_key_on_a_notice();
    a_notice_with_no_open_caption_offers_ok_alone();
    notes_keep_a_line_each();
    every_key_on_a_question();
    paint_draws_what_draw_notice_draws();
    a_question_title_is_cut_to_its_header();
    paint_draws_what_draw_question_draws();
    the_progress_bar_fills_as_far_as_it_came();
    the_work_dialog_has_no_buttons();
    return oa::test::check_exit_status();
}
