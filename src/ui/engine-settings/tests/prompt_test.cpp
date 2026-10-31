// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A prompt of Open Annihilation's own: prompts of one, two and three
// buttons laid out right-aligned at their captions' widths, the keys that
// answer and mark them, a pointer's press and release, a finger's press
// taking the nearest button within reach, the progress bar's part, and a
// text too long for the prompt found cut. At each size class, a notice and a
// three-button prompt within the class's width and heights.

#include "oa/ui/engine_settings/notice.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace settings = oa::ui::engine_settings;
using settings::DialogKey;
using settings::Prompt;
using settings::PromptAction;

/// Returns a prompt of buttons with the captions given, the last an accent one.
///
/// @param captions the captions, left to right
/// @return the prompt
Prompt prompt_of(const std::vector<std::string>& captions) {
    Prompt prompt{};
    prompt.title = "UPDATE MOD";
    prompt.paragraphs.push_back({"Example Mod 1.0, revision 1, is installed.", false});
    prompt.paragraphs.push_back(
        {"/Users/someone/Documents/Open Annihilation/Mods/example-mod", true}
    );
    for (const auto& caption : captions)
        prompt.buttons.push_back({caption, false});
    prompt.buttons.back().accent = true;
    prompt.cancel_button = 0;
    prompt.primary_button = static_cast<int32_t>(captions.size()) - 1;
    prompt.marked = prompt.primary_button;
    return prompt;
}

/// Returns the parts of a layout that are buttons, by their numbers.
///
/// @param prompt the prompt
/// @return the buttons' parts, left to right
std::vector<settings::LayoutPart> buttons_of(const Prompt& prompt) {
    std::vector<settings::LayoutPart> buttons;
    for (const auto& part : settings::prompt_layout(prompt))
        if (part.control >= 0 && part.control < static_cast<int32_t>(settings::most_prompt_buttons))
            buttons.push_back(part);
    return buttons;
}

void test_layout() {
    for (const std::vector<std::string>& captions :
         {std::vector<std::string>{"OK"},
          std::vector<std::string>{"CANCEL", "REPLACE"},
          std::vector<std::string>{"CANCEL", "INSTALL ALONGSIDE", "REPLACE"}}) {
        const Prompt prompt = prompt_of(captions);
        const auto buttons = buttons_of(prompt);
        OA_CHECK(buttons.size() == captions.size());
        // Right-aligned, the last at the footer's right, as Settings places
        // its buttons: two spaces apart, each its caption with two spaces
        // either side, at least twice the control height.
        OA_CHECK(buttons.back().rect.x + buttons.back().rect.width == settings::notice_width - 12);
        const oa::ui::kit::Metrics& compact = oa::ui::kit::compact_metrics;
        for (std::size_t index = 0; index < buttons.size(); ++index) {
            OA_CHECK(buttons[index].control == static_cast<int32_t>(index));
            OA_CHECK(buttons[index].text == captions[index]);
            const int32_t caption_width =
                static_cast<int32_t>(captions[index].size()) * settings::estimated_character_width;
            OA_CHECK(
                buttons[index].rect.width ==
                std::max(2 * compact.control_height, caption_width + 4 * compact.space)
            );
            if (index + 1 < buttons.size())
                OA_CHECK(
                    buttons[index].rect.x + buttons[index].rect.width + 2 * compact.space ==
                    buttons[index + 1].rect.x
                );
        }
        OA_CHECK(settings::prompt_fits(prompt));
        const int32_t height = settings::prompt_height(prompt);
        OA_CHECK(height > 0 && height <= settings::greatest_notice_height);
    }
    // The progress bar lies under the text and makes the prompt taller.
    Prompt bar = prompt_of({"CANCEL"});
    const int32_t without = settings::prompt_height(bar);
    bar.progress = 500;
    bool found = false;
    for (const auto& part : settings::prompt_layout(bar))
        if (part.control == settings::prompt_bar_control) {
            found = true;
            OA_CHECK(part.rect.width == settings::notice_width - 24);
        }
    OA_CHECK(found);
    OA_CHECK(settings::prompt_height(bar) >= without);
    // A text longer than the greatest height is cut, and found cut.
    Prompt long_text = prompt_of({"OK"});
    for (int paragraph = 0; paragraph < 40; ++paragraph)
        long_text.paragraphs.push_back({"A line of the prompt's text that takes a row.", false});
    OA_CHECK(!settings::prompt_fits(long_text));
    OA_CHECK(settings::prompt_height(long_text) == settings::greatest_notice_height);
}

