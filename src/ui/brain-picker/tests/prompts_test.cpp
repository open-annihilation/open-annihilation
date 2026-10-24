// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// "Load with the built-in player?": one missing brain and two, the buttons,
// the texts fitting at every class, and a lookup that translates.

#include "oa/test/check.hpp"
#include "oa/ui/brain_picker/prompts.hpp"
#include "oa/ui/brain_picker/text.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/library/text.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

namespace picker = oa::ui::brain_picker;
namespace kit = oa::ui::kit;
namespace library = oa::ui::library;

/// The paragraph texts, in order.
///
/// @param question the question
/// @return the texts
std::vector<std::string> paragraphs_of(const kit::Question& question) {
    std::vector<std::string> texts;
    for (const kit::Paragraph& paragraph : question.paragraphs)
        texts.push_back(paragraph.text);
    return texts;
}

/// The buttons' names to automation.
///
/// @param question the question
/// @return the names, including the body
std::vector<std::string> names_of(const kit::Question& question) {
    const kit::DisplayList list = kit::question_list(question, nullptr);
    std::vector<std::string> names;
    for (const kit::Control& control : list.controls)
        names.push_back(control.name);
    return names;
}

/// Requires the question's text to fit at every class, within that class's height.
///
/// @param question the question
void fits(const kit::Question& question) {
    for (const kit::SizeClass size_class :
         {kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large}) {
        kit::Question sized = question;
        sized.size_class = size_class;
        const kit::PlacedQuestion placed = kit::place_question(sized, nullptr);
        OA_CHECK(!placed.cut);
        OA_CHECK(placed.height <= kit::metrics_of(size_class).greatest_notice_height);
    }
}

/// One missing brain, and two.
void load_with_the_built_in_player() {
    const picker::MissingBrain one_brain{"Steady Commander", "1.2", 1};
    const kit::Question one =
        picker::load_built_in_question(std::span<const picker::MissingBrain>(&one_brain, 1));
    OA_CHECK(one.title == "Load with the built-in player?");
    OA_CHECK(
        paragraphs_of(one) ==
        std::vector<std::string>(
            {"This game was saved with Steady Commander 1.2 playing seat 2, which is not "
             "installed here.",
             "The built-in player takes those seats from the units as they stand. GET the brain "
             "in the Library to load the game as it was saved."}
        )
    );
    OA_CHECK(one.buttons.size() == 2);
    OA_CHECK(
        one.buttons[0].caption == "BACK" && one.buttons[0].id == "back" && !one.buttons[0].accent
    );
    OA_CHECK(
        one.buttons[1].caption == "LOAD WITH THE BUILT-IN PLAYER" &&
        one.buttons[1].id == "load-built-in" && one.buttons[1].accent
    );
    OA_CHECK(one.cancel_button == 0 && one.primary_button == 1 && one.marked == 1);
    OA_CHECK(kit::question_word_of(one) == "prompt");
    OA_CHECK(
        names_of(one) ==
        std::vector<std::string>({"prompt.back", "prompt.load-built-in", "prompt.body"})
    );
    fits(one);

    const picker::MissingBrain other{"Ridge Marshal", "3", 0};
    const picker::MissingBrain both[] = {other, one_brain};
    const kit::Question two = picker::load_built_in_question(both);
    const std::vector<std::string> paragraphs = paragraphs_of(two);
    OA_CHECK(paragraphs.size() == 3);
    OA_CHECK(
        paragraphs[0] ==
        "This game was saved with Ridge Marshal 3 playing seat 1, which is not installed here."
    );
    OA_CHECK(paragraphs[1].find("Steady Commander 1.2") != std::string::npos);
    OA_CHECK(paragraphs[1].find("seat 2") != std::string::npos);
    OA_CHECK(paragraphs[2].find("GET the brain in the Library") != std::string::npos);
    OA_CHECK(two.buttons[0].id == picker::back_id && two.cancel_button == 0);
    OA_CHECK(two.buttons[1].id == picker::load_built_in_id && two.primary_button == 1);
    OA_CHECK(two.marked == 1);
    fits(two);
}

/// A lookup that translates the title and leaves the places filled.
void translated() {
    const auto shown = [](void*, std::string_view english) -> std::string_view {
        if (english == picker::words::load_built_in_title)
            return "Mit dem eingebauten Spieler laden?";
        return english;
    };
    library::TextHooks hooks{nullptr, shown};
    const picker::MissingBrain one_brain{"Steady Commander", "1.2", 1};
    const kit::Question question =
        picker::load_built_in_question(std::span<const picker::MissingBrain>(&one_brain, 1), hooks);
    OA_CHECK(question.title == "Mit dem eingebauten Spieler laden?");
    OA_CHECK(question.buttons[0].caption == "BACK");
    OA_CHECK(paragraphs_of(question)[0].find("Steady Commander") != std::string::npos);
    OA_CHECK(paragraphs_of(question)[0].find("seat 2") != std::string::npos);
}

} // namespace

int main() {
    load_with_the_built_in_player();
    translated();
    return oa::test::check_exit_status();
}