void test_keys() {
    Prompt prompt = prompt_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"});
    OA_CHECK(settings::prompt_key(prompt, DialogKey::enter).button == 2);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::space).action == PromptAction::answered);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::escape).button == 0);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::no).button == 0);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::yes).button == 2);
    // The mark moves round the buttons.
    OA_CHECK(settings::prompt_key(prompt, DialogKey::right).action == PromptAction::redraw);
    OA_CHECK(prompt.marked == 0);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::tab).action == PromptAction::redraw);
    OA_CHECK(prompt.marked == 1);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::left).action == PromptAction::redraw);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::back_tab).action == PromptAction::redraw);
    OA_CHECK(prompt.marked == 2);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::up).action == PromptAction::redraw);
    OA_CHECK(prompt.marked == 1);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::down).action == PromptAction::redraw);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::down).action == PromptAction::redraw);
    OA_CHECK(prompt.marked == 0);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::enter).button == 0);
    OA_CHECK(settings::prompt_key(prompt, DialogKey::home).action == PromptAction::none);
    // One button: the arrows change nothing, and every answer is that one.
    Prompt one = prompt_of({"OK"});
    OA_CHECK(settings::prompt_key(one, DialogKey::tab).action == PromptAction::none);
    OA_CHECK(settings::prompt_key(one, DialogKey::escape).button == 0);
}

void test_pointer() {
    Prompt prompt = prompt_of({"CANCEL", "REPLACE"});
    const int32_t height = settings::prompt_height(prompt);
    const auto buttons = buttons_of(prompt);
    const auto& cancel = buttons[0].rect;
    const auto& replace = buttons[1].rect;
    const int32_t x = replace.x + replace.width / 2;
    const int32_t y = replace.y + replace.height / 2;
    OA_CHECK(settings::prompt_pointer_move(prompt, x, y, height).action == PromptAction::redraw);
    OA_CHECK(prompt.hovered == 1);
    OA_CHECK(settings::prompt_pointer_down(prompt, x, y, height).action == PromptAction::redraw);
    OA_CHECK(prompt.pressed == 1);
    const auto released = settings::prompt_pointer_up(prompt, x, y, height);
    OA_CHECK(released.action == PromptAction::answered && released.button == 1);
    // A release off the button pressed answers nothing.
    OA_CHECK(
        settings::prompt_pointer_down(prompt, cancel.x + 1, cancel.y + 1, height).action ==
        PromptAction::redraw
    );
    OA_CHECK(settings::prompt_pointer_up(prompt, x, y, height).action == PromptAction::redraw);
    // A press on nothing holds nothing.
    OA_CHECK(settings::prompt_pointer_down(prompt, 2, 2, height).action == PromptAction::none);
    OA_CHECK(settings::prompt_pointer_up(prompt, 2, 2, height).action == PromptAction::none);
    // A finger just under a button within reach takes it, and its release
    // where it landed answers it.
    const int32_t finger_y = replace.y + replace.height + 3;
    OA_CHECK(
        settings::prompt_finger_down(prompt, x, finger_y, height, 6).action == PromptAction::redraw
    );
    OA_CHECK(prompt.pressed == 1);
    const auto tapped = settings::prompt_pointer_up(prompt, x, finger_y, height);
    OA_CHECK(tapped.action == PromptAction::answered && tapped.button == 1);
    // Out of reach, nothing.
    OA_CHECK(
        settings::prompt_finger_down(prompt, x, finger_y, height, 2).action == PromptAction::none
    );
}

/// Tells whether a rectangle lies wholly in a box of a width and a height
/// from its top left corner.
///
/// @param rect the rectangle
/// @param width the box's width
/// @param height the box's height
/// @return true when no part of it lies outside
bool inside_box(const oa::ui::frontend_renderer::SourceRect& rect, int32_t width, int32_t height) {
    return rect.x >= 0 && rect.y >= 0 && rect.x + rect.width <= width &&
           rect.y + rect.height <= height;
}

void test_size_classes() {
    namespace kit = oa::ui::kit;
    std::size_t compact_lines = 0;
    for (const auto size_class :
         {kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large}) {
        const kit::Metrics& sized = kit::metrics_of(size_class);
        // A prompt of three buttons, right-aligned at the class's width with
        // Settings' buttons at the class's sizes, and its text as wide as the
        // class lets it be.
        Prompt prompt = prompt_of({"CANCEL", "INSTALL ALONGSIDE", "REPLACE"});
        prompt.size_class = size_class;
        prompt.progress = 250;
        const kit::PlacedQuestion placed = kit::place_question(prompt, nullptr);
        const int32_t height = settings::prompt_height(prompt);
        OA_CHECK(height == placed.height && height > 0);
        OA_CHECK(height <= sized.greatest_notice_height);
        OA_CHECK(settings::prompt_fits(prompt));
        Prompt compact = prompt;
        compact.size_class = kit::SizeClass::compact;
        const auto compact_buttons = buttons_of(compact);
        const auto buttons = buttons_of(prompt);
        OA_CHECK(buttons.size() == 3 && compact_buttons.size() == 3);
        OA_CHECK(
            buttons.back().rect.x + buttons.back().rect.width ==
            placed.width - kit::compact_metrics.padding
        );
        for (std::size_t index = 0; index < buttons.size() && index < compact_buttons.size();
             ++index) {
            OA_CHECK(buttons[index].rect.width >= compact_buttons[index].rect.width);
            OA_CHECK(buttons[index].rect.width >= 2 * sized.control_height);
            OA_CHECK(buttons[index].rect.height == sized.control_height);
            if (index + 1 < buttons.size())
                OA_CHECK(
                    buttons[index].rect.x + buttons[index].rect.width + 2 * sized.space ==
                    buttons[index + 1].rect.x
                );
        }
        for (const auto& part : settings::prompt_layout(prompt)) {
            OA_CHECK(inside_box(part.rect, placed.width, height));
            if (part.control == settings::no_control && !part.text.empty() &&
                part.text != prompt.title)
                OA_CHECK(part.rect.width == placed.width - 2 * kit::compact_metrics.padding);
        }
        // A text longer than the class's greatest height is cut there.
        Prompt long_text = prompt;
        for (int paragraph = 0; paragraph < 60; ++paragraph)
            long_text.paragraphs.push_back(
                {"A line of the prompt's text that takes a row.", false}
            );
        const int32_t long_width = kit::place_question(long_text, nullptr).width;
        OA_CHECK(!settings::prompt_fits(long_text));
        OA_CHECK(settings::prompt_height(long_text) == sized.greatest_notice_height);
        for (const auto& part : settings::prompt_layout(long_text))
            OA_CHECK(inside_box(part.rect, long_width, sized.greatest_notice_height));
        // A notice: its text wraps at the class's width, its buttons keep
        // their sizes at its footer's right, and every part lies inside it.
        settings::Notice notice{};
        notice.title = "SAVED GAMES";
        notice.paragraphs.push_back(
            {"Saved games, screenshots and films now go to your own folder, which this game "
             "keeps for you whatever folder the game itself is in, so that nothing you make is "
             "lost when the game is moved or put back.",
             false}
        );
        notice.paragraphs.push_back({"/Users/someone/Documents/Open Annihilation", true});
        notice.open_caption = "OPEN FOLDER";
        notice.size_class = size_class;
        const int32_t notice_height = settings::notice_height(notice);
        OA_CHECK(notice_height >= sized.least_notice_height);
        OA_CHECK(notice_height <= sized.greatest_notice_height);
        std::size_t lines = 0;
        for (const auto& part : settings::notice_layout(notice)) {
            OA_CHECK(inside_box(part.rect, sized.notice_width, notice_height));
            if (part.control == settings::notice_ok_control) {
                OA_CHECK(part.rect.x + part.rect.width == sized.notice_width - 12);
                OA_CHECK(part.rect.width == 52 && part.rect.height == 17);
            }
            if (part.control == settings::no_control && part.rect.y > 26 && !part.text.empty())
                ++lines;
        }
        // A wider class breaks the text into fewer lines.
        if (size_class == kit::SizeClass::compact)
            compact_lines = lines;
        else
            OA_CHECK(lines < compact_lines);
    }
}

} // namespace

int main() {
    test_layout();
    test_keys();
    test_pointer();
    test_size_classes();
    return oa::test::check_exit_status();
}
